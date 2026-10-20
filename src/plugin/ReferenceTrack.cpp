#include "plugin/ReferenceTrack.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

// Reference decoding (JUCE readers), sample-rate conversion and offline analysis. Runs on the processor's loader
// thread only.
//
// Resampling: band-limited interpolation with a Kaiser-windowed sinc (48 zero crossings per side, beta 9.5,
// ~ -95 dB stopband). The cut-off sits at 95 % of the lower of the two Nyquist frequencies, so downsampling
// (e.g. a 96 kHz file into a 48 kHz session) is properly anti-aliased and upsampling does not create images. The
// kernel is precomputed as a polyphase table: exact phases when the two rates have a small rational ratio
// (44.1 <-> 48 kHz: 160 / 147 phases), otherwise 4096 quantised phases (timing error < 1/8000 sample).
namespace rm
{
namespace
{
constexpr int kDecodeChunk = 1 << 16;    // samples per read() call (cancellation granularity)
constexpr int kResampleChunk = 1 << 15;  // output samples between cancellation checks
constexpr int kAnalysisChunk = 1 << 16;  // samples per analysis push (cancellation granularity)
constexpr double kMinReferenceSeconds = 0.5;   // shortest usable reference (= the shortest loop region)

double besselI0 (double x) noexcept
{
    // Power series of the modified Bessel function of the first kind, order 0 (converges quickly for x < 30).
    double sum = 1.0, term = 1.0;
    const double q = x * x * 0.25;

    for (int k = 1; k < 64; ++k)
    {
        term *= q / ((double) k * (double) k);
        sum += term;
        if (term < sum * 1.0e-16)
            break;
    }

    return sum;
}

class SincResampler
{
public:
    static constexpr double kZeroCrossings = 48.0;
    static constexpr double kKaiserBeta = 9.5;
    static constexpr double kRolloff = 0.95;
    static constexpr long long kMaxExactPhases = 4096;

    SincResampler (double inRate, double outRate)
        : ratio (inRate / outRate)
    {
        // Exact rational ratio when both rates are integers with a small common denominator.
        const auto inInt = (long long) std::llround (inRate), outInt = (long long) std::llround (outRate);

        if (std::abs ((double) inInt - inRate) < 1.0e-9 && std::abs ((double) outInt - outRate) < 1.0e-9
            && inInt > 0 && outInt > 0)
        {
            const auto g = std::gcd (inInt, outInt);
            if (outInt / g <= kMaxExactPhases)
            {
                exact = true;
                upFactor = outInt / g;     // L: output steps per input "period"
                downFactor = inInt / g;    // M
            }
        }

        numPhases = exact ? (int) upFactor : (int) kMaxExactPhases;

        cutoff = kRolloff * std::min (1.0, outRate / inRate);    // fraction of the input Nyquist
        halfTaps = (int) std::ceil (kZeroCrossings / cutoff) + 1;
        numTaps = 2 * halfTaps;

        const double i0Beta = besselI0 (kKaiserBeta);
        phases.assign ((size_t) numPhases * (size_t) numTaps, 0.0f);
        std::vector<double> tmp ((size_t) numTaps);

        for (int p = 0; p < numPhases; ++p)
        {
            const double frac = (double) p / (double) numPhases;
            float* w = phases.data() + (size_t) p * (size_t) numTaps;
            double sum = 0.0;

            for (int j = 0; j < numTaps; ++j)
            {
                // Distance (in input samples) between the output position and input tap j.
                const double d = frac + (double) (halfTaps - 1) - (double) j;
                const double u = cutoff * d;
                double v = 0.0;

                if (std::abs (u) < kZeroCrossings)
                {
                    const double sinc = std::abs (u) < 1.0e-12 ? 1.0
                                                               : std::sin (juce::MathConstants<double>::pi * u)
                                                                     / (juce::MathConstants<double>::pi * u);
                    const double r = u / kZeroCrossings;
                    const double window = besselI0 (kKaiserBeta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
                    v = cutoff * sinc * window;
                }

                tmp[(size_t) j] = v;
                sum += v;
            }

            // Unity DC gain for every phase.
            for (int j = 0; j < numTaps; ++j)
                w[j] = (float) (sum != 0.0 ? tmp[(size_t) j] / sum : 0.0);
        }
    }

    long long outputLength (long long numInput) const noexcept
    {
        if (exact)
            return (numInput * upFactor + downFactor - 1) / downFactor;

        return (long long) std::ceil ((double) numInput / ratio - 1.0e-9);
    }

    // Resamples output samples [outStart, outEnd) of `numChannels` channels.
    void process (const float* const* in, long long numInput, float* const* out, int numChannels,
                  long long outStart, long long outEnd) const noexcept
    {
        for (long long n = outStart; n < outEnd; ++n)
        {
            long long i0 = 0;
            int phase = 0;
            position (n, i0, phase);

            const float* w = phases.data() + (size_t) phase * (size_t) numTaps;
            const long long first = i0 - halfTaps + 1;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* x = in[ch];
                double acc = 0.0;

                if (first >= 0 && first + numTaps <= numInput)
                {
                    const float* xs = x + first;
                    float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
                    int j = 0;

                    for (; j + 4 <= numTaps; j += 4)
                    {
                        a0 += w[j] * xs[j];
                        a1 += w[j + 1] * xs[j + 1];
                        a2 += w[j + 2] * xs[j + 2];
                        a3 += w[j + 3] * xs[j + 3];
                    }

                    for (; j < numTaps; ++j)
                        a0 += w[j] * xs[j];

                    acc = (double) a0 + (double) a1 + (double) a2 + (double) a3;
                }
                else
                {
                    // Near the edges: zero padding outside the input.
                    for (int j = 0; j < numTaps; ++j)
                    {
                        const long long k = first + j;
                        if (k >= 0 && k < numInput)
                            acc += (double) w[j] * (double) x[k];
                    }
                }

                out[ch][n] = (float) acc;
            }
        }
    }

private:
    void position (long long n, long long& i0, int& phase) const noexcept
    {
        if (exact)
        {
            const long long num = n * downFactor;
            i0 = num / upFactor;
            phase = (int) (num % upFactor);
            return;
        }

        const double t = (double) n * ratio;
        i0 = (long long) std::floor (t);
        phase = (int) std::lround ((t - (double) i0) * numPhases);

        if (phase >= numPhases)
        {
            phase = 0;
            ++i0;
        }
    }

    double ratio = 1.0;        // input samples per output sample
    bool exact = false;
    long long upFactor = 1, downFactor = 1;
    int numPhases = 1;
    double cutoff = 1.0;
    int halfTaps = 1, numTaps = 2;
    std::vector<float> phases;   // numPhases x numTaps
};

bool isCancelled (const std::function<bool()>& shouldExit)
{
    return shouldExit != nullptr && shouldExit();
}

// rm::shortTermLoudnessCurve (one value per completed 100 ms meter step) of the stereo signal and of its mono
// fold-down, in one pass and with cancellation. Returns false when cancelled.
bool buildShortTermCurves (const float* left, const float* right, long long numSamples, double sampleRate,
                           std::vector<float>& stereo, std::vector<float>& mono,
                           const std::function<bool()>& shouldExit)
{
    LoudnessMeter stereoMeter, monoMeter;
    stereoMeter.prepare (sampleRate);
    monoMeter.prepare (sampleRate);

    const int step = stereoMeter.getSamplesPerStep();
    const long long numSteps = numSamples / step;
    std::vector<float> fold ((size_t) step);
    stereo.clear();
    mono.clear();
    stereo.reserve ((size_t) numSteps);
    mono.reserve ((size_t) numSteps);

    for (long long s = 0; s < numSteps; ++s)
    {
        if (s % 64 == 0 && isCancelled (shouldExit))
            return false;

        const long long offset = s * step;

        for (int i = 0; i < step; ++i)
            fold[(size_t) i] = (left[offset + i] + right[offset + i]) * 0.5f;

        stereoMeter.process (left + offset, right + offset, step);
        monoMeter.process (fold.data(), nullptr, step);
        stereo.push_back (stereoMeter.shortTermLufs());
        mono.push_back (monoMeter.shortTermLufs());
    }

    return true;
}

std::pair<long long, long long> regionBounds (const ReferenceTrack& track, double startSeconds, double endSeconds)
{
    const long long length = track.numSamples();
    long long start = 0, end = length;

    if (endSeconds > startSeconds)
    {
        start = juce::jlimit (0LL, length, (long long) std::llround (startSeconds * track.sampleRate));
        end = juce::jlimit (start, length, (long long) std::llround (endSeconds * track.sampleRate));
    }

    return { start, end };
}
} // namespace

//==============================================================================
void registerReferenceFormats (juce::AudioFormatManager& manager)
{
    // WAV, AIFF, FLAC, Ogg Vorbis and MP3 (JUCE_USE_FLAC / JUCE_USE_OGGVORBIS / JUCE_USE_MP3AUDIOFORMAT are on).
    manager.registerBasicFormats();
}

std::shared_ptr<const ReferenceTrack> loadReferenceTrack (const juce::File& file, double targetSampleRate,
                                                          juce::String& error,
                                                          const std::function<bool()>& shouldExit)
{
    error = {};

    if (! file.existsAsFile())
    {
        error = "File not found";
        return nullptr;
    }

    juce::AudioFormatManager formats;
    registerReferenceFormats (formats);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));

    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels == 0)
    {
        error = "Unsupported or damaged audio file";
        return nullptr;
    }

    const double fileRate = reader->sampleRate;
    const long long fileLength = reader->lengthInSamples;

    if (fileLength <= 0)
    {
        error = "The file contains no audio";
        return nullptr;
    }

    if ((double) fileLength / fileRate < kMinReferenceSeconds)
    {
        error = "File is shorter than half a second";
        return nullptr;
    }

    if ((double) fileLength / fileRate > kMaxReferenceSeconds + 1.0
        || fileLength > (long long) std::numeric_limits<int>::max())
    {
        error = "File is longer than 20 minutes";
        return nullptr;
    }

    const double outRate = targetSampleRate > 0.0 ? targetSampleRate : fileRate;
    const bool resample = std::abs (outRate - fileRate) > 1.0e-6;

    auto track = std::make_shared<ReferenceTrack>();
    track->file = file;
    track->displayName = file.getFileNameWithoutExtension();
    track->fileSampleRate = fileRate;
    track->fileChannels = (int) reader->numChannels;
    track->sampleRate = outRate;

    // ---- Decode (always 2 channels: mono files are duplicated, extra channels ignored) ----------------------------
    juce::AudioBuffer<float> decoded;

    try
    {
        decoded.setSize (2, (int) fileLength);
    }
    catch (const std::bad_alloc&)
    {
        error = "Not enough memory to load this file";
        return nullptr;
    }

    // A damaged floating-point file can contain NaN / inf samples. They would play straight to the host's output (and
    // the resampler would smear each one over ~100 samples), so they become silence here, before resampling.
    long long nonFinite = 0;

    for (long long pos = 0; pos < fileLength; pos += kDecodeChunk)
    {
        if (isCancelled (shouldExit))
            return nullptr;

        const int n = (int) std::min<long long> (kDecodeChunk, fileLength - pos);
        if (! reader->read (&decoded, (int) pos, n, pos, true, true))
        {
            error = "Unsupported or damaged audio file";
            return nullptr;
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            float* d = decoded.getWritePointer (ch, (int) pos);

            for (int i = 0; i < n; ++i)
            {
                if (! std::isfinite (d[i]))
                {
                    d[i] = 0.0f;
                    ++nonFinite;
                }
            }
        }
    }

    reader.reset();

    // More than a few damaged samples (> 1 %): the file is not usable as a reference.
    if (nonFinite * 100 > 2 * fileLength)
    {
        error = "Unsupported or damaged audio file";
        return nullptr;
    }

    // ---- Resample to the host rate --------------------------------------------------------------------------------
    if (resample)
    {
        const SincResampler resampler (fileRate, outRate);
        const long long outLength = resampler.outputLength (fileLength);

        if (outLength <= 0 || outLength > (long long) std::numeric_limits<int>::max())
        {
            error = "Unsupported or damaged audio file";
            return nullptr;
        }

        try
        {
            track->audio.setSize (2, (int) outLength);
        }
        catch (const std::bad_alloc&)
        {
            error = "Not enough memory to load this file";
            return nullptr;
        }

        const float* in[2] = { decoded.getReadPointer (0), decoded.getReadPointer (1) };
        float* out[2] = { track->audio.getWritePointer (0), track->audio.getWritePointer (1) };

        for (long long pos = 0; pos < outLength; pos += kResampleChunk)
        {
            if (isCancelled (shouldExit))
                return nullptr;

            resampler.process (in, fileLength, out, 2, pos, std::min<long long> (outLength, pos + kResampleChunk));
        }

        decoded.setSize (0, 0);
    }
    else
    {
        track->audio = std::move (decoded);
    }

    if (isCancelled (shouldExit))
        return nullptr;

    // ---- Analysis needed for drawing / short-term matching ----------------------------------------------------------
    const float* left = track->audio.getReadPointer (0);
    const float* right = track->audio.getReadPointer (1);
    const long long length = track->numSamples();

    if (! buildShortTermCurves (left, right, length, outRate, track->shortTermCurve, track->monoShortTermCurve,
                                shouldExit))
        return nullptr;

    track->overview = makeWaveformOverview (left, right, length, ReferenceTrack::kOverviewColumns);
    return track;
}

TrackProfile analyseReferenceRegion (const ReferenceTrack& track, double startSeconds, double endSeconds)
{
    if (track.numSamples() <= 0 || track.sampleRate <= 0.0)
        return {};

    const auto [start, end] = regionBounds (track, startSeconds, endSeconds);

    if (end <= start)
        return {};

    return analyseRegion (track.audio.getReadPointer (0), track.audio.getReadPointer (1), start, end, track.sampleRate);
}

bool analyseReferenceRegion (const ReferenceTrack& track, double startSeconds, double endSeconds,
                             RegionAnalysis& result, const std::function<bool()>& shouldExit)
{
    result = {};

    if (track.numSamples() <= 0 || track.sampleRate <= 0.0)
        return true;

    const auto [start, end] = regionBounds (track, startSeconds, endSeconds);

    if (end <= start)
        return true;

    // Same as rm::analyseRegion (SinceReset averaging, pushed in chunks), plus the mono fold-down's loudness.
    TrackAnalyser analyser;
    analyser.prepare (track.sampleRate, Averaging::SinceReset);
    LoudnessMeter monoMeter;
    monoMeter.prepare (track.sampleRate);

    const float* left = track.audio.getReadPointer (0);
    const float* right = track.audio.getReadPointer (1);
    std::vector<float> fold ((size_t) kAnalysisChunk);

    for (long long pos = start; pos < end; pos += kAnalysisChunk)
    {
        if (isCancelled (shouldExit))
            return false;

        const int n = (int) std::min<long long> (kAnalysisChunk, end - pos);
        analyser.push (left + pos, right + pos, n);

        for (int i = 0; i < n; ++i)
            fold[(size_t) i] = (left[pos + i] + right[pos + i]) * 0.5f;

        monoMeter.process (fold.data(), nullptr, n);
    }

    result.profile = analyser.profile();
    result.monoIntegratedLufs = monoMeter.integratedLufs();
    return true;
}

} // namespace rm
