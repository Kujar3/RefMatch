#include "AllocationTracking.h"
#include "core/Loudness.h"
#include "plugin/PluginProcessor.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

// Processor integration tests (owner: processor agent). Print one line per check; return 0 when all pass.
//
// The tests drive RefMatchProcessor like a host: prepareToPlay, a fake AudioPlayHead, processBlock, getState /
// setState. Reference files are written to a temporary folder of their own (unique per run, so concurrent runs do
// not interfere) with juce::WavAudioFormat (32-bit float, so samples round-trip exactly and playback can be compared
// bit for bit). Every processBlock call runs with allocation tracking on (AllocationTracking.h): operator new /
// delete in all forms and, through this executable's imports, the CRT malloc / calloc / realloc / free that JUCE's
// HeapBlock uses. A canary check proves the counter sees both before the processor tests run.

namespace
{
//==============================================================================
int numPassed = 0;
int numFailed = 0;

void check (bool ok, const std::string& name, const std::string& detail = {})
{
    if (ok)
    {
        ++numPassed;
        std::cout << "PASS " << name << "\n";
    }
    else
    {
        ++numFailed;
        std::cout << "FAIL " << name << (detail.empty() ? std::string() : ": " + detail) << "\n";
    }
}

template <typename... Args>
std::string str (Args&&... args)
{
    std::ostringstream ss;
    (ss << ... << args);
    return ss.str();
}

constexpr double kRate = 48000.0;
const int kJumpFade = (int) std::lround (RefMatchProcessor::kJumpFadeMs * kRate / 1000.0);       // 384
const int kListenFade = (int) std::lround (RefMatchProcessor::kListenFadeMs * kRate / 1000.0);   // 1440
const int kMonoFade = (int) std::lround (RefMatchProcessor::kMonoFadeMs * kRate / 1000.0);       // 960

long long floorMod (long long v, long long m)
{
    const auto r = v % m;
    return r < 0 ? r + m : r;
}

//==============================================================================
// Signals and test files
struct Lcg
{
    juce::uint32 state;
    float next() noexcept
    {
        state = state * 1664525u + 1013904223u;
        return (float) ((double) (state >> 8) / 8388608.0 - 1.0);   // -1 .. 1
    }
};

using Generator = std::function<float (int channel, long long n)>;

Generator sine (double freqL, double ampL, double freqR, double ampR, double sampleRate)
{
    return [=] (int ch, long long n)
    {
        const double f = ch == 0 ? freqL : freqR, a = ch == 0 ? ampL : ampR;
        return (float) (a * std::sin (juce::MathConstants<double>::twoPi * f * (double) n / sampleRate));
    };
}

struct TestTrack
{
    juce::File file;
    double sampleRate = 0.0;
    std::vector<float> left, right;   // as written (right == left for mono files)

    long long size() const noexcept { return (long long) left.size(); }
};

juce::File tempDir;

TestTrack writeTrackFile (const juce::File& file, double sampleRate, int channels, long long numSamples,
                          const Generator& gen)
{
    TestTrack t;
    t.file = file;
    t.file.getParentDirectory().createDirectory();
    t.sampleRate = sampleRate;
    t.left.resize ((size_t) numSamples);
    t.right.resize ((size_t) numSamples);

    juce::AudioBuffer<float> buffer (channels, (int) numSamples);

    for (long long i = 0; i < numSamples; ++i)
    {
        t.left[(size_t) i] = gen (0, i);
        t.right[(size_t) i] = channels > 1 ? gen (1, i) : t.left[(size_t) i];
        buffer.setSample (0, (int) i, t.left[(size_t) i]);
        if (channels > 1)
            buffer.setSample (1, (int) i, t.right[(size_t) i]);
    }

    t.file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream (new juce::FileOutputStream (t.file));
    juce::WavAudioFormat wav;
    const auto options = juce::AudioFormatWriterOptions()
                             .withSampleRate (sampleRate)
                             .withNumChannels (channels)
                             .withBitsPerSample (32)
                             .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);

    if (auto writer = wav.createWriterFor (stream, options))
        writer->writeFromAudioSampleBuffer (buffer, 0, (int) numSamples);
    else
        std::cout << "could not write " << t.file.getFullPathName() << "\n";

    return t;
}

TestTrack writeTrack (const juce::String& name, double sampleRate, int channels, long long numSamples,
                      const Generator& gen)
{
    return writeTrackFile (tempDir.getChildFile (name), sampleRate, channels, numSamples, gen);
}

std::vector<float> noise (long long n, juce::uint32 seed, float amp)
{
    Lcg lcg { seed };
    std::vector<float> v ((size_t) n);
    for (auto& x : v)
        x = amp * lcg.next();
    return v;
}

//==============================================================================
// Fake host
struct FakePlayHead final : juce::AudioPlayHead
{
    enum class TimeMode { samples, seconds, ppq };

    bool playing = false;
    long long time = 0;           // host position in samples
    TimeMode mode = TimeMode::samples;
    double sampleRate = kRate;
    double bpm = 120.0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (playing);
        info.setBpm (bpm);

        if (mode == TimeMode::samples)
            info.setTimeInSamples ((juce::int64) time);
        else if (mode == TimeMode::seconds)
            info.setTimeInSeconds ((double) time / sampleRate);
        else
            info.setPpqPosition ((double) time / sampleRate * bpm / 60.0);

        return info;
    }
};

struct Output
{
    std::vector<float> l, r;
    std::vector<long long> time;   // host time of each sample (-1 when stopped)
};

struct Host
{
    explicit Host (double rate = kRate, int block = 512, int channels = 2)
        : sampleRate (rate), blockSize (block), numChannels (channels)
    {
        if (channels == 1)
        {
            juce::AudioProcessor::BusesLayout layout;
            layout.inputBuses.add (juce::AudioChannelSet::mono());
            layout.outputBuses.add (juce::AudioChannelSet::mono());
            proc.setBusesLayout (layout);
        }

        head.sampleRate = rate;
        proc.setPlayHead (&head);
    }

    ~Host() { proc.releaseResources(); }

    // Prepares like a host; by default also waits for a re-load at the new rate to finish.
    void prepare (bool waitForReference = true)
    {
        proc.prepareToPlay (sampleRate, blockSize);
        if (waitForReference)
            proc.waitUntilReferenceIdle (30000);
    }

    void set (const char* id, float plainValue)
    {
        if (auto* p = proc.getAPVTS().getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
    }

    float get (const char* id)
    {
        auto* v = proc.getAPVTS().getRawParameterValue (id);
        return v != nullptr ? v->load() : std::nanf ("");
    }

    bool load (const juce::File& f, int timeoutMs = 30000)
    {
        proc.loadReference (f);
        return proc.waitUntilReferenceIdle (timeoutMs);
    }

    void processBlock (juce::AudioBuffer<float>& buffer)
    {
        rmdev::setAllocationTracking (true);
        proc.processBlock (buffer, midi);
        rmdev::setAllocationTracking (false);

        if (head.playing)
            head.time += buffer.getNumSamples();
    }

    using InputFn = std::function<void (long long globalIndex, float& l, float& r)>;

    // Streams numSamples through the processor. Block sizes cycle through `sizes`; `before (i)` runs before each block
    // with the index (relative to this call) of the block's first sample.
    Output stream (long long numSamples, const InputFn& input = {}, std::vector<int> sizes = {},
                   const std::function<void (long long)>& before = {})
    {
        if (sizes.empty())
            sizes = { blockSize };

        Output out;
        out.l.reserve ((size_t) numSamples);
        out.r.reserve ((size_t) numSamples);
        out.time.reserve ((size_t) numSamples);

        int maxSize = 1;
        for (auto s : sizes)
            maxSize = juce::jmax (maxSize, s);

        juce::AudioBuffer<float> buffer (numChannels, maxSize);
        long long done = 0;
        size_t next = 0;

        while (done < numSamples)
        {
            if (before)
                before (done);

            const int n = (int) std::min<long long> (sizes[next++ % sizes.size()], numSamples - done);
            buffer.setSize (numChannels, n, false, false, true);

            for (int i = 0; i < n; ++i)
            {
                float l = 0.0f, r = 0.0f;
                if (input)
                    input (processed + i, l, r);
                buffer.setSample (0, i, l);
                if (numChannels > 1)
                    buffer.setSample (1, i, r);
            }

            const long long blockTime = head.playing ? head.time : -1;
            processBlock (buffer);

            for (int i = 0; i < n; ++i)
            {
                out.l.push_back (buffer.getSample (0, i));
                out.r.push_back (buffer.getSample (numChannels > 1 ? 1 : 0, i));
                out.time.push_back (blockTime >= 0 ? blockTime + i : -1);
            }

            done += n;
            processed += n;
        }

        return out;
    }

    RefMatchProcessor proc;
    FakePlayHead head;
    juce::MidiBuffer midi;
    double sampleRate;
    int blockSize;
    int numChannels;
    long long processed = 0;
};

//==============================================================================
// Output checks
float maxStep (const std::vector<float>& x, size_t from = 0, size_t to = (size_t) -1)
{
    float m = 0.0f;
    to = juce::jmin (to, x.size());
    for (size_t i = from + 1; i < to; ++i)
        m = juce::jmax (m, std::abs (x[i] - x[i - 1]));
    return m;
}

float maxAbs (const std::vector<float>& x, size_t from = 0, size_t to = (size_t) -1)
{
    float m = 0.0f;
    to = juce::jmin (to, x.size());
    for (size_t i = from; i < to; ++i)
        m = juce::jmax (m, std::abs (x[i]));
    return m;
}

bool allFinite (const Output& o)
{
    for (size_t i = 0; i < o.l.size(); ++i)
        if (! std::isfinite (o.l[i]) || ! std::isfinite (o.r[i]))
            return false;
    return true;
}

// Checks out[i] == ref[posAt (i)] (both channels, bit-exact, `gain` applied) for i in [from, to), skipping the
// crossfade around every discontinuity of posAt (posAt (i) != posAt (i - 1) + 1): `fade` samples after it, or, for a
// loop wrap at the end of the file (no continuation to fade from), `fade` samples on both sides. Returns "" or a
// description.
std::string checkFollows (const Output& out, const TestTrack& ref, long long from, long long to,
                          const std::function<long long (long long)>& posAt, int fade = kJumpFade, float gain = 1.0f)
{
    std::vector<char> skip ((size_t) juce::jmax (0LL, to - from), 0);

    for (long long i = from + 1; i < to; ++i)
    {
        const long long prev = posAt (i - 1);
        if (posAt (i) == prev + 1)
            continue;

        const bool atFileEnd = prev + 1 + fade > ref.size();
        for (long long k = juce::jmax (from, atFileEnd ? i - fade : i); k < juce::jmin (to, i + fade); ++k)
            skip[(size_t) (k - from)] = 1;
    }

    long long checked = 0;

    for (long long i = from; i < to; ++i)
    {
        const long long pos = posAt (i);

        if (skip[(size_t) (i - from)] != 0)
            continue;

        const float wantL = pos >= 0 && pos < ref.size() ? ref.left[(size_t) pos] * gain : 0.0f;
        const float wantR = pos >= 0 && pos < ref.size() ? ref.right[(size_t) pos] * gain : 0.0f;

        if (out.l[(size_t) i] != wantL || out.r[(size_t) i] != wantR)
            return str ("sample ", i, " (ref pos ", pos, "): got ", out.l[(size_t) i], "/", out.r[(size_t) i],
                        " want ", wantL, "/", wantR);
        ++checked;
    }

    return checked > 0 ? std::string() : std::string ("nothing checked");
}

// Integrated loudness of a stretch of output.
float outputLufs (const Output& o, size_t from, size_t to, double rate = kRate)
{
    return rm::measureIntegratedLufs (o.l.data() + from, o.r.data() + from, (long long) (to - from), rate);
}

// Frequency of a sine from its positive-going zero crossings (linear interpolation between samples).
double measureFrequency (const float* x, long long n, double rate)
{
    double first = -1.0, last = -1.0;
    int crossings = 0;

    for (long long i = 1; i < n; ++i)
    {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = (double) (i - 1) + (double) -x[i - 1] / (double) (x[i] - x[i - 1]);
            if (first < 0.0)
                first = t;
            last = t;
            ++crossings;
        }
    }

    return crossings > 1 ? (double) (crossings - 1) * rate / (last - first) : 0.0;
}

//==============================================================================
void testLoad()
{
    std::cout << "\n-- Load\n";

    {
        Host h;
        h.prepare();
        const auto tone = writeTrack ("tone44k.wav", 44100.0, 2, 88200, sine (1000.0, 0.5, 15000.0, 0.5, 44100.0));
        h.proc.loadReference (tone.file);
        check (h.proc.getReferenceInfo().state == RefMatchProcessor::ReferenceState::Loading, "load: Loading right after loadReference");
        check (h.proc.waitUntilReferenceIdle (30000), "load: loader finishes");

        const auto info = h.proc.getReferenceInfo();
        const auto track = h.proc.getReferenceTrack();
        check (info.state == RefMatchProcessor::ReferenceState::Ready, "load: 44.1 kHz WAV -> Ready", info.error.toStdString());
        check (info.fileSampleRate == 44100.0 && info.fileChannels == 2, "load: file rate / channels reported",
               str (info.fileSampleRate, " ", info.fileChannels));
        check (info.displayName == "tone44k" && info.file == tone.file, "load: name and file");
        check (std::abs (info.lengthSeconds - 2.0) < 1.0e-3 && ! info.hasLoop && info.loopEnd == info.lengthSeconds,
               "load: length 2 s, whole-file region", str (info.lengthSeconds));

        if (track != nullptr)
        {
            check (track->sampleRate == kRate && track->numSamples() == 96000 && track->audio.getNumChannels() == 2,
                   "load: resampled to 48 kHz with the right length", str (track->sampleRate, " ", track->numSamples()));

            const float* l = track->audio.getReadPointer (0);
            const float* r = track->audio.getReadPointer (1);
            const double freq = measureFrequency (l + 2000, 90000, kRate);
            check (std::abs (freq - 1000.0) < 0.05, "load: resampled 1 kHz tone keeps its frequency", str (freq, " Hz"));

            // Against the ideal continuous sines (away from the file edges).
            double errL = 0.0, errR = 0.0;
            for (int n = 2000; n < 94000; ++n)
            {
                const double t = (double) n / kRate;
                errL = std::max (errL, std::abs (l[n] - 0.5 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * t)));
                errR = std::max (errR, std::abs (r[n] - 0.5 * std::sin (juce::MathConstants<double>::twoPi * 15000.0 * t)));
            }
            check (errL < 1.0e-3 && errR < 2.0e-3, "load: 44.1 -> 48 kHz resampling is clean (1 kHz and 15 kHz)",
                   str ("max error ", errL, " / ", errR));

            const auto coreCurve = rm::shortTermLoudnessCurve (l, r, track->numSamples(), kRate);
            check (coreCurve == track->shortTermCurve && track->monoShortTermCurve.size() == coreCurve.size(),
                   "load: short-term curve identical to rm::shortTermLoudnessCurve (+ mono fold-down curve)");

            rm::RegionAnalysis region;
            const bool done = rm::analyseReferenceRegion (*track, 0.5, 1.5, region, {});
            const auto core = rm::analyseReferenceRegion (*track, 0.5, 1.5);
            bool same = done && core.valid == region.profile.valid && core.integratedLufs == region.profile.integratedLufs
                        && core.truePeakDb == region.profile.truePeakDb && core.loudnessRangeLu == region.profile.loudnessRangeLu
                        && core.spectralLoudnessDb == region.profile.spectralLoudnessDb;
            for (int b = 0; b < rm::kNumBands; ++b)
                same = same && core.bands[(size_t) b].levelDb == region.profile.bands[(size_t) b].levelDb
                       && core.bands[(size_t) b].widthPct == region.profile.bands[(size_t) b].widthPct;
            check (same, "load: cancellable region analysis identical to rm::analyseRegion",
                   str (core.integratedLufs, " vs ", region.profile.integratedLufs));

            int polls = 0;
            check (! rm::analyseReferenceRegion (*track, 0.0, 0.0, region, [&] { return ++polls > 1; }),
                   "load: region analysis can be cancelled");

            check (track->overview.minimum.size() == (size_t) rm::ReferenceTrack::kOverviewColumns
                       && track->overview.maximum.size() == (size_t) rm::ReferenceTrack::kOverviewColumns,
                   "load: overview has 2048 columns");
            check (track->shortTermCurve.size() >= 19 && track->shortTermCurve.size() <= 21,
                   "load: short-term curve every 100 ms", str (track->shortTermCurve.size(), " values"));
        }
        else
        {
            check (false, "load: track available");
        }

        // Downsampling must be band-limited: a 30 kHz component of a 96 kHz file disappears at 48 kHz.
        const auto hi = writeTrack ("tone96k.wav", 96000.0, 2, 192000, [] (int ch, long long n)
        {
            const double t = (double) n / 96000.0;
            const double ultra = 0.25 * std::sin (juce::MathConstants<double>::twoPi * 30000.0 * t);
            return (float) (ch == 0 ? 0.5 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * t) + ultra : ultra);
        });
        check (h.load (hi.file), "load: 96 kHz file loads");

        if (const auto t96 = h.proc.getReferenceTrack())
        {
            const float* l = t96->audio.getReadPointer (0);
            const float* r = t96->audio.getReadPointer (1);
            double err = 0.0, rms = 0.0;
            for (int n = 2000; n < 94000; ++n)
            {
                err = std::max (err, std::abs (l[n] - 0.5 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * n / kRate)));
                rms += (double) r[n] * r[n];
            }
            rms = std::sqrt (rms / 92000.0);
            check (t96->numSamples() == 96000 && err < 1.0e-3 && rms < 0.25 * 1.0e-4,
                   "load: 96 -> 48 kHz removes content above the new Nyquist (< -80 dB)",
                   str ("len ", t96->numSamples(), " err ", err, " ultrasonic rms ", rms));
        }
        else
        {
            check (false, "load: 96 kHz track available");
        }
    }

    {
        Host h;
        h.prepare();
        const auto mono = writeTrack ("mono48k.wav", kRate, 1, 48000, sine (440.0, 0.5, 440.0, 0.5, kRate));
        check (h.load (mono.file), "load: mono file loads");
        const auto info = h.proc.getReferenceInfo();
        const auto track = h.proc.getReferenceTrack();
        bool same = track != nullptr && track->numSamples() == mono.size();
        for (long long i = 0; same && i < mono.size(); ++i)
            same = track->audio.getSample (0, (int) i) == mono.left[(size_t) i]
                   && track->audio.getSample (1, (int) i) == mono.left[(size_t) i];
        check (info.state == RefMatchProcessor::ReferenceState::Ready && info.fileChannels == 1 && same,
               "load: mono file duplicated to both channels, sample-exact at equal rates");

        if (track != nullptr)
        {
            rm::RegionAnalysis region;
            rm::analyseReferenceRegion (*track, 0.0, 0.0, region, {});
            check (std::abs (region.profile.integratedLufs - region.monoIntegratedLufs - 3.0103f) < 0.02f,
                   "load: mono fold-down loudness measured (identical channels: -3.01 dB)",
                   str (region.profile.integratedLufs, " / ", region.monoIntegratedLufs));
        }

        const auto missing = tempDir.getChildFile ("missing.wav");
        check (h.load (missing), "load: missing file finishes");
        const auto mi = h.proc.getReferenceInfo();
        check (mi.state == RefMatchProcessor::ReferenceState::Error && mi.error.contains ("File not found")
                   && mi.error.contains ("missing.wav") && mi.file == missing,
               "load: missing file -> Error with message, path kept", mi.error.toStdString());
        check (h.proc.getReferenceTrack() == nullptr, "load: no track after an error");

        const auto garbage = tempDir.getChildFile ("garbage.wav");
        garbage.replaceWithText ("this is not a wave file at all, just some text pretending to be one");
        check (h.load (garbage), "load: damaged file finishes");
        const auto gi = h.proc.getReferenceInfo();
        check (gi.state == RefMatchProcessor::ReferenceState::Error && gi.error.contains ("Unsupported"),
               "load: damaged file -> Error 'Unsupported or damaged'", gi.error.toStdString());

        const auto tiny = writeTrack ("tiny.wav", kRate, 2, 14400, sine (440.0, 0.5, 440.0, 0.5, kRate));
        check (h.load (tiny.file) && h.proc.getReferenceInfo().error.contains ("shorter"),
               "load: file shorter than half a second -> Error", h.proc.getReferenceInfo().error.toStdString());

        const auto text = tempDir.getChildFile ("notes.txt");
        text.replaceWithText ("hello");
        check (h.load (text) && h.proc.getReferenceInfo().state == RefMatchProcessor::ReferenceState::Error,
               "load: unsupported file type -> Error");

        check (RefMatchProcessor::isSupportedAudioFile (juce::File ("C:\\a\\b.FLAC"))
                   && RefMatchProcessor::isSupportedAudioFile (juce::File ("C:\\a\\b.mp3"))
                   && ! RefMatchProcessor::isSupportedAudioFile (juce::File ("C:\\a\\b.txt")),
               "load: isSupportedAudioFile");
    }

    {
        // A damaged 32-bit float file with NaN / inf samples: they become silence at load (resampled or not), so
        // they can never reach the host's output; the samples around them are untouched.
        constexpr float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
        const auto bad = writeTrack ("nonfinite.wav", 44100.0, 2, 44100, [&] (int ch, long long i)
        {
            if (i == 20000)
                return ch == 0 ? nan : inf;
            if (i == 30000)
                return -inf;
            return (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 440.0 * (double) i / 44100.0));
        });

        juce::AudioFormatManager formats;
        rm::registerReferenceFormats (formats);
        bool written = false;
        if (std::unique_ptr<juce::AudioFormatReader> reader { formats.createReaderFor (bad.file) })
        {
            juce::AudioBuffer<float> raw (2, 44100);
            reader->read (&raw, 0, 44100, 0, true, true);
            written = std::isnan (raw.getSample (0, 20000)) && std::isinf (raw.getSample (1, 20000))
                      && std::isinf (raw.getSample (0, 30000));
        }
        check (written, "load: test file really contains NaN / inf samples");

        for (const double rate : { kRate, 44100.0 })
        {
            juce::String error;
            const auto track = rm::loadReferenceTrack (bad.file, rate, error);
            bool finite = track != nullptr && track->numSamples() > 0;

            for (int ch = 0; finite && ch < 2; ++ch)
                for (long long i = 0; finite && i < track->numSamples(); ++i)
                    finite = std::isfinite (track->audio.getSample (ch, (int) i));

            check (finite, str ("load: NaN / inf samples become silence (", rate == 44100.0 ? "same rate" : "resampled", ")"),
                   error.toStdString());

            if (track != nullptr && rate == 44100.0)
                check (track->audio.getSample (0, 20000) == 0.0f && track->audio.getSample (1, 20000) == 0.0f
                           && track->audio.getSample (0, 30000) == 0.0f
                           && track->audio.getSample (0, 19999) == bad.left[19999]
                           && track->audio.getSample (0, 20001) == bad.left[20001]
                           && track->audio.getSample (1, 29999) == bad.right[29999],
                       "load: only the bad samples are replaced (neighbours bit-exact)");
        }

        // Mostly damaged (> 1 % of the samples): not usable as a reference.
        const auto broken = writeTrack ("mostly nan.wav", 44100.0, 2, 44100, [&] (int, long long i)
        {
            return i % 20 == 0 ? nan : 0.25f;
        });
        juce::String error;
        check (rm::loadReferenceTrack (broken.file, kRate, error) == nullptr && error.contains ("damaged"),
               "load: a file with many NaN samples -> 'Unsupported or damaged'", error.toStdString());
    }

    // Replace the reference while it plays, then clear it.
    {
        Host h (kRate, 480);
        h.set (RefMatchProcessor::kParamMatch, 0.0f);
        h.set (RefMatchProcessor::kParamListen, 1.0f);
        const auto a = writeTrack ("refA.wav", kRate, 2, 3 * 48000, sine (300.0, 0.4, 300.0, 0.4, kRate));
        const auto b = writeTrack ("refB.wav", kRate, 2, 3 * 48000, sine (500.0, 0.4, 500.0, 0.4, kRate));
        check (h.load (a.file), "replace: first reference loads");
        h.prepare();

        auto out = h.stream (24000);
        h.proc.loadReference (b.file);
        int guard = 0;
        while (! h.proc.waitUntilReferenceIdle (0) && guard++ < 2000)
        {
            auto more = h.stream (480);
            out.l.insert (out.l.end(), more.l.begin(), more.l.end());
            juce::Thread::sleep (1);
        }
        auto tail = h.stream (9600);
        out.l.insert (out.l.end(), tail.l.begin(), tail.l.end());

        const float stepB = 0.4f * (float) (juce::MathConstants<double>::twoPi * 500.0 / kRate);
        check (h.proc.getReferenceInfo().displayName == "refB", "replace: new reference is Ready");
        check (maxStep (out.l) < 1.6f * stepB, "replace: swapping references while playing does not click",
               str ("max step ", maxStep (out.l), " limit ", 1.6f * stepB));

        // The last samples are B at the reported playhead.
        const long long end = (long long) std::llround (h.proc.getReferencePlayheadSeconds() * kRate);
        bool match = end >= 200;
        for (long long k = 1; match && k <= 200; ++k)
            match = tail.l[tail.l.size() - (size_t) k] == b.left[(size_t) (end - k)];
        check (match, "replace: output continues sample-exact on the new reference", str ("playhead ", end));

        // Load A again and clear it right after the hand-over, while B -> A is still fading: no cut.
        h.proc.loadReference (a.file);
        Output handOff;
        for (int guard2 = 0; guard2 < 4000 && h.proc.getReferenceInfo().state != RefMatchProcessor::ReferenceState::Ready; ++guard2)
        {
            auto more = h.stream (64, {}, { 64 });
            handOff.l.insert (handOff.l.end(), more.l.begin(), more.l.end());
            juce::Thread::sleep (1);
        }
        auto swap = h.stream (64, {}, { 64 });   // the audio thread picks up the new track: fade B -> A starts
        handOff.l.insert (handOff.l.end(), swap.l.begin(), swap.l.end());
        h.proc.clearReference();                  // while that fade runs
        auto after = h.stream (4800, {}, { 64 });
        handOff.l.insert (handOff.l.end(), after.l.begin(), after.l.end());
        check (maxStep (handOff.l) < 1.6f * stepB && maxAbs (after.l, (size_t) kJumpFade) == 0.0f,
               "replace: a second hand-over during a fade (load + clear) still fades, no cut",
               str ("max step ", maxStep (handOff.l), " limit ", 1.6f * stepB));

        // Several hand-overs while the host makes no audio callback: the track that was playing still fades out.
        h.proc.loadReference (a.file);
        h.proc.waitUntilReferenceIdle (20000);
        auto beforeBurst = h.stream (4900);   // ends mid-cycle (a cut would show)
        for (const auto* f : { &b.file, &a.file, &b.file, &a.file, &b.file })
        {
            h.proc.loadReference (*f);
            h.proc.waitUntilReferenceIdle (20000);
        }
        auto afterBurst = h.stream (4800);
        beforeBurst.l.insert (beforeBurst.l.end(), afterBurst.l.begin(), afterBurst.l.end());
        check (maxStep (beforeBurst.l) < 1.6f * stepB, "replace: five hand-overs without an audio callback still crossfade",
               str ("max step ", maxStep (beforeBurst.l), " limit ", 1.6f * stepB));

        h.proc.clearReference();
        const auto cleared = h.stream (4800);
        const auto ci = h.proc.getReferenceInfo();
        check (ci.state == RefMatchProcessor::ReferenceState::Empty && ci.file == juce::File()
                   && h.proc.getReferenceTrack() == nullptr,
               "clear: state Empty, no track");
        check (maxAbs (cleared.l, (size_t) kJumpFade + 1) == 0.0f && maxStep (cleared.l) < 1.6f * stepB,
               "clear: reference fades out to exact silence");
    }
}

//==============================================================================
void testSwitching()
{
    std::cout << "\n-- A/B switching\n";

    Host h (kRate, 256);
    h.set (RefMatchProcessor::kParamMatch, 0.0f);   // gain 0 dB
    const auto ref = writeTrack ("switch.wav", kRate, 2, 4 * 48000, sine (1000.0, 0.4, 1000.0, 0.4, kRate));
    check (h.load (ref.file), "switch: reference loads");
    h.prepare();
    h.head.playing = true;
    h.head.time = 0;

    auto mixInput = [] (long long n, float& l, float& r)
    {
        l = (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 220.0 * (double) n / kRate));
        r = (float) (0.45 * std::sin (juce::MathConstants<double>::twoPi * 220.0 * (double) n / kRate + 0.3));
    };

    const std::vector<long long> toggles { 4800, 5500, 6000, 30000, 60000, 90000 };   // ref, mix, ref, mix, ref, mix
    std::vector<long long> actual;
    bool listen = false;

    const auto out = h.stream (120000, mixInput, { 256, 37, 1024, 480, 1, 999, 64, 3000 }, [&] (long long i)
    {
        if (actual.size() < toggles.size() && i >= toggles[actual.size()])
        {
            listen = ! listen;
            h.set (RefMatchProcessor::kParamListen, listen ? 1.0f : 0.0f);
            actual.push_back (i);
        }
    });

    std::vector<float> mixL;
    for (long long n = 0; n < 120000; ++n)
    {
        float l = 0.0f, r = 0.0f;
        mixInput (n, l, r);
        mixL.push_back (l);
    }

    const float limit = 1.1f * juce::jmax (maxStep (mixL), maxStep (ref.left));
    check (maxStep (out.l) < limit && maxStep (out.r) < limit, "switch: no clicks while toggling A/B (also mid-fade)",
           str ("max step ", maxStep (out.l), " / ", maxStep (out.r), ", limit ", limit));
    check (allFinite (out), "switch: output finite");

    // Settled on the reference: exactly the reference (gain 0 dB), following the host position.
    auto hostPos = [&] (long long i) { return floorMod (out.time[(size_t) i], ref.size()); };
    check (checkFollows (out, ref, actual[4] + kListenFade, actual[5], hostPos).empty(),
           "switch: settled on Reference -> output == reference samples exactly",
           checkFollows (out, ref, actual[4] + kListenFade, actual[5], hostPos));

    // Settled on the mix: bit-exact input.
    bool exact = true;
    long long bad = -1;
    for (long long i = actual[3] + kListenFade; exact && i < actual[4]; ++i)
    {
        float l = 0.0f, r = 0.0f;
        mixInput (i, l, r);
        exact = out.l[(size_t) i] == l && out.r[(size_t) i] == r;
        bad = i;
    }
    for (long long i = actual[5] + kListenFade; exact && i < 120000; ++i)
    {
        float l = 0.0f, r = 0.0f;
        mixInput (i, l, r);
        exact = out.l[(size_t) i] == l && out.r[(size_t) i] == r;
        bad = i;
    }
    check (exact, "switch: switching back to Mix ends bit-exact with the input", str ("first mismatch at ", bad));
    check (h.proc.getListenPosition() == 0.0f, "switch: listen position back at 0");
}

//==============================================================================
void testPassthrough()
{
    std::cout << "\n-- Passthrough\n";

    Host h (kRate, 512);
    const auto ref = writeTrack ("pass.wav", kRate, 2, 48000, sine (700.0, 0.5, 900.0, 0.5, kRate));
    check (h.load (ref.file), "pass: reference loads");
    h.prepare();

    const auto in = noise (200000, 77u, 0.9f);
    auto input = [&] (long long n, float& l, float& r)
    {
        l = in[(size_t) (n % 100000)];
        r = in[(size_t) (n % 100000) + 100000];
        if (n % 1000 == 0)
            l = 1.0e-39f;   // denormal input must pass untouched as well
    };

    const long long start = h.processed;
    auto out = h.stream (48000, input, { 512, 100, 4096 });
    h.head.playing = true;
    auto out2 = h.stream (48000, input, { 333 });

    bool exact = true;
    for (size_t i = 0; exact && i < out.l.size() + out2.l.size(); ++i)
    {
        float l = 0.0f, r = 0.0f;
        input (start + (long long) i, l, r);
        const auto& o = i < out.l.size() ? out : out2;
        const size_t k = i < out.l.size() ? i : i - out.l.size();
        exact = std::memcmp (&o.l[k], &l, sizeof (float)) == 0 && std::memcmp (&o.r[k], &r, sizeof (float)) == 0;
    }
    check (exact, "pass: on Mix with mono off the output is bit-identical to the input");

    // Offline render: always the input, even on Reference.
    h.set (RefMatchProcessor::kParamListen, 1.0f);
    h.proc.setNonRealtime (true);
    const long long offlineStart = h.processed;
    const auto offline = h.stream (24000, input);
    exact = true;
    for (size_t i = 0; exact && i < offline.l.size(); ++i)
    {
        float l = 0.0f, r = 0.0f;
        input (offlineStart + (long long) i, l, r);
        exact = std::memcmp (&offline.l[i], &l, sizeof (float)) == 0 && std::memcmp (&offline.r[i], &r, sizeof (float)) == 0;
    }
    check (exact, "pass: isNonRealtime() -> untouched input even with Listen = Reference");

    h.proc.setNonRealtime (false);
    const auto back = h.stream (9600, input);
    check (maxAbs (back.l, 4000) > 0.0f && h.proc.getListenPosition() == 1.0f, "pass: realtime again -> reference audible");
    h.set (RefMatchProcessor::kParamListen, 0.0f);
}

//==============================================================================
void testGainMatching()
{
    std::cout << "\n-- Gain matching\n";

    // Mix ~ -20 LUFS, reference ~ -9 LUFS.
    const auto ref = writeTrack ("loud.wav", kRate, 2, 12 * 48000, sine (400.0, 0.45, 400.0, 0.35, kRate));
    auto mixInput = [] (long long n, float& l, float& r)
    {
        l = r = (float) (0.1 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) n / kRate));
    };

    std::vector<float> mixL, mixR;
    for (long long n = 0; n < 8 * 48000; ++n)
    {
        float l = 0.0f, r = 0.0f;
        mixInput (n, l, r);
        mixL.push_back (l);
        mixR.push_back (r);
    }
    const float mixLufs = rm::measureIntegratedLufs (mixL.data(), mixR.data(), (long long) mixL.size(), kRate);
    const float refLufs = rm::measureIntegratedLufs (ref.left.data(), ref.right.data(), ref.size(), kRate);
    check (rm::hasValue (mixLufs) && rm::hasValue (refLufs) && refLufs - mixLufs > 6.0f,
           "gain: test signals measured", str ("mix ", mixLufs, " LUFS, ref ", refLufs, " LUFS"));

    auto run = [&] (int mode, Output& out)
    {
        auto h = std::make_unique<Host> (kRate, 512);
        h->set (RefMatchProcessor::kParamMatch, (float) mode);
        h->set (RefMatchProcessor::kParamListen, 1.0f);
        h->load (ref.file);
        h->prepare();
        h->head.playing = true;
        out = h->stream (8 * 48000, mixInput, { 512 }, [&] (long long i)
        {
            if (i % 4608 == 0)
                h->proc.runAnalysisNow();
        });
        return h;
    };

    Output integrated, shortTerm, off;
    const auto hi = run (1, integrated);
    const float iLufs = outputLufs (integrated, 5 * 48000, 8 * 48000);
    check (std::abs (iLufs - mixLufs) < 0.5f, "gain: Integrated -> reference plays within 0.5 LU of the mix",
           str ("reference out ", iLufs, " LUFS, mix ", mixLufs, " LUFS, gain ", hi->proc.getMatchGainDb(), " dB"));

    const auto hs = run (2, shortTerm);
    const float sLufs = outputLufs (shortTerm, 5 * 48000, 8 * 48000);
    check (std::abs (sLufs - mixLufs) < 0.5f, "gain: Short-term -> reference plays within 0.5 LU of the mix",
           str ("reference out ", sLufs, " LUFS, gain ", hs->proc.getMatchGainDb(), " dB"));

    auto ho = run (0, off);
    const float oLufs = outputLufs (off, 5 * 48000, 8 * 48000);
    check (std::abs (oLufs - refLufs) < 0.1f && ho->proc.getMatchGainDb() == 0.0f,
           "gain: Off -> reference at its own level", str (oLufs, " vs ", refLufs, " LUFS"));

    // Switch Off -> Integrated while listening: the gain glides, no steps.
    ho->set (RefMatchProcessor::kParamMatch, 1.0f);
    const auto ramp = ho->stream (48000, mixInput, { 512 }, [&] (long long i)
    {
        if (i % 4608 == 0)
            ho->proc.runAnalysisNow();
    });

    double prevDb = 0.0, maxDelta = 0.0, firstDb = 0.0;
    long long prev = -1;
    for (size_t i = 0; i < ramp.l.size(); ++i)
    {
        const float want = ref.left[(size_t) floorMod (ramp.time[i], ref.size())];
        if (std::abs (want) < 0.1f)
            continue;

        const double db = 20.0 * std::log10 (std::abs ((double) ramp.l[i] / (double) want));
        if (prev >= 0)
            maxDelta = std::max (maxDelta, std::abs (db - prevDb) / (double) ((long long) i - prev));
        else
            firstDb = db;
        prevDb = db;
        prev = (long long) i;
    }
    check (std::abs (firstDb) < 0.01 && prevDb < -6.0 && maxDelta < 0.01,
           "gain: matching gain ramps smoothly (< 0.01 dB per sample)",
           str ("from ", firstDb, " dB to ", prevDb, " dB, max ", maxDelta, " dB/sample"));
}

//==============================================================================
void testTransport()
{
    std::cout << "\n-- Transport sync\n";

    const long long n = 3 * 48000;
    const auto nl = noise (n, 11u, 0.5f), nr = noise (n, 12u, 0.5f);
    const auto ref = writeTrack ("noise.wav", kRate, 2, n, [&] (int ch, long long i) { return ch == 0 ? nl[(size_t) i] : nr[(size_t) i]; });

    Host h (kRate, 333);
    h.set (RefMatchProcessor::kParamMatch, 0.0f);
    h.set (RefMatchProcessor::kParamListen, 1.0f);
    check (h.load (ref.file), "sync: reference loads");
    h.prepare();

    // 1. Host plays from t0: output = ref[(t0 + i) mod N]. (The track was re-loaded at the host rate by prepareToPlay,
    //    so the A/B switch fades from the mix to it once it is there.)
    h.head.playing = true;
    h.head.time = 12345;
    auto out = h.stream (24000);
    auto hostPos = [&] (const Output& o, long long rs, long long len, long long offset)
    {
        return [&o, rs, len, offset] (long long i) { return rs + floorMod (o.time[(size_t) i] + offset, len); };
    };
    {
        const auto err = checkFollows (out, ref, kListenFade, (long long) out.l.size(), hostPos (out, 0, n, 0));
        check (err.empty(), "sync: playing at t -> reference sample at (t mod length), exact", err);
    }

    // 2. Host jump.
    long long jumpAt = -1;
    out = h.stream (60000, {}, { 333 }, [&] (long long i)
    {
        if (jumpAt < 0 && i >= 10000)
        {
            h.head.time = 100000;
            jumpAt = i;
        }
    });
    {
        const auto err = checkFollows (out, ref, 0, (long long) out.l.size(), hostPos (out, 0, n, 0));
        check (err.empty(), "sync: host jump -> crossfade, then exact again; wrap at the file end exact", err);
    }

    // 3. Loop region [0.5 s, 1.7 s): position = start + (t mod length), wraps exact.
    h.proc.setLoopRegion (0.5, 1.7);
    check (h.proc.waitUntilReferenceIdle (20000), "sync: loop region set");
    const auto li = h.proc.getReferenceInfo();
    check (li.hasLoop && li.loopStart == 0.5 && li.loopEnd == 1.7, "sync: loop region reported",
           str (li.loopStart, " .. ", li.loopEnd));
    const long long rs = 24000, re = 81600, len = re - rs;
    out = h.stream (150000);
    {
        const auto err = checkFollows (out, ref, kJumpFade, (long long) out.l.size(), hostPos (out, rs, len, 0));
        check (err.empty(), "sync: loop region follows the host and wraps exact", err);
    }

    // 4. Host stops, free run on: the reference continues from where it is.
    const long long lastTime = out.time.back();
    const long long p0 = rs + floorMod (lastTime + 1, len);
    h.head.playing = false;
    out = h.stream (100000);
    {
        const auto err = checkFollows (out, ref, 0, (long long) out.l.size(),
                                       [&] (long long i) { return rs + floorMod (p0 - rs + i, len); });
        check (err.empty(), "sync: host stopped + free run -> continues seamlessly (looping the region)", err);
    }
    check (! h.proc.isHostPlaying() && h.proc.isReferenceSounding(), "sync: reports host stopped, reference sounding");

    // 5. Free run off: fades to silence, position holds.
    h.set (RefMatchProcessor::kParamFreeRun, 0.0f);
    out = h.stream (4800);
    const double held = h.proc.getReferencePlayheadSeconds();
    const auto silent = h.stream (9600);
    check (maxAbs (out.l, (size_t) kJumpFade) == 0.0f && maxAbs (silent.l) == 0.0f, "sync: free run off -> silence");
    check (h.proc.getReferencePlayheadSeconds() == held && ! h.proc.isReferenceSounding(),
           "sync: free run off -> position holds", str (held, " -> ", h.proc.getReferencePlayheadSeconds()));

    // 6. No playhead (Standalone without transport) behaves like stopped: free run on resumes from the held point.
    h.proc.setPlayHead (nullptr);
    h.set (RefMatchProcessor::kParamFreeRun, 1.0f);
    const long long heldPos = (long long) std::llround (held * kRate);
    out = h.stream (48000);
    {
        const auto err = checkFollows (out, ref, kJumpFade, (long long) out.l.size(),
                                       [&] (long long i) { return rs + floorMod (heldPos - rs + i, len); });
        check (err.empty(), "sync: no playhead -> free run from the held position", err);
    }

    // 7. Seek while stopped.
    h.proc.seekReference (1.0);
    out = h.stream (20000);
    {
        const auto err = checkFollows (out, ref, kJumpFade, (long long) out.l.size(),
                                       [&] (long long i) { return rs + floorMod (48000 - rs + i, len); });
        check (err.empty(), "sync: seekReference while stopped -> plays from there", err);
    }

    // 8. Seek while the host plays: the host keeps driving, offset so the clicked point plays now.
    h.proc.setPlayHead (&h.head);
    h.head.playing = true;
    h.head.time = 500000;
    out = h.stream (9600);
    long long seekAt = -1, seekTime = 0;
    out = h.stream (30000, {}, { 333 }, [&] (long long i)
    {
        if (seekAt < 0 && i >= 6000)
        {
            h.proc.seekReference (1.2);
            seekAt = i;
            seekTime = h.head.time;
        }
    });
    const long long offset = floorMod (57600 - rs - seekTime, len);
    {
        const auto err = checkFollows (out, ref, seekAt + kJumpFade, (long long) out.l.size(), hostPos (out, rs, len, offset));
        check (err.empty() && ! out.l.empty(), "sync: seekReference while playing -> clicked point plays now", err);
    }
    long long jump2 = -1;
    out = h.stream (30000, {}, { 333 }, [&] (long long i)
    {
        if (jump2 < 0 && i >= 3000)
        {
            h.head.time = 700000;
            jump2 = i;
        }
    });
    {
        const auto err = checkFollows (out, ref, 0, (long long) out.l.size(), hostPos (out, rs, len, offset));
        check (err.empty(), "sync: after a seek the reference keeps following host jumps (with the offset)", err);
    }

    // 9. Other time sources: seconds and ppq (tolerance of a couple of samples, then exact). The seek offset stays
    //    until the reference changes, now applied to the whole-file region.
    h.proc.clearLoopRegion();
    h.proc.waitUntilReferenceIdle (20000);
    for (auto mode : { FakePlayHead::TimeMode::seconds, FakePlayHead::TimeMode::ppq })
    {
        h.head.mode = mode;
        h.head.time = 33333;
        out = h.stream (20000);
        const auto err = checkFollows (out, ref, 2 * kJumpFade, (long long) out.l.size(), hostPos (out, 0, n, offset));
        check (err.empty(), str ("sync: host time from ", mode == FakePlayHead::TimeMode::seconds ? "seconds" : "ppq", " -> exact"), err);
    }
    h.head.mode = FakePlayHead::TimeMode::samples;

    // 10. Loop wraps and jumps on tonal material: no clicks.
    {
        const auto tone = writeTrack ("looptone.wav", kRate, 2, 2 * 48000, sine (1000.0, 0.5, 1000.0, 0.5, kRate));
        Host t (kRate, 256);
        t.set (RefMatchProcessor::kParamMatch, 0.0f);
        t.set (RefMatchProcessor::kParamListen, 1.0f);
        t.load (tone.file);
        t.proc.setLoopRegion (0.3001, 0.8123);
        t.proc.waitUntilReferenceIdle (20000);
        t.prepare();
        t.head.playing = true;
        auto o = t.stream (96000, {}, { 256 }, [&] (long long i)
        {
            if (i == 256 * 100)
                t.head.time = 77777;   // host jump
            if (i == 256 * 250)
                t.head.playing = false;   // free run
        });
        const float step = 0.5f * (float) (juce::MathConstants<double>::twoPi * 1000.0 / kRate);
        check (maxStep (o.l, 1) < 1.6f * step, "sync: loop wraps / host jumps / stop are click-free on a tone",
               str ("max step ", maxStep (o.l, 1), " limit ", 1.6f * step));
    }

    // 11. Scrubbing: the host jumps on every block, at irregular intervals mostly shorter than the 8 ms fade, so
    //     several voices overlap. Every jump crossfades from whatever is sounding; nothing is cut off.
    {
        const auto slow = writeTrack ("scrub.wav", kRate, 2, 4 * 48000, sine (50.0, 0.5, 50.0, 0.5, kRate));
        Host t (kRate, 64);
        t.set (RefMatchProcessor::kParamMatch, 0.0f);
        t.set (RefMatchProcessor::kParamListen, 1.0f);
        t.load (slow.file);
        t.prepare();
        t.head.playing = true;
        Lcg lcg { 4u };
        const auto o = t.stream (48000, {}, { 64, 64, 300, 64, 128, 64, 1000, 64, 64, 200, 32, 96 },
                                 [&] (long long) { t.head.time = (long long) ((lcg.next() + 1.0f) * 90000.0f); });
        const float step = 0.5f * (float) (juce::MathConstants<double>::twoPi * 50.0 / kRate);
        check (maxStep (o.l, 1) < 0.03f, "sync: scrubbing (a host jump on every short block) never cuts a voice off",
               str ("max step ", maxStep (o.l, 1), ", tone step ", step));
    }

    // 12. Loops that end at the end of the file (nothing to continue with), with and without audio before the loop
    //     start, on a tone that starts and ends mid-cycle: exact outside the wrap, no clicks at the wrap.
    {
        const long long edgeLength = 48590;   // not a whole number of cycles
        const auto edge = writeTrack ("edge.wav", kRate, 2, edgeLength, [] (int, long long i)
        {
            return (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) i / kRate + 1.0));
        });
        const float step = 0.5f * (float) (juce::MathConstants<double>::twoPi * 1000.0 / kRate);

        for (const double loopStart : { -1.0, 0.25 })
        {
            Host t (kRate, 512);
            t.set (RefMatchProcessor::kParamMatch, 0.0f);
            t.set (RefMatchProcessor::kParamListen, 1.0f);
            t.prepare();
            t.load (edge.file);
            if (loopStart >= 0.0)
            {
                t.proc.setLoopRegion (loopStart, 2.0);   // clamped to the file end
                t.proc.waitUntilReferenceIdle (20000);
            }
            t.head.playing = true;
            const auto o = t.stream (3 * edgeLength);
            const long long loopFrom = loopStart >= 0.0 ? (long long) std::llround (loopStart * kRate) : 0;
            const auto err = checkFollows (o, edge, kListenFade, (long long) o.l.size() - kJumpFade,   // the next wrap's pre-fade
                                           [&] (long long i) { return loopFrom + floorMod (o.time[(size_t) i], edgeLength - loopFrom); });
            const char* what = loopStart >= 0.0 ? "loop to the file end (with lead-in)" : "whole-file loop (no lead-in)";
            check (err.empty(), str ("sync: ", what, " -> exact outside the wrap"), err);
            check (maxStep (o.l, 1) < 1.6f * step, str ("sync: ", what, " -> wrap is click-free"),
                   str ("max step ", maxStep (o.l, 1), " limit ", 1.6f * step));
        }

        // Stop and resume while the whole-file loop is in its silent lead-in (just after the end fade-out started).
        Host t (kRate, 50);
        t.set (RefMatchProcessor::kParamMatch, 0.0f);
        t.set (RefMatchProcessor::kParamListen, 1.0f);
        t.prepare();
        t.load (edge.file);
        t.head.playing = true;
        t.head.time = edgeLength - 300;
        auto o = t.stream (100, {}, { 50 });
        t.head.playing = false;
        t.set (RefMatchProcessor::kParamFreeRun, 0.0f);
        auto stopped = t.stream (1000, {}, { 50 });
        t.set (RefMatchProcessor::kParamFreeRun, 1.0f);
        auto resumed = t.stream (2000, {}, { 50 });
        o.l.insert (o.l.end(), stopped.l.begin(), stopped.l.end());
        o.l.insert (o.l.end(), resumed.l.begin(), resumed.l.end());
        check (maxStep (o.l, 1) < 1.6f * step && maxAbs (resumed.l, 1500) > 0.4f,
               "sync: stop / resume inside the loop lead-in fades in at the file start (no onset click)",
               str ("max step ", maxStep (o.l, 1), " limit ", 1.6f * step));
    }

    // 13. A host time that wobbles by a sample (rounding in hosts / wrappers: 513, 511, 513 ... samples per 512-sample
    //     block) is not a jump: the reference plays on continuously, sample-exact, no crossfades.
    {
        Host t (kRate, 512);
        t.set (RefMatchProcessor::kParamMatch, 0.0f);
        t.set (RefMatchProcessor::kParamListen, 1.0f);
        t.prepare();
        t.load (ref.file);
        t.head.playing = true;
        const long long t0 = 5000;
        long long trueTime = t0, blockIndex = 0;
        const auto o = t.stream (48000, {}, { 512 }, [&] (long long)
        {
            t.head.time = trueTime + (blockIndex++ % 2);
            trueTime += 512;
        });
        const auto err = checkFollows (o, ref, kListenFade, (long long) o.l.size(),
                                       [&] (long long i) { return floorMod (t0 + i, n); });
        check (err.empty(), "sync: host time wobbling by +-1 sample per block -> continuous, sample-exact", err);
    }

    // 14. A host time that runs ahead by one sample per block (positions derived from tempo, buggy wrappers). Until
    //     the drift exceeds the tolerance the reference just plays on at full level (before: a crossfade on every
    //     block, -5.8 dB at 128 and -11.7 dB at 64 samples); then it resyncs with exactly one crossfade, which never
    //     dips the level (DC reference: the equal-power weights sum to >= 1).
    {
        const auto dc = writeTrack ("dc.wav", kRate, 2, 4 * 48000, [] (int, long long) { return 0.5f; });
        const int tolerance = (int) std::lround (5.0 * kRate / 1000.0);   // kHostSyncToleranceMs

        for (const int block : { 64, 128 })
        {
            Host t (kRate, block);
            t.set (RefMatchProcessor::kParamMatch, 0.0f);
            t.set (RefMatchProcessor::kParamListen, 1.0f);
            t.prepare();
            t.load (dc.file);
            t.head.playing = true;
            long long reported = 1000;
            auto drift = [&] (long long) { t.head.time = reported; reported += block + 1; };

            const auto before = t.stream ((long long) (tolerance - 40) * block, {}, { block }, drift);   // drift < tolerance
            float lo = 10.0f, hi = 0.0f;
            for (size_t i = (size_t) kListenFade; i < before.l.size(); ++i)
            {
                lo = juce::jmin (lo, before.l[i] / 0.5f);
                hi = juce::jmax (hi, before.l[i] / 0.5f);
            }
            check (lo == 1.0f && hi == 1.0f, str ("sync: +1 sample per ", block, "-sample block, drift below the tolerance -> full level, no crossfade"),
                   str ("level ", lo, " .. ", hi));

            const auto after = t.stream (100LL * block, {}, { block }, drift);   // crosses the tolerance once
            int crossfades = 0;
            bool inFade = false;
            lo = 10.0f;
            for (float x : after.l)
            {
                const bool off = std::abs (x - 0.5f) > 1.0e-6f;
                crossfades += off && ! inFade ? 1 : 0;
                inFade = off;
                lo = juce::jmin (lo, x / 0.5f);
            }
            check (crossfades == 1 && lo >= 0.9999f, str ("sync: drift beyond the tolerance (", block, "-sample blocks) -> one resync crossfade, no level dip"),
                   str (crossfades, " crossfades, min level ", lo));
        }
    }
}

//==============================================================================
void testMono()
{
    std::cout << "\n-- Mono\n";

    {
        Host h (kRate, 512);
        h.set (RefMatchProcessor::kParamMatch, 0.0f);
        h.prepare();
        auto input = [] (long long n, float& l, float& r)
        {
            l = (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 300.0 * (double) n / kRate));
            r = (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * 700.0 * (double) n / kRate));
        };

        h.set (RefMatchProcessor::kParamMono, 1.0f);
        const long long start = h.processed;
        const auto out = h.stream (9600, input);
        bool exact = true;
        for (long long i = kMonoFade; exact && i < 9600; ++i)
        {
            float l = 0.0f, r = 0.0f;
            input (start + i, l, r);
            const float mid = (l + r) * 0.5f;
            exact = out.l[(size_t) i] == mid && out.r[(size_t) i] == mid;
        }
        check (exact, "mono: after the ramp L == R == (L+R)/2");
        check (maxStep (out.l) < 0.06f, "mono: fold-down ramps without a click", str (maxStep (out.l)));

        h.set (RefMatchProcessor::kParamMono, 0.0f);
        const long long start2 = h.processed;
        const auto back = h.stream (4800, input);
        exact = true;
        for (long long i = kMonoFade; exact && i < 4800; ++i)
        {
            float l = 0.0f, r = 0.0f;
            input (start2 + i, l, r);
            exact = back.l[(size_t) i] == l && back.r[(size_t) i] == r;
        }
        check (exact, "mono: off again -> bit-exact stereo input");
    }

    {
        RefMatchProcessor bus;
        juce::AudioProcessor::BusesLayout mono, stereo, mixed;
        mono.inputBuses.add (juce::AudioChannelSet::mono());
        mono.outputBuses.add (juce::AudioChannelSet::mono());
        stereo.inputBuses.add (juce::AudioChannelSet::stereo());
        stereo.outputBuses.add (juce::AudioChannelSet::stereo());
        mixed.inputBuses.add (juce::AudioChannelSet::mono());
        mixed.outputBuses.add (juce::AudioChannelSet::stereo());
        check (bus.checkBusesLayoutSupported (mono) && bus.checkBusesLayoutSupported (stereo)
                   && ! bus.checkBusesLayoutSupported (mixed),
               "mono: bus layouts mono->mono and stereo->stereo only");
    }

    {
        Host h (kRate, 512, 1);
        h.set (RefMatchProcessor::kParamMatch, 0.0f);
        const auto ref = writeTrack ("monobus.wav", kRate, 2, 48000, sine (1000.0, 0.4, 1000.0, 0.2, kRate));
        h.load (ref.file);
        h.prepare();
        check (h.proc.getTotalNumOutputChannels() == 1, "mono bus: layout applied");

        const auto in = noise (20000, 5u, 0.5f);
        const long long start = h.processed;
        const auto pass = h.stream (20000, [&] (long long i, float& l, float&) { l = in[(size_t) (i - start)]; });
        bool exact = true;
        for (size_t i = 0; exact && i < pass.l.size(); ++i)
            exact = pass.l[i] == in[i];
        check (exact, "mono bus: Mix passes through bit-exact");

        h.set (RefMatchProcessor::kParamListen, 1.0f);
        h.head.playing = true;
        h.head.time = 0;
        const auto out = h.stream (9600);
        exact = true;
        for (long long i = kListenFade + kJumpFade; exact && i < 9600; ++i)
        {
            const long long pos = floorMod (out.time[(size_t) i], ref.size());
            exact = out.l[(size_t) i] == (ref.left[(size_t) pos] + ref.right[(size_t) pos]) * 0.5f;
        }
        check (exact, "mono bus: reference folded to (L+R)/2");
    }

    {
        // Matching on a mono bus uses the loudness of the fold-down that is actually heard: a reference with audio
        // only on the left (folds to half amplitude) plays as loud as the mono mix.
        const auto oneSided = writeTrack ("left only.wav", kRate, 2, 10 * 48000, sine (500.0, 0.6, 500.0, 0.0, kRate));
        Host h (kRate, 512, 1);
        h.set (RefMatchProcessor::kParamListen, 1.0f);
        h.prepare();
        h.load (oneSided.file);
        h.head.playing = true;
        auto mixInput = [] (long long n, float& l, float&)
        {
            l = (float) (0.1 * std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) n / kRate));
        };
        const auto out = h.stream (8 * 48000, mixInput, { 512 }, [&] (long long i) { if (i % 4608 == 0) h.proc.runAnalysisNow(); });

        std::vector<float> mixL;
        for (long long n = 0; n < 8 * 48000; ++n)
        {
            float l = 0.0f, r = 0.0f;
            mixInput (n, l, r);
            mixL.push_back (l);
        }
        const float mixLufs = rm::measureIntegratedLufs (mixL.data(), nullptr, (long long) mixL.size(), kRate);
        const float outLufs = rm::measureIntegratedLufs (out.l.data() + 5 * 48000, nullptr, 3 * 48000, kRate);
        check (std::abs (outLufs - mixLufs) < 0.5f, "mono bus: Integrated matching uses the heard fold-down",
               str ("reference out ", outLufs, " LUFS, mix ", mixLufs, " LUFS"));
    }
}

//==============================================================================
std::unique_ptr<juce::XmlElement> stateXml (RefMatchProcessor& p)
{
    juce::MemoryBlock block;
    p.getStateInformation (block);
    return juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
}

void testState()
{
    std::cout << "\n-- State\n";

    const auto ref = writeTrack ("state ref.wav", 44100.0, 2, 4 * 44100, sine (440.0, 0.3, 550.0, 0.3, 44100.0));
    juce::MemoryBlock saved;

    {
        Host a;
        a.prepare();
        check (a.load (ref.file), "state: reference loads");
        a.proc.setLoopRegion (1.0, 2.5);
        a.proc.waitUntilReferenceIdle (20000);
        a.set (RefMatchProcessor::kParamMatch, 2.0f);
        a.set (RefMatchProcessor::kParamMono, 1.0f);
        a.set (RefMatchProcessor::kParamFreeRun, 0.0f);
        a.set (RefMatchProcessor::kParamAveraging, 1.0f);
        a.set (RefMatchProcessor::kParamListen, 1.0f);
        a.proc.setEditorSize ({ 1300, 800 });
        a.proc.getStateInformation (saved);

        const auto xml = stateXml (a.proc);
        const auto* r = xml != nullptr ? xml->getChildByName ("Reference") : nullptr;
        check (xml != nullptr && xml->hasTagName ("RefMatch") && xml->getIntAttribute ("version") == 1
                   && xml->getIntAttribute ("editorW") == 1300 && xml->getIntAttribute ("editorH") == 800
                   && xml->getChildByName ("PARAMS") != nullptr && r != nullptr
                   && r->getStringAttribute ("path") == ref.file.getFullPathName() && r->getIntAttribute ("hasLoop") == 1
                   && r->getDoubleAttribute ("loopStart") == 1.0 && r->getDoubleAttribute ("loopEnd") == 2.5,
               "state: XML format (RefMatch / PARAMS / Reference with loop)",
               xml != nullptr ? xml->toString().toStdString() : std::string ("no xml"));
    }

    {
        // Restored before prepareToPlay, like hosts do; prepareToPlay then re-loads at the real rate.
        Host b;
        b.proc.setStateInformation (saved.getData(), (int) saved.getSize());
        b.prepare();
        check (b.proc.waitUntilReferenceIdle (30000), "state: restore loads the reference");

        const auto info = b.proc.getReferenceInfo();
        const auto track = b.proc.getReferenceTrack();
        check (info.state == RefMatchProcessor::ReferenceState::Ready && info.file == ref.file && track != nullptr
                   && track->sampleRate == kRate,
               "state: reference path restored (re-loaded at the host rate)", info.error.toStdString());
        check (info.hasLoop && std::abs (info.loopStart - 1.0) < 1.0e-9 && std::abs (info.loopEnd - 2.5) < 1.0e-9,
               "state: loop region restored", str (info.loopStart, " .. ", info.loopEnd));
        check (b.get (RefMatchProcessor::kParamMatch) == 2.0f && b.get (RefMatchProcessor::kParamMono) == 1.0f
                   && b.get (RefMatchProcessor::kParamFreeRun) == 0.0f && b.get (RefMatchProcessor::kParamAveraging) == 1.0f,
               "state: parameters restored");
        check (b.get (RefMatchProcessor::kParamListen) == 0.0f, "state: Listen forced back to Mix");
        check (b.proc.getEditorSize() == juce::Point<int> (1300, 800), "state: editor size round-trip");

        const auto again = stateXml (b.proc);
        const auto* r = again != nullptr ? again->getChildByName ("Reference") : nullptr;
        check (r != nullptr && r->getStringAttribute ("path") == ref.file.getFullPathName()
                   && r->getDoubleAttribute ("loopStart") == 1.0 && r->getDoubleAttribute ("loopEnd") == 2.5,
               "state: re-saved state keeps reference and loop");

        // Same file again (e.g. undo): no re-load, loop applied directly.
        const auto before = b.proc.getReferenceTrack();
        b.proc.setStateInformation (saved.getData(), (int) saved.getSize());
        check (b.proc.getReferenceTrack() == before && b.proc.getReferenceInfo().state == RefMatchProcessor::ReferenceState::Ready,
               "state: restoring the same reference keeps the loaded track");
    }

    {
        // Missing file: Error, path kept and saved again.
        auto parsed = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        const auto missing = tempDir.getChildFile ("gone away.wav");
        parsed->getChildByName ("Reference")->setAttribute ("path", missing.getFullPathName());
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (*parsed, block);

        Host c;
        c.prepare();
        c.proc.setStateInformation (block.getData(), (int) block.getSize());
        c.proc.waitUntilReferenceIdle (20000);
        const auto info = c.proc.getReferenceInfo();
        check (info.state == RefMatchProcessor::ReferenceState::Error && info.error == "File not found: gone away.wav"
                   && info.file == missing,
               "state: missing file -> Error 'File not found: <name>', path kept", info.error.toStdString());
        const auto resaved = stateXml (c.proc);
        const auto* r = resaved != nullptr ? resaved->getChildByName ("Reference") : nullptr;
        check (r != nullptr && r->getStringAttribute ("path") == missing.getFullPathName() && r->getIntAttribute ("hasLoop") == 1,
               "state: missing file is saved again (project keeps the path)");

        // No Reference element -> no reference.
        parsed->removeChildElement (parsed->getChildByName ("Reference"), true);
        parsed->setAttribute ("editorW", 5000);
        parsed->setAttribute ("editorH", 10);
        juce::AudioProcessor::copyXmlToBinary (*parsed, block);
        c.proc.setStateInformation (block.getData(), (int) block.getSize());
        check (c.proc.getReferenceInfo().state == RefMatchProcessor::ReferenceState::Empty, "state: no Reference element -> empty");
        check (c.proc.getEditorSize() == juce::Point<int> (1600, 560), "state: editor size clamped to the allowed range",
               str (c.proc.getEditorSize().x, "x", c.proc.getEditorSize().y));
    }

    {
        // Relink after the project moved: the saved path is missing (Error), the user picks the same file name in
        // its new folder -> the project's loop region is kept. Another file name is a new reference without a loop.
        auto parsed = juce::AudioProcessor::getXmlFromBinary (saved.getData(), (int) saved.getSize());
        parsed->getChildByName ("Reference")->setAttribute ("path", tempDir.getChildFile ("old folder").getChildFile ("relink me.wav").getFullPathName());
        juce::MemoryBlock missingState;
        juce::AudioProcessor::copyXmlToBinary (*parsed, missingState);

        const auto moved = writeTrackFile (tempDir.getChildFile ("new folder").getChildFile ("Relink Me.WAV"), 44100.0, 2,
                                           3 * 44100, sine (440.0, 0.3, 550.0, 0.3, 44100.0));
        const auto other = writeTrack ("something else.wav", 44100.0, 2, 3 * 44100, sine (440.0, 0.3, 550.0, 0.3, 44100.0));

        Host e;
        e.prepare();
        e.proc.setStateInformation (missingState.getData(), (int) missingState.getSize());
        e.proc.waitUntilReferenceIdle (20000);
        check (e.proc.getReferenceInfo().state == RefMatchProcessor::ReferenceState::Error, "state: relink - saved path missing -> Error");

        check (e.load (moved.file), "state: relink - file picked in its new folder loads");
        const auto info = e.proc.getReferenceInfo();
        check (info.state == RefMatchProcessor::ReferenceState::Ready && info.file == moved.file && info.hasLoop
                   && info.loopStart == 1.0 && info.loopEnd == 2.5,
               "state: relink (same name, another folder / case) keeps the saved loop region",
               str (info.hasLoop, " ", info.loopStart, " .. ", info.loopEnd, " ", info.error));

        const auto resaved = stateXml (e.proc);
        const auto* r = resaved != nullptr ? resaved->getChildByName ("Reference") : nullptr;
        check (r != nullptr && r->getStringAttribute ("path") == moved.file.getFullPathName() && r->getIntAttribute ("hasLoop") == 1
                   && r->getDoubleAttribute ("loopStart") == 1.0 && r->getDoubleAttribute ("loopEnd") == 2.5,
               "state: relink - the new path and the loop are saved");

        // Loading again while Ready, or a different name from the Error state: a new reference starts without a loop.
        check (e.load (moved.file) && ! e.proc.getReferenceInfo().hasLoop, "state: re-loading a Ready reference starts without a loop");
        e.proc.setStateInformation (missingState.getData(), (int) missingState.getSize());
        e.proc.waitUntilReferenceIdle (20000);
        check (e.load (other.file) && e.proc.getReferenceInfo().state == RefMatchProcessor::ReferenceState::Ready
                   && ! e.proc.getReferenceInfo().hasLoop,
               "state: a different file name from the Error state -> no loop");
    }

    {
        // Garbage must not crash and must leave sane parameters.
        Host d;
        d.prepare();
        Lcg lcg { 99u };
        std::vector<char> junk (3000);
        for (auto& ch : junk)
            ch = (char) (lcg.next() * 127.0f);
        d.proc.setStateInformation (junk.data(), (int) junk.size());
        d.proc.setStateInformation (saved.getData(), (int) saved.getSize() / 2);   // truncated
        d.proc.setStateInformation (nullptr, 0);

        juce::XmlElement wrong ("Something");
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (wrong, block);
        d.proc.setStateInformation (block.getData(), (int) block.getSize());

        juce::XmlElement bad ("RefMatch");
        auto* params = bad.createNewChildElement ("PARAMS");
        auto* p1 = params->createNewChildElement ("PARAM");
        p1->setAttribute ("id", "matchMode");
        p1->setAttribute ("value", "nan");
        auto* p2 = params->createNewChildElement ("PARAM");
        p2->setAttribute ("id", "averaging");
        p2->setAttribute ("value", 77.0);
        auto* p3 = params->createNewChildElement ("PARAM");
        p3->setAttribute ("id", "listen");
        p3->setAttribute ("value", 1.0);
        auto* r = bad.createNewChildElement ("Reference");
        r->setAttribute ("path", "relative\\path.wav");
        r->setAttribute ("hasLoop", 1);
        r->setAttribute ("loopStart", "inf");
        juce::AudioProcessor::copyXmlToBinary (bad, block);
        d.proc.setStateInformation (block.getData(), (int) block.getSize());
        d.proc.waitUntilReferenceIdle (20000);

        const float match = d.get (RefMatchProcessor::kParamMatch), avg = d.get (RefMatchProcessor::kParamAveraging);
        check (std::isfinite (match) && match >= 0.0f && match <= 2.0f && avg == 1.0f
                   && d.get (RefMatchProcessor::kParamListen) == 0.0f,
               "state: garbage / truncated / out-of-range data does not crash, values sanitised",
               str ("match ", match, " averaging ", avg));
        check (d.proc.getReferenceInfo().state == RefMatchProcessor::ReferenceState::Error,
               "state: odd reference path -> Error, no crash");
        const auto out = d.stream (4800);
        check (allFinite (out), "state: processing still fine afterwards");
    }
}

//==============================================================================
void testSampleRateChange()
{
    std::cout << "\n-- Sample-rate change\n";

    const auto ref = writeTrack ("rate.wav", 44100.0, 2, 2 * 44100, sine (1000.0, 0.5, 1000.0, 0.5, 44100.0));
    Host h (kRate, 512);
    h.prepare();
    h.load (ref.file);
    h.proc.setLoopRegion (0.5, 1.5);
    h.proc.waitUntilReferenceIdle (20000);
    const auto t48 = h.proc.getReferenceTrack();
    check (t48 != nullptr && t48->sampleRate == kRate, "rate: loaded at 48 kHz");

    h.proc.releaseResources();
    h.sampleRate = 96000.0;
    h.head.sampleRate = 96000.0;
    h.prepare (false);
    check (h.proc.getReferenceTrack() == nullptr || h.proc.getReferenceInfo().state == RefMatchProcessor::ReferenceState::Loading,
           "rate: old-rate track is not played after the change");
    check (h.proc.waitUntilReferenceIdle (30000), "rate: re-load finishes");
    const auto info = h.proc.getReferenceInfo();
    const auto t96 = h.proc.getReferenceTrack();
    check (info.state == RefMatchProcessor::ReferenceState::Ready && t96 != nullptr && t96->sampleRate == 96000.0
               && t96->numSamples() == 192000,
           "rate: re-loaded at 96 kHz", t96 != nullptr ? str (t96->sampleRate, " ", t96->numSamples()) : std::string());
    check (info.hasLoop && info.loopStart == 0.5 && info.loopEnd == 1.5, "rate: loop region kept");

    if (t96 != nullptr)
    {
        const double f = measureFrequency (t96->audio.getReadPointer (0) + 4000, 180000, 96000.0);
        check (std::abs (f - 1000.0) < 0.05, "rate: pitch correct at the new rate", str (f, " Hz"));
    }

    h.set (RefMatchProcessor::kParamListen, 1.0f);
    const auto out = h.stream (19200);
    const double played = measureFrequency (out.l.data() + 9600, 9600, 96000.0);
    check (maxAbs (out.l, 9600) > 0.05f && std::abs (played - 1000.0) < 0.5, "rate: reference plays at the new rate",
           str (maxAbs (out.l, 9600), " peak, ", played, " Hz"));

    h.prepare();   // same rate again: nothing re-loaded
    check (h.proc.getReferenceTrack() == t96 && h.proc.getReferenceInfo().state == RefMatchProcessor::ReferenceState::Ready,
           "rate: prepareToPlay at the same rate keeps the track");
}

//==============================================================================
void testAnalysis()
{
    std::cout << "\n-- Analysis\n";

    const auto refNoise = noise (6 * 48000, 21u, 0.3f);
    const auto ref = writeTrack ("analysis ref.wav", kRate, 2, 6 * 48000, [&] (int, long long i) { return refNoise[(size_t) i]; });
    const auto mixNoise = noise (100000, 31u, 0.25f);

    Host h;
    h.prepare();
    h.load (ref.file);
    const int v0 = h.proc.getAnalysisVersion();
    h.head.playing = true;
    h.stream (5 * 48000, [&] (long long i, float& l, float& r)
    {
        l = mixNoise[(size_t) (i % 100000)];
        r = mixNoise[(size_t) ((i + 777) % 100000)];
    }, { 512 }, [&] (long long i) { if (i % 9216 == 0) h.proc.runAnalysisNow(); });
    h.proc.runAnalysisNow();

    const auto mix = h.proc.getMixProfile();
    const auto refProfile = h.proc.getReferenceProfile();
    const auto cmp = h.proc.getComparison();
    check (h.proc.getAnalysisVersion() > v0, "analysis: version increments");
    check (mix.valid && rm::hasValue (mix.integratedLufs) && mix.analysedSeconds > 3.0,
           "analysis: mix profile published", str ("valid ", mix.valid, " I ", mix.integratedLufs, " s ", mix.analysedSeconds));
    check (refProfile.valid && rm::hasValue (refProfile.integratedLufs), "analysis: reference profile published",
           str ("I ", refProfile.integratedLufs));
    check (cmp.valid() && ! cmp.hints.empty(), "analysis: comparison published with hints",
           str ("mixReady ", cmp.mixReady, " refReady ", cmp.referenceReady, " hints ", cmp.hints.size()));
    check (! h.proc.getReferenceInfo().analysing, "analysis: reference analysis finished");

    h.proc.resetMixAnalysis();
    const auto cleared = h.proc.getMixProfile();
    check (! cleared.valid && ! rm::hasValue (cleared.integratedLufs) && ! h.proc.getComparison().mixReady,
           "analysis: resetMixAnalysis clears the mix measurements");

    // Averaging: Recent forgets old audio, Since reset keeps it; the parameter switch reaches the analyser. 5 s of
    // bass, then three Recent time constants of presence.
    const long long averagingLength = (long long) ((5.0 + 3.0 * rm::kRecentSeconds) * kRate);
    auto lowMinusHigh = [&] (int startMode, int switchTo)
    {
        Host x;
        x.set (RefMatchProcessor::kParamAveraging, (float) startMode);
        x.prepare();
        x.proc.runAnalysisNow();
        if (switchTo >= 0)
        {
            x.set (RefMatchProcessor::kParamAveraging, (float) switchTo);
            x.proc.runAnalysisNow();
        }

        x.stream (averagingLength, [] (long long i, float& l, float& r)
        {
            const double f = i < 5 * 48000 ? 100.0 : 4000.0;
            l = r = (float) (0.3 * std::sin (juce::MathConstants<double>::twoPi * f * (double) i / kRate));
        }, { 1024 }, [&] (long long i) { if (i % (1024 * 20) == 0) x.proc.runAnalysisNow(); });
        x.proc.runAnalysisNow();
        const auto p = x.proc.getMixProfile();
        return p.bands[1].levelDb - p.bands[4].levelDb;
    };

    const float recent = lowMinusHigh (0, -1), sinceReset = lowMinusHigh (1, -1), switched = lowMinusHigh (0, 1);
    check (sinceReset - recent > 6.0f, "analysis: Recent forgets older audio, Since reset keeps it",
           str ("bass - presence: recent ", recent, " dB, since reset ", sinceReset, " dB"));
    check (std::abs (switched - sinceReset) < 0.01f, "analysis: Averaging parameter switch is applied",
           str (switched, " vs ", sinceReset));
}

//==============================================================================
void testTransportAnalysis()
{
    std::cout << "\n-- Analysis and host transport\n";

    const auto in = noise (200000, 51u, 0.3f);
    auto input = [&] (long long i, float& l, float& r)
    {
        l = in[(size_t) (i % 100000)];
        r = in[(size_t) (i % 100000) + 100000];
    };
    auto analyseEvery = [] (Host& h) { return [&h] (long long i) { if (i % 9216 == 0) h.proc.runAnalysisNow(); }; };

    // Reference: the same steady noise played without interruption.
    Host a;
    a.prepare();
    a.head.playing = true;
    a.stream (48 * 48000, input, { 512 }, analyseEvery (a));
    a.proc.runAnalysisNow();
    const auto pa = a.proc.getMixProfile();

    // FL keeps calling processBlock with silence while stopped: 6 plays of 8 s with 3 s stops in between.
    Host b;
    b.prepare();
    bool frozen = true;
    for (int play = 0; play < 6; ++play)
    {
        b.head.playing = true;
        b.stream (8 * 48000, input, { 512 }, analyseEvery (b));
        b.head.playing = false;
        b.proc.runAnalysisNow();
        const auto atStop = b.proc.getMixProfile();
        b.stream (3 * 48000, {}, { 512 }, analyseEvery (b));
        b.proc.runAnalysisNow();
        const auto stopped = b.proc.getMixProfile();
        frozen = frozen && stopped.analysedSeconds == atStop.analysedSeconds
                 && stopped.integratedLufs == atStop.integratedLufs && stopped.shortTermLufs == atStop.shortTermLufs;
    }
    const auto pb = b.proc.getMixProfile();

    check (frozen, "transport: while the host is stopped the mix analysis holds (silence is not measured)");
    check (pa.loudnessRangeLu < 1.0f && pb.loudnessRangeLu < 1.0f,
           "transport: stop / start does not inflate the mix LRA (steady noise < 1 LU)",
           str ("LRA contiguous ", pa.loudnessRangeLu, " LU, with stops ", pb.loudnessRangeLu, " LU"));
    check (std::abs (pb.integratedLufs - pa.integratedLufs) < 0.05f,
           "transport: stop / start does not lower the mix integrated loudness",
           str ("I contiguous ", pa.integratedLufs, " LUFS, with stops ", pb.integratedLufs, " LUFS"));

    // A playhead that never reports playing (JUCE's Standalone player, transport-less hosts) and no playhead at all:
    // everything is measured, as before.
    for (const bool withPlayHead : { true, false })
    {
        Host c;
        if (! withPlayHead)
            c.proc.setPlayHead (nullptr);
        c.prepare();
        c.stream (5 * 48000, input, { 512 }, analyseEvery (c));
        c.proc.runAnalysisNow();
        const auto pc = c.proc.getMixProfile();
        check (pc.analysedSeconds > 4.0 && std::abs (pc.integratedLufs - pa.integratedLufs) < 0.1f,
               withPlayHead ? "transport: a playhead that never plays (Standalone) -> the input is measured"
                            : "transport: no playhead -> the input is measured",
               str (pc.analysedSeconds, " s, I ", pc.integratedLufs));
    }
}

//==============================================================================
void testShortTermMatching()
{
    std::cout << "\n-- Short-term matching\n";

    // Reference: 10 s of noise 10 dB down, then the loud 10 s (the loop region), then 5 s quiet again. Left == right,
    // so the mono fold-down is the same signal. Mix: different noise at the loud part's level -> target ~0 dB.
    const long long second = 48000;
    const auto refNoise = noise (25 * second, 61u, 1.0f);
    const auto ref = writeTrack ("st ref.wav", kRate, 2, 25 * second, [&] (int, long long i)
    {
        const bool loud = i >= 10 * second && i < 20 * second;
        return refNoise[(size_t) i] * (loud ? 0.3f : 0.095f);
    });
    const auto mixNoise = noise (300000, 62u, 0.3f);

    for (const int channels : { 2, 1 })
    {
        const std::string bus = channels == 2 ? "" : " (mono bus)";
        bool playing = true;
        float level = 1.0f;   // mix input gain
        auto input = [&] (long long i, float& l, float& r) { l = r = playing ? level * mixNoise[(size_t) (i % 300000)] : 0.0f; };

        Host h (kRate, 512, channels);
        h.set (RefMatchProcessor::kParamMatch, 2.0f);
        h.set (RefMatchProcessor::kParamListen, 1.0f);   // free run is on (default)
        h.prepare();
        h.load (ref.file);
        h.proc.setLoopRegion (10.0, 20.0);
        h.proc.waitUntilReferenceIdle (20000);

        std::vector<float> gains;
        auto run = [&] (long long samples)
        {
            gains.clear();
            h.stream (samples, input, { 512 }, [&] (long long i)
            {
                if (i % 4608 == 0)
                {
                    h.proc.runAnalysisNow();
                    gains.push_back (h.proc.getMatchGainDb());
                }
            });
        };
        auto range = [&] (size_t from, float centre)
        {
            float worst = 0.0f;
            for (size_t k = from; k < gains.size(); ++k)
                worst = juce::jmax (worst, std::abs (gains[k] - centre));
            return worst;
        };

        // Play from bar 1: the reference loops its loud region, 35 s = three wraps. Before, each wrap compared the
        // file's 3 s before the region (the quiet part) and boosted the reference by up to 10 dB.
        h.head.playing = true;
        h.head.time = 0;
        run (35 * second);
        const float steady = gains.back();
        check (std::abs (steady) < 0.5f && range (42, steady) < 1.0f,   // from 4 s on
               "short-term: across loop wraps the gain stays within 1 dB" + bus,
               str ("steady ", steady, " dB, max deviation ", range (42, steady), " dB"));

        // Host stops (free run: the reference keeps playing, the mix is silent) for 4 s, then plays again from the
        // start: the gain holds instead of pumping the reference down and swelling back.
        h.head.playing = playing = false;
        run (4 * second);
        const float stopDev = range (0, steady);
        h.head.playing = playing = true;
        h.head.time = 0;
        run (6 * second);
        const float playDev = range (0, steady);
        check (stopDev < 1.0f && playDev < 1.0f, "short-term: host stop and restart hold the gain (within 1 dB)" + bus,
               str ("max deviation stopped ", stopDev, " dB, after play ", playDev, " dB"));
    }

    // A break of digital silence inside the reference (1 s at 8 s) is not compared either: the gain holds instead of
    // boosting the reference by the silence's share of the window (+1.8 dB) and carrying that into the next beat.
    {
        const auto gapped = writeTrack ("st gap.wav", kRate, 2, 20 * second, [&] (int, long long i)
        {
            return i >= 8 * second && i < 9 * second ? 0.0f : 0.3f * refNoise[(size_t) i];
        });
        auto input = [&] (long long i, float& l, float& r) { l = r = mixNoise[(size_t) (i % 300000)]; };

        Host h (kRate, 512);
        h.set (RefMatchProcessor::kParamMatch, 2.0f);
        h.set (RefMatchProcessor::kParamListen, 1.0f);
        h.prepare();
        h.load (gapped.file);
        h.head.playing = true;

        float lowest = 100.0f, highest = -100.0f;
        h.stream (16 * second, input, { 512 }, [&] (long long i)
        {
            if (i % 4608 == 0)
            {
                h.proc.runAnalysisNow();
                if (i >= 4 * second)
                {
                    lowest = juce::jmin (lowest, h.proc.getMatchGainDb());
                    highest = juce::jmax (highest, h.proc.getMatchGainDb());
                }
            }
        });
        check (lowest > -1.0f && highest < 1.0f, "short-term: a silent break in the reference holds the gain (within 1 dB)",
               str ("gain ", lowest, " .. ", highest, " dB"));
    }

    // Standalone (no transport): the reference follows the live input's short-term loudness, and a noise floor or
    // silence holds the last gain instead of pulling the reference down to -40 dB.
    {
        bool silent = false;
        float level = 1.0f;
        auto input = [&] (long long i, float& l, float& r)
        {
            l = r = silent ? 3.0e-5f * mixNoise[(size_t) (i % 300000)] / 0.3f : level * mixNoise[(size_t) (i % 300000)];
        };

        Host h (kRate, 512);
        h.set (RefMatchProcessor::kParamMatch, 2.0f);
        h.set (RefMatchProcessor::kParamListen, 1.0f);
        h.prepare();
        h.load (ref.file);
        h.proc.setLoopRegion (10.0, 20.0);
        h.proc.waitUntilReferenceIdle (20000);

        auto play = [&] (long long samples)
        {
            float lowest = 100.0f, highest = -100.0f;
            h.stream (samples, input, { 512 }, [&] (long long i)
            {
                if (i % 4608 == 0)
                {
                    h.proc.runAnalysisNow();
                    lowest = juce::jmin (lowest, h.proc.getMatchGainDb());
                    highest = juce::jmax (highest, h.proc.getMatchGainDb());
                }
            });
            h.proc.runAnalysisNow();
            return std::make_pair (lowest, highest);
        };

        play (6 * second);
        const float g0 = h.proc.getMatchGainDb();
        level = 0.1f;   // the mix gets 20 dB quieter
        play (6 * second);
        const float g1 = h.proc.getMatchGainDb();
        check (std::abs (g0) < 0.5f && std::abs (g1 + 20.0f) < 0.5f,
               "short-term: without a transport the gain follows the input's short-term loudness",
               str ("gain ", g0, " dB, then ", g1, " dB (want 0 and -20)"));

        silent = true;   // -90 dBFS noise floor
        const auto [lowest, highest] = play (6 * second);
        check (lowest > g1 - 1.0f && highest < g1 + 1.0f, "short-term: a noise floor below -70 LUFS holds the gain",
               str ("gain ", lowest, " .. ", highest, " dB, held ", g1, " dB"));
    }
}

//==============================================================================
void testListenWithoutReference()
{
    std::cout << "\n-- Listen = Reference without a playable reference\n";

    auto mixInput = [] (long long n, float& l, float& r)
    {
        l = (float) (0.5 * std::sin (juce::MathConstants<double>::twoPi * 220.0 * (double) n / kRate));
        r = (float) (0.45 * std::sin (juce::MathConstants<double>::twoPi * 220.0 * (double) n / kRate + 0.3));
    };
    // First sample (from `from`) where the output differs from the input, -1 when none.
    auto firstMismatch = [&] (const Output& o, long long start, long long from)
    {
        for (long long i = from; i < (long long) o.l.size(); ++i)
        {
            float l = 0.0f, r = 0.0f;
            mixInput (start + i, l, r);
            if (o.l[(size_t) i] != l || o.r[(size_t) i] != r)
                return i;
        }
        return -1LL;
    };

    const auto tone = writeTrack ("nr tone.wav", kRate, 2, 3 * 48000, sine (1000.0, 0.4, 1000.0, 0.4, kRate));
    const float stepLimit = 1.6f * 0.5f * (float) (juce::MathConstants<double>::twoPi * 1000.0 / kRate);

    {
        // Automation or a controller switches to Reference before any file is loaded.
        Host h;
        h.set (RefMatchProcessor::kParamListen, 1.0f);
        h.prepare();
        const long long start = h.processed;
        const auto out = h.stream (9600, mixInput);
        check (firstMismatch (out, start, 0) < 0 && h.proc.getListenPosition() == 0.0f,
               "no reference: Listen = Reference without a file -> the mix plays untouched");
    }

    {
        Host h (kRate, 480);
        h.set (RefMatchProcessor::kParamMatch, 0.0f);
        h.set (RefMatchProcessor::kParamListen, 1.0f);
        h.prepare();
        check (h.load (tone.file), "no reference: reference loads");
        h.head.playing = true;
        auto out = h.stream (9600, mixInput);
        check (h.proc.getListenPosition() == 1.0f && maxStep (out.l) < stepLimit,
               "no reference: a reference loaded while on Reference fades in from the mix");

        // Removing the reference while listening to it: back to the mix, click-free, then bit-exact.
        h.proc.clearReference();
        long long start = h.processed;
        out = h.stream (9600, mixInput);
        const long long cleared = firstMismatch (out, start, kListenFade + kJumpFade);
        check (cleared < 0 && maxStep (out.l) < stepLimit && h.proc.getListenPosition() == 0.0f,
               "no reference: clearing it while on Reference crossfades back to the mix",
               str ("first mismatch ", cleared, ", max step ", maxStep (out.l), " limit ", stepLimit));

        // The next file is heard again (Listen still on).
        check (h.load (tone.file), "no reference: loads again");
        out = h.stream (9600, mixInput);
        check (h.proc.getListenPosition() == 1.0f && maxStep (out.l) < stepLimit,
               "no reference: the next reference is heard again");

        // A file that fails to load replaces the reference: the mix plays.
        const auto garbage = tempDir.getChildFile ("nr garbage.wav");
        garbage.replaceWithText ("not audio");
        h.load (garbage);
        start = h.processed;
        out = h.stream (9600, mixInput);
        const long long failed = firstMismatch (out, start, kListenFade + kJumpFade);
        check (h.proc.getReferenceInfo().state == RefMatchProcessor::ReferenceState::Error && failed < 0
                   && maxStep (out.l) < stepLimit,
               "no reference: a load error while on Reference returns to the mix",
               str ("first mismatch ", failed, ", max step ", maxStep (out.l)));
    }

    {
        // Sample-rate change while on Reference: the mix is heard while the file re-loads, then the reference.
        const auto slow = writeTrack ("nr rate.wav", 44100.0, 2, 20 * 44100, sine (1000.0, 0.4, 1000.0, 0.4, 44100.0));
        Host h (kRate, 512);
        h.set (RefMatchProcessor::kParamMatch, 0.0f);
        h.set (RefMatchProcessor::kParamListen, 1.0f);
        h.prepare();
        h.load (slow.file);
        h.stream (4800, mixInput);

        // Every block after which the new track is still not there (the loader installs the track and the UI pointer
        // in one locked step) was played without a track: it must be the untouched input. The re-load (resampling
        // 20 s) takes far longer than a block; should the test thread still miss it, the rate change is repeated.
        Output all;
        long long checkedBlocks = 0, badBlock = -1;

        for (int attempt = 0; attempt < 3 && checkedBlocks == 0; ++attempt)
        {
            h.proc.releaseResources();
            h.sampleRate = h.head.sampleRate = (attempt % 2 == 0 ? 96000.0 : 88200.0);
            h.prepare (false);
            all = {};

            for (int guard = 0; guard < 20000 && ! h.proc.waitUntilReferenceIdle (0); ++guard)
            {
                const long long start = h.processed;
                const auto more = h.stream (512, mixInput);
                all.l.insert (all.l.end(), more.l.begin(), more.l.end());

                if (h.proc.getReferenceTrack() == nullptr)
                {
                    ++checkedBlocks;
                    if (firstMismatch (more, start, 0) >= 0 && badBlock < 0)
                        badBlock = guard;
                }

                juce::Thread::sleep (1);
            }
        }

        check (checkedBlocks > 0 && badBlock < 0, "no reference: during the re-load after a rate change the mix plays untouched",
               str (checkedBlocks, " blocks checked, first bad block ", badBlock));
        const auto after = h.stream (19200, mixInput);
        all.l.insert (all.l.end(), after.l.begin(), after.l.end());
        const double played = measureFrequency (after.l.data() + 9600, 9600, h.sampleRate);
        check (std::abs (played - 1000.0) < 0.5 && h.proc.getListenPosition() == 1.0f && maxStep (all.l) < stepLimit,
               "no reference: after the re-load the reference fades in (click-free)",
               str (played, " Hz, max step ", maxStep (all.l), " limit ", stepLimit));
    }
}

//==============================================================================
void testSlotLock()
{
    std::cout << "\n-- Track hand-over lock\n";

    const auto a = writeTrack ("lock a.wav", kRate, 2, 2 * 48000, sine (300.0, 0.4, 300.0, 0.4, kRate));
    const auto b = writeTrack ("lock b.wav", kRate, 2, 2 * 48000, sine (500.0, 0.4, 500.0, 0.4, kRate));

    {
        // The analysis thread no longer locks while nothing is parked; a replaced track must still be freed.
        Host h (kRate, 256);
        h.set (RefMatchProcessor::kParamListen, 1.0f);
        h.prepare();
        h.load (a.file);
        h.stream (4800);
        const std::weak_ptr<const rm::ReferenceTrack> weakA = h.proc.getReferenceTrack();
        h.load (b.file);
        h.stream (4 * kJumpFade);   // the audio thread fades out of A and marks it releasable
        h.proc.runAnalysisNow();
        check (! weakA.lock() && h.proc.getReferenceInfo().displayName == "lock b",
               "lock: a replaced track is still released after its fade-out");
    }

    {
        // Audio thread flat out (Listen = Reference, free run) while the analysis thread runs: never a dropout.
        Host h (kRate, 256);
        h.set (RefMatchProcessor::kParamMatch, 0.0f);
        h.set (RefMatchProcessor::kParamListen, 1.0f);
        h.prepare();
        h.load (a.file);
        h.stream (4800);

        juce::AudioBuffer<float> buffer (2, 256);
        constexpr long long blocks = 300000;   // ~1 s here; the analysis thread passes every 15 ms meanwhile
        long long silentBlocks = 0;
        const int before = h.proc.getReferenceDropoutCount();

        for (long long k = 0; k < blocks; ++k)
        {
            buffer.clear();
            h.processBlock (buffer);
            silentBlocks += buffer.getMagnitude (0, 256) == 0.0f ? 1 : 0;
        }

        const int dropouts = h.proc.getReferenceDropoutCount() - before;
        check (dropouts == 0 && silentBlocks == 0, "lock: no reference dropouts while the analysis thread runs",
               str (blocks, " blocks, ", dropouts, " try-lock failures, ", silentBlocks, " silent blocks"));
    }
}

//==============================================================================
void testThreadSafety()
{
    std::cout << "\n-- Thread safety\n";

    const auto a = writeTrack ("ts a.wav", kRate, 2, 2 * 48000, sine (300.0, 0.4, 310.0, 0.4, kRate));
    const auto b = writeTrack ("ts b.wav", 44100.0, 1, 2 * 44100, sine (500.0, 0.4, 500.0, 0.4, 44100.0));
    const auto missing = tempDir.getChildFile ("ts missing.wav");

    Host h (kRate, 256);
    h.prepare();
    h.load (a.file);

    std::atomic<bool> stop { false };
    std::atomic<long long> blocks { 0 };
    std::atomic<bool> finite { true };

    std::thread audio ([&]
    {
        juce::AudioBuffer<float> buffer (2, 256);
        Lcg lcg { 3u };
        while (! stop.load())
        {
            for (int i = 0; i < 256; ++i)
            {
                buffer.setSample (0, i, 0.2f * lcg.next());
                buffer.setSample (1, i, 0.2f * lcg.next());
            }

            if (lcg.next() > 0.995f)
                h.head.time = (long long) ((lcg.next() + 1.0f) * 200000.0f);
            if (lcg.next() > 0.998f)
                h.head.playing = ! h.head.playing;

            h.processBlock (buffer);
            for (int i = 0; i < 256; ++i)
                if (! std::isfinite (buffer.getSample (0, i)) || ! std::isfinite (buffer.getSample (1, i)))
                    finite.store (false);
            blocks.fetch_add (1);
        }
    });

    juce::MemoryBlock state;
    h.proc.getStateInformation (state);
    Lcg lcg { 8u };
    const double end = juce::Time::getMillisecondCounterHiRes() + 3000.0;
    int ops = 0;

    while (juce::Time::getMillisecondCounterHiRes() < end)
    {
        const float x = lcg.next() * 0.5f + 0.5f;

        if (x < 0.15f)
            h.proc.loadReference (a.file);
        else if (x < 0.25f)
            h.proc.loadReference (b.file);
        else if (x < 0.28f)
            h.proc.loadReference (missing);
        else if (x < 0.35f)
            h.proc.clearReference();
        else if (x < 0.55f)
            h.proc.setLoopRegion ((lcg.next() + 1.0) * 0.8, (lcg.next() + 1.0) * 0.9);
        else if (x < 0.6f)
            h.proc.clearLoopRegion();
        else if (x < 0.7f)
            h.proc.seekReference ((lcg.next() + 1.0) * 1.0);
        else if (x < 0.75f)
            h.set (RefMatchProcessor::kParamListen, lcg.next() > 0.0f ? 1.0f : 0.0f);
        else if (x < 0.78f)
            h.set (RefMatchProcessor::kParamMono, lcg.next() > 0.0f ? 1.0f : 0.0f);
        else if (x < 0.81f)
            h.set (RefMatchProcessor::kParamMatch, (float) juce::jlimit (0, 2, (int) ((lcg.next() + 1.0f) * 1.5f)));
        else if (x < 0.85f)
            h.proc.runAnalysisNow();
        else if (x < 0.88f)
            h.proc.setStateInformation (state.getData(), (int) state.getSize());
        else if (x < 0.9f)
            h.proc.getStateInformation (state);
        else if (x < 0.92f)
            h.proc.resetMixAnalysis();
        else
        {
            juce::ignoreUnused (h.proc.getReferenceInfo(), h.proc.getReferenceTrack(), h.proc.getComparison(),
                                h.proc.getMixProfile(), h.proc.getReferenceProfile());
        }

        ++ops;
        if (ops % 4 == 0)
            juce::Thread::sleep (1);
    }

    const bool idle = h.proc.waitUntilReferenceIdle (30000);
    stop.store (true);
    audio.join();

    check (idle, "threads: loader idle again after the storm", str (ops, " operations, ", blocks.load(), " audio blocks"));
    check (finite.load() && blocks.load() > 100, "threads: audio thread kept running with finite output");

    check (h.load (a.file) && h.proc.getReferenceInfo().state == RefMatchProcessor::ReferenceState::Ready,
           "threads: still loads normally afterwards");
}

//==============================================================================
// The allocation counter must see JUCE's malloc-based allocations (HeapBlock) as well as operator new, or the final
// "no allocation" check could pass while processBlock allocates. Restores the counters afterwards.
void testAllocationCounter()
{
    std::cout << "\n-- Allocation counter\n";

    const auto hooked = rmdev::installHeapHooks();
    check (hooked.find ("malloc") != std::string::npos && hooked.find ("free") != std::string::npos,
           "realtime: CRT heap functions hooked (" + hooked + ")", "no import of malloc / free found (static CRT?)");

    const auto saved = rmdev::getAllocationCounts();
    volatile float sink = 0.0f;

    rmdev::setAllocationTracking (true);
    {
        juce::AudioBuffer<float> buffer;   // HeapBlock -> std::malloc / std::free
        buffer.setSize (2, 64);
        buffer.setSample (1, 63, 0.5f);
        juce::Array<int> array;            // HeapBlock -> std::realloc / std::free
        array.add (1);
        sink = sink + buffer.getSample (1, 63) + (float) array[0];
    }
    rmdev::setAllocationTracking (false);
    const auto heap = rmdev::getAllocationCounts();

    rmdev::setAllocationTracking (true);
    {
        auto vector = std::make_unique<std::vector<int>> (16, 1);   // operator new / delete
        sink = sink + (float) (*vector)[3];
    }
    rmdev::setAllocationTracking (false);
    const auto all = rmdev::getAllocationCounts();

    check (heap.allocations - saved.allocations >= 2 && heap.frees - saved.frees >= 2,
           "realtime: allocation counter sees JUCE HeapBlock malloc / free",
           str (heap.allocations - saved.allocations, " allocations, ", heap.frees - saved.frees, " frees"));
    check (all.allocations - heap.allocations >= 2 && all.frees - heap.frees >= 2,
           "realtime: allocation counter sees operator new / delete",
           str (all.allocations - heap.allocations, " allocations, ", all.frees - heap.frees, " frees"));

    rmdev::setAllocationCounts (saved);
    juce::ignoreUnused (sink);
}
} // namespace

//==============================================================================
int runSelfTest()
{
    // A folder of its own for every run: two self-tests at the same time must not delete each other's files.
    tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("RefMatchSelfTest-" + juce::Uuid().toDashedString());
    tempDir.createDirectory();
    std::cout << "selftest: temporary files in " << tempDir.getFullPathName() << "\n";

    const double start = juce::Time::getMillisecondCounterHiRes();

    testAllocationCounter();
    testLoad();
    testSwitching();
    testPassthrough();
    testGainMatching();
    testTransport();
    testMono();
    testState();
    testSampleRateChange();
    testAnalysis();
    testTransportAnalysis();
    testShortTermMatching();
    testListenWithoutReference();
    testSlotLock();
    testThreadSafety();

    std::cout << "\n-- Realtime safety\n";
    const auto counts = rmdev::getAllocationCounts();
    check (counts.allocations == 0 && counts.frees == 0,
           "realtime: no allocation / free (operator new or CRT heap) inside processBlock in any test",
           str (counts.allocations, " allocation calls, ", counts.frees, " frees"));

    tempDir.deleteRecursively();

    std::cout << "\nselftest: " << numPassed << " passed, " << numFailed << " failed ("
              << juce::String ((juce::Time::getMillisecondCounterHiRes() - start) / 1000.0, 1) << " s)\n";
    return numFailed == 0 ? 0 : 1;
}
