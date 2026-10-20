#include "plugin/PluginProcessor.h"

#include "core/Loudness.h"
#include "ui/PluginEditor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#if JUCE_INTEL
 #include <immintrin.h>
#endif

//==============================================================================
// RefMatchProcessor implementation (threads and behaviour: ARCHITECTURE.md, "Processor").
//
// Locks (a thread may take a lock further down this list while holding one further up, never the reverse):
//   stateMutex     reference info, loader jobs, the UI's track pointer, load / region serials, the host rate
//                  (message, loader and analysis threads)
//   analysisMutex  the mix analyser, the reading side of the FIFO and the matching-gain state (analysis thread,
//                  runAnalysisNow, reset)
//   resultsMutex   published profiles and comparison
//   slotLock       juce::SpinLock around the tracks + region the audio thread plays. Other threads take it only for
//                  a track hand-over, a region change and, while replaced tracks are waiting to be freed, to free
//                  them (never in steady state). The audio thread only ever try-locks it, retrying briefly; when that
//                  still fails the reference is silent for that one block.
//
// The audio thread never owns a track. A replaced track is parked in Slot::retired so the audio thread can fade out
// of it, and is released by another thread (analysis / loader / message) once the audio thread has marked it
// releasable (no voice reads it any more).
//
// Reference playback uses a small pool of voices (read heads). The main voice is what should play; every
// discontinuity (host jump, loop wrap, seek, region or track change, start / stop) starts a new main voice that fades
// in over kJumpFadeMs while the previous ones fade out from wherever their equal-power weight currently is. So
// continuous playback is sample-exact, and even rapid jumps (scrubbing) never cut a voice off.
//==============================================================================

namespace
{
constexpr const char* kStateTag = "RefMatch";
constexpr const char* kParamsTag = "PARAMS";
constexpr const char* kReferenceTag = "Reference";
constexpr int kStateVersion = 1;

constexpr int kAnalysisIntervalMs = 15;
constexpr double kPublishIntervalMs = 30.0;
constexpr double kFifoSeconds = 2.0;
constexpr double kFallbackSampleRate = 44100.0;   // loads requested before prepareToPlay (re-loaded if it differs)
constexpr int kMinChunk = 1024;                   // reference scratch size at least this many samples
constexpr int kThreadStopTimeoutMs = 10000;
constexpr int kMaxVoices = 8;                     // enough for a jump every 64 samples (8 ms fades)
constexpr int kMaxRetired = kMaxVoices;           // replaced tracks kept for their fade-out: no more generations
                                                  // than voices can be audible, so none still needed is evicted
constexpr long long kNoSilence = std::numeric_limits<long long>::min();

// Host positions that differ from the reference's own continuation by up to this much are not treated as jumps.
// Hosts and wrappers can report a timeInSamples that wobbles by a sample or two (rounding, positions derived from
// seconds or ppq); a crossfade between two read heads a sample apart would sum to +3 dB, and jumps closer together
// than the fade would pull the level down. A few ms of offset against a different song is inaudible; larger
// differences (and drift that accumulates beyond this) are real jumps and resync with one crossfade.
constexpr double kHostSyncToleranceMs = 5.0;

// Short-term matching uses a 3 s window only once it holds this much continuous audio (3 s plus the partial 100 ms
// steps at either end), so it never compares a window that is partly silence or partly from before a stop.
constexpr double kShortTermValidSeconds = 3.2;

// A 100 ms step whose mean square is below this (-70 dBFS) is silence, not programme: short-term matching holds
// instead of comparing a window that is fading out or partly silent (input stopped, a noise floor, a break).
constexpr double kSilentMeanSquare = 1.0e-7;

// Attempts at the slot try-lock per block before the reference is silenced for that block. Other holders only run
// short, allocation-free sections, so a brief retry covers them unless their thread was pre-empted.
constexpr int kSlotLockAttempts = 32;

long long floorMod (long long value, long long modulus) noexcept
{
    const auto r = value % modulus;
    return r < 0 ? r + modulus : r;
}

inline void cpuRelax() noexcept
{
   #if JUCE_INTEL
    _mm_pause();
   #endif
}

// Audio thread: length of the current run of continuous, non-silent audio, judged in exact 100 ms steps (a step
// whose mean square is below kSilentMeanSquare, digital silence or a noise floor, ends the run like a gap does).
struct RunCounter
{
    long long run = 0;          // samples since the last gap or silent step
    double stepEnergy = 0.0;    // mean-square sum (averaged over the channels) of the current step
    int stepSamples = 0;

    void gap() noexcept { *this = RunCounter(); }

    void add (const float* const* channels, int numChannels, int numSamples, int stepLength) noexcept
    {
        for (int done = 0; done < numSamples;)
        {
            const int n = juce::jmin (numSamples - done, stepLength - stepSamples);
            double sum = 0.0;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float* x = channels[ch] + done;
                for (int i = 0; i < n; ++i)
                    sum += (double) x[i] * (double) x[i];
            }

            stepEnergy += sum / (double) numChannels;
            stepSamples += n;
            run += n;
            done += n;

            if (stepSamples >= stepLength)
            {
                if (! (stepEnergy >= kSilentMeanSquare * (double) stepSamples))   // also NaN
                    run = 0;

                stepEnergy = 0.0;
                stepSamples = 0;
            }
        }
    }
};

// table[k] = sin (pi/2 * k / length), k = 0..length. Equal-power fade: fade-in weight table[k], fade-out weight
// table[length - k] (= cos). The end points are exactly 0 and 1.
std::vector<float> makeQuarterSine (int length)
{
    std::vector<float> table ((size_t) length + 1);

    for (int k = 0; k <= length; ++k)
        table[(size_t) k] = (float) std::sin (juce::MathConstants<double>::halfPi * (double) k / (double) length);

    table.front() = 0.0f;
    table.back() = 1.0f;
    return table;
}

int samplesForMs (double ms, double sampleRate)
{
    return juce::jmax (1, (int) std::lround (ms * sampleRate / 1000.0));
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { RefMatchProcessor::kParamListen, 1 }, "Listen to reference", false));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { RefMatchProcessor::kParamMatch, 1 }, "Level match",
                                                              juce::StringArray { "Off", "Integrated", "Short-term" }, 1));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { RefMatchProcessor::kParamMono, 1 }, "Mono check", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { RefMatchProcessor::kParamFreeRun, 1 }, "Play reference when stopped", true));
    layout.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { RefMatchProcessor::kParamAveraging, 1 }, "Analysis window",
                                                              juce::StringArray { "Recent (30 s)", "Since reset" }, 0));
    return layout;
}

using TrackPtr = std::shared_ptr<const rm::ReferenceTrack>;

// Tracks taken out of shared state while holding locks. Declared before the lock in a scope, so they are destroyed
// after it is released (and never on the audio thread).
struct Graveyard
{
    std::array<TrackPtr, 2 * kMaxRetired + 2> tracks;
    size_t count = 0;

    void add (TrackPtr t)
    {
        if (t != nullptr && count < tracks.size())
            tracks[count++] = std::move (t);
    }
};
} // namespace

//==============================================================================
struct RefMatchProcessor::Impl
{
    struct LoopRegion
    {
        bool hasLoop = false;
        double start = 0.0, end = 0.0;   // seconds; ignored when hasLoop == false (whole file)

        bool operator== (const LoopRegion& o) const noexcept
        {
            return hasLoop == o.hasLoop && (! hasLoop || (start == o.start && end == o.end));
        }
    };

    struct LoadJob
    {
        juce::File file;
        double sampleRate = 0.0;
        LoopRegion loop;
        juce::uint64 serial = 0;
    };

    struct RegionJob
    {
        juce::uint32 trackGen = 0;
        LoopRegion loop;
        juce::uint64 serial = 0;
    };

    // What the audio thread plays. Guarded by slotLock.
    struct Slot
    {
        struct Retired
        {
            TrackPtr track;
            juce::uint32 gen = 0;
            bool releasable = false;   // set by the audio thread once no voice reads it
        };

        TrackPtr current;
        juce::uint32 currentGen = 0;
        long long regionStart = 0, regionEnd = 0;   // samples of `current`; regionEnd > regionStart when it exists
        juce::uint32 regionGen = 0;
        std::array<Retired, kMaxRetired> retired {};
        juce::uint32 audioSeenGen = 0;   // currentGen at the audio thread's last block (older tracks it never saw
                                         // cannot be in any voice)
    };

    struct Target
    {
        juce::uint32 gen = 0;   // generation of the track to read (not in the slot any more = silence)
        long long pos = 0;      // sample index in that track (outside the file = silence)
        bool active = false;    // false = silent, position holds
    };

    struct Voice
    {
        Target head;
        bool used = false;
        bool rising = true;     // fading in (or settled) vs fading out
        int fade = 0;           // 0..jumpLength; weight = jumpTable[fade]
        long long silentBefore = kNoSilence;   // a rising voice stays silent (fade on hold) below this position
    };

    struct TrackView
    {
        const float* left = nullptr;
        const float* right = nullptr;
        long long length = 0;

        void read (long long pos, float& l, float& r) const noexcept
        {
            if (pos >= 0 && pos < length)
            {
                l = left[pos];
                r = right[pos];
            }
        }
    };

    struct HostPosition
    {
        bool playing = false;
        bool hasTime = false;
        bool exact = false;     // from timeInSamples
        long long sample = 0;   // host position in samples at the start of the block
    };

    // Audio-thread-only playback state.
    struct AudioState
    {
        std::array<Voice, kMaxVoices> voices {};
        int mainVoice = 0;
        juce::uint32 seenTrackGen = 0;
        int seenSeekCount = 0;
        long long seekOffset = 0;   // host-follow offset in samples (seekReference while playing)
        int listenStep = 0;         // 0 = mix .. listenLength = reference
        int monoStep = 0;           // 0 = stereo .. monoLength = mono
        float lastTargetDb = 0.0f;
        bool mainSounding = false;
        bool haveTrack = false;     // a playable track is installed (else the A/B target is Mix)
        bool transportSeen = false; // the playhead has reported playing since prepare: from then on the analysis is
                                    // fed only while it plays (stopped-transport silence is not part of the mix)
        RunCounter mixRun;              // mix fed to the analysis without a gap or a silent step
        RunCounter refRun;              // reference sounding without a gap or a silent step, on the current track
        long long refTrackSamples = 0;  // samples of the current track measured by refMeter
        juce::uint32 regionGen = 0;     // slot.regionGen at the last locked block
    };

    class WorkerThread final : public juce::Thread
    {
    public:
        WorkerThread (const juce::String& name, std::function<void()> bodyToRun)
            : juce::Thread (name), body (std::move (bodyToRun)) {}

        void run() override { body(); }

    private:
        std::function<void()> body;
    };

    //==============================================================================
    explicit Impl (RefMatchProcessor& p)
        : owner (p), apvts (p, nullptr, kParamsTag, createLayout())
    {
        listenParam = apvts.getRawParameterValue (kParamListen);
        matchParam = apvts.getRawParameterValue (kParamMatch);
        monoParam = apvts.getRawParameterValue (kParamMono);
        freeRunParam = apvts.getRawParameterValue (kParamFreeRun);
        averagingParam = apvts.getRawParameterValue (kParamAveraging);
        jassert (listenParam != nullptr && matchParam != nullptr && monoParam != nullptr
                 && freeRunParam != nullptr && averagingParam != nullptr);

        comparison = rm::compareProfiles (mixProfile, refProfile);
        listenTable = jumpTable = makeQuarterSine (1);
        resetAudioState();
        loaderThread.startThread (juce::Thread::Priority::low);
    }

    ~Impl()
    {
        loaderThread.signalThreadShouldExit();
        loaderThread.notify();
        analysisThread.stopThread (kThreadStopTimeoutMs);
        loaderThread.stopThread (kThreadStopTimeoutMs);
    }

    //==============================================================================
    // Parameters
    bool paramOn (const std::atomic<float>* p) const noexcept { return p->load (std::memory_order_relaxed) > 0.5f; }

    int paramIndex (const std::atomic<float>* p, int maxIndex) const noexcept
    {
        return juce::jlimit (0, maxIndex, (int) std::lround (p->load (std::memory_order_relaxed)));
    }

    rm::Averaging averagingFromParam() const noexcept { return (rm::Averaging) paramIndex (averagingParam, 1); }

    double targetSampleRate() const noexcept
    {
        const double rate = hostSampleRate.load();
        return rate > 0.0 ? rate : kFallbackSampleRate;
    }

    //==============================================================================
    // Audio thread
    void process (juce::AudioBuffer<float>& buffer) noexcept
    {
        const int numSamples = buffer.getNumSamples();
        const int channels = juce::jmin (buffer.getNumChannels(), numChannels);

        if (numSamples <= 0 || channels <= 0 || refScratch.getNumSamples() == 0)
            return;

        const auto host = readHostPosition();
        hostPlaying.store (host.playing, std::memory_order_relaxed);

        // While a host transport that has played is stopped (FL keeps calling processBlock with silence), the input is
        // not part of the mix: leave it out of the analysis, so stop / start does not add fades to silence to the
        // loudness range and integrated loudness. The meters then simply hold. Hosts whose playhead never reports
        // playing (JUCE's Standalone player, transport-less hosts) keep feeding everything, as does an offline render.
        audio.transportSeen = audio.transportSeen || host.playing;
        const bool feedAnalysis = owner.isNonRealtime() || host.playing || ! audio.transportSeen;

        // mixWindowFull tells the analysis thread whether the mix meter's last 3 s are continuous programme. A block
        // that ends the run (a silent step) clears it BEFORE its samples enter the FIFO; it is set only AFTER the
        // samples it counts are in the FIFO. So a drain that sees true both before and after it read only valid audio.
        if (feedAnalysis)
        {
            audio.mixRun.add (buffer.getArrayOfReadPointers(), channels, numSamples, silenceStepLength);

            if (audio.mixRun.run < shortTermValidSamples)
                mixWindowFull.store (false, std::memory_order_release);

            if (! pushToFifo (buffer, channels, numSamples))
                audio.mixRun.gap();
        }
        else
        {
            audio.mixRun.gap();
        }

        mixWindowFull.store (audio.mixRun.run >= shortTermValidSamples, std::memory_order_release);

        if (owner.isNonRealtime())
        {
            // Offline render: always the untouched input. The reference does not play, so its window goes stale.
            audio.refRun.gap();
            refMatchShortTermLufs.store (rm::kNoValue);
            return;
        }

        const bool listenTarget = paramOn (listenParam);
        const bool monoTarget = paramOn (monoParam);
        const bool freeRun = paramOn (freeRunParam);

        const float targetDb = targetGainDb.load (std::memory_order_relaxed);
        if (targetDb != audio.lastTargetDb)
        {
            audio.lastTargetDb = targetDb;
            gain.setTargetValue ((float) rm::dbToGain (targetDb));
        }

        const int capacity = refScratch.getNumSamples();

        for (int start = 0; start < numSamples; start += capacity)
        {
            const int n = juce::jmin (capacity, numSamples - start);
            auto chunkHost = host;
            chunkHost.sample += start;

            if (renderReference (n, chunkHost, freeRun))
                measureReference (n);
            else
                audio.refRun.gap();   // a silenced block is a gap in what plays

            // No installed track (none loaded, a load error, the reload after a rate change): the A/B target is Mix,
            // whatever the parameter says, so an empty reference never silences the user's master.
            mixChunk (buffer, channels, start, n, listenTarget && audio.haveTrack, monoTarget);
        }

        matchGainDb.store (rm::gainToDb (gain.getCurrentValue()), std::memory_order_relaxed);
        listenPosition.store ((float) audio.listenStep / (float) listenLength, std::memory_order_relaxed);
        referenceSounding.store (audio.mainSounding && audio.listenStep > 0, std::memory_order_relaxed);

        // Reference short-term loudness as heard. Stored before the region generation: a reader that sees a new
        // generation also sees the values that belong to it.
        const float refShortTerm = refMeter.shortTermLufs();
        refShortTermLufs.store (audio.refTrackSamples >= shortTermValidSamples ? refShortTerm : rm::kNoValue,
                                std::memory_order_relaxed);
        refMatchShortTermLufs.store (audio.refRun.run >= shortTermValidSamples
                                             && refShortTerm > rm::LoudnessMeter::kAbsoluteGateLufs
                                         ? refShortTerm
                                         : rm::kNoValue);
        audioRegionGen.store (audio.regionGen);
    }

    // Feeds the reference just rendered into refScratch (unity gain, before the A/B fade) to refMeter, exactly as it
    // is heard: stereo, or the (L+R)/2 fold-down as one channel on a mono bus (measured like the mono mix). Only while
    // the reference sounds, so a pause keeps the last value (like the mix meters while the host is stopped).
    // A silent step of either signal (the input stopped without a transport, e.g. in the Standalone app; a break in
    // the reference) ends its run, so a window that is fading out or partly silent is never compared.
    void measureReference (int n) noexcept
    {
        if (! audio.mainSounding)
        {
            audio.refRun.gap();
            return;
        }

        const float* l = refScratch.getReadPointer (0);
        const float* r = refScratch.getReadPointer (1);

        if (numChannels == 1)
        {
            float* mono = monoScratch.data();
            for (int i = 0; i < n; ++i)
                mono[i] = (l[i] + r[i]) * 0.5f;

            refMeter.process (mono, nullptr, n);
            const float* channel[] = { mono };
            audio.refRun.add (channel, 1, n, silenceStepLength);
        }
        else
        {
            refMeter.process (l, r, n);
            const float* channel[] = { l, r };
            audio.refRun.add (channel, 2, n, silenceStepLength);
        }

        audio.refTrackSamples += n;
    }

    // False when the FIFO is full (the analysis thread is behind): the block is dropped.
    bool pushToFifo (const juce::AudioBuffer<float>& buffer, int channels, int numSamples) noexcept
    {
        if (fifoData[0] == nullptr || fifo.getFreeSpace() < numSamples)
        {
            droppedBlocks.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        const float* l = buffer.getReadPointer (0);
        const float* r = buffer.getReadPointer (channels > 1 ? 1 : 0);
        const auto scope = fifo.write (numSamples);

        auto copy = [&] (int fifoStart, int size, int sourceOffset)
        {
            if (size <= 0)
                return;

            juce::FloatVectorOperations::copy (fifoData[0] + fifoStart, l + sourceOffset, size);
            juce::FloatVectorOperations::copy (fifoData[1] + fifoStart, r + sourceOffset, size);
        };

        copy (scope.startIndex1, scope.blockSize1, 0);
        copy (scope.startIndex2, scope.blockSize2, scope.blockSize1);
        return true;
    }

    HostPosition readHostPosition() const noexcept
    {
        HostPosition h;

        if (auto* ph = owner.getPlayHead())
        {
            if (const auto pos = ph->getPosition())
            {
                h.playing = pos->getIsPlaying();

                if (const auto samples = pos->getTimeInSamples())
                {
                    h.sample = *samples;
                    h.hasTime = h.exact = true;
                }
                else if (const auto seconds = pos->getTimeInSeconds())
                {
                    h.sample = (long long) std::llround (*seconds * sampleRate);
                    h.hasTime = true;
                }
                else if (const auto ppq = pos->getPpqPosition())
                {
                    if (const auto bpm = pos->getBpm(); bpm.hasValue() && *bpm > 0.0)
                    {
                        h.sample = (long long) std::llround (*ppq * 60.0 / *bpm * sampleRate);
                        h.hasTime = true;
                    }
                }
            }
        }

        return h;
    }

    TrackView viewOf (juce::uint32 gen) const noexcept   // slotLock held
    {
        const rm::ReferenceTrack* t = nullptr;

        if (gen != 0 && gen == slot.currentGen)
            t = slot.current.get();
        else if (gen != 0)
            for (auto& r : slot.retired)
                if (r.track != nullptr && r.gen == gen)
                    t = r.track.get();

        if (t == nullptr)
            return {};

        return { t->audio.getReadPointer (0), t->audio.getReadPointer (1), t->numSamples() };
    }

    Voice& mainVoice() noexcept { return audio.voices[(size_t) audio.mainVoice]; }

    void resetAudioState() noexcept
    {
        audio = AudioState();
        audio.voices[0].used = true;
        audio.voices[0].fade = jumpLength;
    }

    // Starts a new main voice at `target`; the current main fades out from its present weight. Returns the new
    // voice's index (its TrackView must be refreshed).
    int startJump (const Target& target, long long silentBefore = kNoSilence) noexcept
    {
        auto& current = mainVoice();

        if (current.head.active && current.fade > 0)
            current.rising = false;
        else
            current.used = false;

        int index = -1;

        for (int i = 0; i < kMaxVoices && index < 0; ++i)
            if (! audio.voices[(size_t) i].used)
                index = i;

        if (index < 0)
        {
            // Every voice busy: only with kMaxVoices jumps inside one fade (e.g. 192, 96, 48 ... samples apart).
            // Replace the quietest one; its weight is tiny by then (about 1 % with 8 voices).
            index = 0;
            for (int i = 1; i < kMaxVoices; ++i)
                if (audio.voices[(size_t) i].fade < audio.voices[(size_t) index].fade)
                    index = i;
        }

        auto& v = audio.voices[(size_t) index];
        v.head = target;
        v.used = true;
        v.rising = true;
        v.fade = 0;
        v.silentBefore = silentBefore;
        audio.mainVoice = index;
        return index;
    }

    // Renders n samples of the (unity gain, stereo) reference into refScratch and advances the voices. Returns false
    // when the slot could not be locked (the chunk is silent, nothing advanced).
    bool renderReference (int n, const HostPosition& host, bool freeRun) noexcept
    {
        float* outL = refScratch.getWritePointer (0);
        float* outR = refScratch.getWritePointer (1);

        const juce::SpinLock::ScopedTryLockType lock (slotLock);

        for (int attempt = 1; attempt < kSlotLockAttempts && ! lock.isLocked(); ++attempt)
        {
            cpuRelax();
            lock.retryLock();
        }

        if (! lock.isLocked())
        {
            referenceDropouts.fetch_add (1, std::memory_order_relaxed);
            juce::FloatVectorOperations::clear (outL, n);
            juce::FloatVectorOperations::clear (outR, n);
            return false;   // audio.haveTrack keeps its value: one failed try-lock must not start an A/B fade
        }

        const rm::ReferenceTrack* track = slot.current.get();
        const juce::uint32 currentGen = slot.currentGen;
        const long long regionStart = slot.regionStart, regionEnd = slot.regionEnd;
        const long long regionLength = regionEnd - regionStart;
        const bool haveTrack = track != nullptr && regionLength > 0;
        audio.haveTrack = haveTrack;
        audio.regionGen = slot.regionGen;

        // Loop wrap point. When the file continues after the region the wrap crossfades from that continuation into
        // the region start. When the region ends at (or near) the end of the file there is nothing to continue with,
        // so the crossfade starts jumpLength samples early, from the region's last samples into the lead-in before
        // its start; positions in that lead-in [regionStart - jumpLength, regionStart) count as inside the region.
        // Without a lead-in either (region starts at the file start) the new voice stays silent until the region
        // start: the end fades out, then the start fades in.
        const bool lateWrap = (haveTrack && regionEnd + jumpLength <= track->numSamples())
                              || regionLength <= 2 * (long long) jumpLength;
        const long long wrapAt = lateWrap ? regionEnd : regionEnd - jumpLength;
        const long long leadIn = lateWrap ? 0 : jumpLength;
        auto inRegion = [&] (long long p) { return p >= regionStart - leadIn && p < regionEnd; };

        // A voice placed in a lead-in without audio stays silent until the region start (then fades in).
        auto silenceFor = [&] (long long p) { return p < regionStart && regionStart < jumpLength ? regionStart : kNoSilence; };

        if (currentGen != audio.seenTrackGen)
        {
            audio.seenTrackGen = currentGen;
            audio.seekOffset = 0;
            audio.refRun.gap();   // refMeter's window still holds the previous track
            audio.refTrackSamples = 0;
        }

        slot.audioSeenGen = currentGen;

        long long seekTarget = -1;
        const int seeks = seekCount.load (std::memory_order_acquire);

        if (seeks != audio.seenSeekCount)
        {
            audio.seenSeekCount = seeks;

            if (haveTrack)
                seekTarget = juce::jlimit (regionStart, regionEnd - 1,
                                           (long long) std::llround (seekSeconds.load() * track->sampleRate));
        }

        // Where the main voice should be now.
        const auto& mainHead = mainVoice().head;
        Target want { currentGen, mainHead.pos, false };
        bool jump = false;

        if (! haveTrack)
        {
            want.pos = 0;
            jump = mainHead.gen != currentGen || mainHead.active;
        }
        else if (host.playing && host.hasTime)
        {
            if (seekTarget >= 0)
                audio.seekOffset = floorMod (seekTarget - regionStart - host.sample, regionLength);

            want.pos = regionStart + floorMod (host.sample + audio.seekOffset, regionLength);
            want.active = true;

            if (mainHead.gen == currentGen && mainHead.active && inRegion (mainHead.pos))
            {
                // Same position modulo the region (the main voice may be in the lead-in). Host times can wobble by a
                // sample or more (rounding in hosts and wrappers, positions derived from seconds or ppq): differences
                // within syncTolerance keep playing continuously (see kHostSyncToleranceMs); larger ones jump.
                const long long d = floorMod (mainHead.pos - want.pos, regionLength);
                jump = juce::jmin (d, regionLength - d) > syncTolerance;
            }
            else
            {
                jump = true;
            }
        }
        else
        {
            if (mainHead.gen != currentGen || ! inRegion (want.pos))
                want.pos = regionStart;

            if (seekTarget >= 0)
                want.pos = seekTarget;

            want.active = host.playing || freeRun;
            jump = want.gen != mainHead.gen || want.active != mainHead.active || want.pos != mainHead.pos;
        }

        if (jump)
        {
            if (! want.active && ! mainHead.active)
            {
                mainVoice().head = want;   // silent: just move
                mainVoice().silentBefore = silenceFor (want.pos);
            }
            else
            {
                startJump (want, silenceFor (want.pos));
            }
        }

        std::array<TrackView, kMaxVoices> views;
        for (size_t v = 0; v < views.size(); ++v)
            if (audio.voices[v].used)
                views[v] = viewOf (audio.voices[v].head.gen);

        for (int i = 0; i < n; ++i)
        {
            float l = 0.0f, r = 0.0f;

            for (size_t v = 0; v < audio.voices.size(); ++v)
            {
                auto& voice = audio.voices[v];

                if (! voice.used)
                    continue;

                const bool audible = voice.head.active && voice.head.pos >= voice.silentBefore;

                if (audible)
                {
                    float vl = 0.0f, vr = 0.0f;
                    views[v].read (voice.head.pos, vl, vr);
                    const float w = jumpTable[(size_t) voice.fade];
                    l += vl * w;
                    r += vr * w;
                }

                if (voice.head.active)
                    ++voice.head.pos;

                if (voice.rising)
                {
                    if (audible && voice.fade < jumpLength)
                        ++voice.fade;
                }
                else if (--voice.fade <= 0)
                {
                    voice.used = false;
                }
            }

            outL[i] = l;
            outR[i] = r;

            const auto& m = mainVoice().head;

            if (haveTrack && m.active && m.gen == currentGen && m.pos >= wrapAt)
            {
                const long long pos = m.pos - regionLength;
                const int index = startJump ({ currentGen, pos, true }, silenceFor (pos));
                views[(size_t) index] = viewOf (currentGen);
            }
        }

        const auto& m = mainVoice().head;
        const bool onCurrent = haveTrack && m.gen == currentGen;
        audio.mainSounding = onCurrent && m.active;

        if (onCurrent)
        {
            const long long shown = m.pos < regionStart ? m.pos + regionLength : m.pos;   // lead-in = end of region
            playheadSeconds.store ((double) shown / track->sampleRate, std::memory_order_relaxed);
        }
        else if (! haveTrack)
        {
            playheadSeconds.store (0.0, std::memory_order_relaxed);
        }

        // Tell the other threads which replaced tracks may be released.
        for (auto& r : slot.retired)
        {
            if (r.track == nullptr)
                continue;

            bool inUse = false;
            for (auto& v : audio.voices)
                inUse = inUse || (v.used && v.head.active && v.head.gen == r.gen);

            r.releasable = ! inUse;
        }

        return true;
    }

    // A/B crossfade (+ matching gain) and mono fold-down of one chunk, in place.
    void mixChunk (juce::AudioBuffer<float>& buffer, int channels, int start, int n, bool listenTarget,
                   bool monoTarget) noexcept
    {
        const float* refL = refScratch.getReadPointer (0);
        const float* refR = refScratch.getReadPointer (1);

        if (audio.listenStep == 0 && ! listenTarget)
        {
            gain.skip (n);   // settled on the mix: the input stays untouched
        }
        else
        {
            float* outL = buffer.getWritePointer (0, start);
            float* outR = channels > 1 ? buffer.getWritePointer (1, start) : nullptr;

            for (int i = 0; i < n; ++i)
            {
                if (listenTarget)
                {
                    if (audio.listenStep < listenLength)
                        ++audio.listenStep;
                }
                else if (audio.listenStep > 0)
                {
                    --audio.listenStep;
                }

                const float g = gain.getNextValue();

                if (audio.listenStep == 0)
                    continue;

                const float refLeft = outR != nullptr ? refL[i] * g : (refL[i] + refR[i]) * 0.5f * g;
                const float refRight = refR[i] * g;

                if (audio.listenStep == listenLength)
                {
                    outL[i] = refLeft;
                    if (outR != nullptr)
                        outR[i] = refRight;
                }
                else
                {
                    const float wRef = listenTable[(size_t) audio.listenStep];
                    const float wMix = listenTable[(size_t) (listenLength - audio.listenStep)];
                    outL[i] = outL[i] * wMix + refLeft * wRef;
                    if (outR != nullptr)
                        outR[i] = outR[i] * wMix + refRight * wRef;
                }
            }
        }

        if (channels < 2)
        {
            audio.monoStep = monoTarget ? monoLength : 0;   // a mono bus is mono already
            return;
        }

        if (audio.monoStep == 0 && ! monoTarget)
            return;

        float* l = buffer.getWritePointer (0, start);
        float* r = buffer.getWritePointer (1, start);

        for (int i = 0; i < n; ++i)
        {
            if (monoTarget)
            {
                if (audio.monoStep < monoLength)
                    ++audio.monoStep;
            }
            else if (audio.monoStep > 0)
            {
                --audio.monoStep;
            }

            const float mid = (l[i] + r[i]) * 0.5f;

            if (audio.monoStep == monoLength)
            {
                l[i] = mid;
                r[i] = mid;
            }
            else if (audio.monoStep > 0)
            {
                const float m = (float) audio.monoStep / (float) monoLength;
                l[i] += m * (mid - l[i]);
                r[i] += m * (mid - r[i]);
            }
        }
    }

    //==============================================================================
    // Slot (track hand-over to the audio thread)

    // Puts `track` (nullptr = silence) into the slot. The replaced track is kept for its fade-out while an audio
    // thread runs; everything that may go is added to `graveyard` (released by the caller outside every lock).
    void installTrack (TrackPtr track, juce::uint32 gen, long long regionStart, long long regionEnd, Graveyard& graveyard)
    {
        const bool keepForFade = audioPrepared.load();
        const juce::SpinLock::ScopedLockType lock (slotLock);

        for (auto& r : slot.retired)
        {
            if (r.track != nullptr && (r.releasable || ! keepForFade))
            {
                graveyard.add (std::move (r.track));
                r = {};
            }
        }

        if (slot.current != nullptr)
        {
            // Kept only if the audio thread has seen it (a track replaced before any block played is in no voice).
            if (keepForFade && slot.audioSeenGen == slot.currentGen)
            {
                // A free entry, else the oldest (several hand-overs within one fade).
                auto* entry = &slot.retired[0];
                for (auto& r : slot.retired)
                    if (r.track == nullptr || (entry->track != nullptr && r.gen < entry->gen))
                        entry = &r;

                graveyard.add (std::move (entry->track));
                entry->track = std::move (slot.current);
                entry->gen = slot.currentGen;
                entry->releasable = false;
            }
            else
            {
                graveyard.add (std::move (slot.current));
            }
        }

        updateRetiredPending();
        slot.current = std::move (track);
        slot.currentGen = gen;
        slot.regionStart = regionStart;
        slot.regionEnd = regionEnd;
        ++slot.regionGen;
    }

    void setSlotRegion (long long regionStart, long long regionEnd)
    {
        const juce::SpinLock::ScopedLockType lock (slotLock);
        slot.regionStart = regionStart;
        slot.regionEnd = regionEnd;
        ++slot.regionGen;
    }

    // slotLock held. Written only under the lock, so the unlocked read in releaseRetiredTracks is just a hint: a stale
    // false delays freeing a track by one analysis pass, a stale true costs one lock (only right after a swap).
    void updateRetiredPending() noexcept
    {
        bool any = false;
        for (auto& r : slot.retired)
            any = any || r.track != nullptr;

        retiredPending.store (any, std::memory_order_release);
    }

    // Releases replaced tracks once the audio thread is done with them (or all, when no audio thread is running).
    // Called by the analysis thread every pass: with nothing parked it returns without touching slotLock, so in
    // steady state no other thread ever contends with the audio thread's try-lock.
    void releaseRetiredTracks (bool force)
    {
        if (! force && ! retiredPending.load (std::memory_order_acquire))
            return;

        Graveyard graveyard;
        const juce::SpinLock::ScopedLockType lock (slotLock);

        for (auto& r : slot.retired)
        {
            if (r.track != nullptr && (force || r.releasable))
            {
                graveyard.add (std::move (r.track));
                r = {};
            }
        }

        updateRetiredPending();
    }

    // Removes the current track (stateMutex held).
    void uninstallLocked (Graveyard& graveyard)
    {
        installTrack (nullptr, nextTrackGen++, 0, 0, graveyard);
        graveyard.add (std::move (uiTrack));
        uiTrack = nullptr;
        uiTrackGen = 0;
        wholeFile = {};
        wholeFileGen = 0;
    }

    //==============================================================================
    // Reference requests (message thread or any non-audio thread)
    static LoopRegion clampLoop (const rm::ReferenceTrack& track, const LoopRegion& loop)
    {
        const double length = track.lengthSeconds();

        if (! loop.hasLoop || ! std::isfinite (loop.start) || ! std::isfinite (loop.end) || length <= 0.0)
            return {};

        double s = juce::jlimit (0.0, length, juce::jmin (loop.start, loop.end));
        double e = juce::jlimit (0.0, length, juce::jmax (loop.start, loop.end));
        const double minLength = juce::jmin (RefMatchProcessor::kMinLoopSeconds, length);

        if (e - s < minLength)
        {
            e = s + minLength;

            if (e > length)
            {
                e = length;
                s = length - minLength;
            }
        }

        return { true, s, e };
    }

    static std::pair<long long, long long> regionSamples (const rm::ReferenceTrack& track, const LoopRegion& loop)
    {
        const long long n = track.numSamples();

        if (! loop.hasLoop || n <= 1)
            return { 0, n };

        const auto s = juce::jlimit (0LL, n - 1, (long long) std::llround (loop.start * track.sampleRate));
        const auto e = juce::jlimit (s + 1, n, (long long) std::llround (loop.end * track.sampleRate));
        return { s, e };
    }

    void bumpInfo() { info.version = ++infoVersion; }                          // stateMutex held
    void bumpRegionSerial() { regionSerialAtomic.store (++regionSerial); }    // stateMutex held

    void requestLoadLocked (const juce::File& file, const LoopRegion& loop)   // stateMutex held
    {
        loadSerialAtomic.store (++loadSerial);
        bumpRegionSerial();
        pendingRegion.reset();

        const double rate = targetSampleRate();
        pendingLoad = LoadJob { file, rate, loop, loadSerial };
        loadSampleRate = rate;
        loopForState = loop;

        info.state = ReferenceState::Loading;
        info.file = file;
        info.displayName = file.getFileNameWithoutExtension();
        info.error = {};
        bumpInfo();
        loaderThread.notify();
    }

    // A file the user picked / dropped. A new reference starts without a loop, unless it relinks the reference that
    // failed to load (Error state, e.g. "File not found" after the project moved to another PC or folder) under the
    // same file name: then the project's saved loop is kept (clampLoop adapts it to the new file's length). The old
    // path is never probed here (it could be a dead network share).
    void requestUserLoad (const juce::File& file)
    {
        const std::lock_guard<std::mutex> lock (stateMutex);

        const bool relink = info.state == ReferenceState::Error && info.file != juce::File()
                            && file.getFileName().equalsIgnoreCase (info.file.getFileName());

        requestLoadLocked (file, relink ? loopForState : LoopRegion {});
    }

    // Restores a reference from the state: re-uses the loaded track when it is the same file at the same rate.
    void restoreReference (const juce::File& file, const LoopRegion& loop)
    {
        const std::lock_guard<std::mutex> lock (stateMutex);

        if (info.state == ReferenceState::Ready && info.file == file && uiTrack != nullptr
            && uiTrack->sampleRate == targetSampleRate())
        {
            const auto clamped = clampLoop (*uiTrack, loop);
            if (! (clamped == loopForState))
                applyLoopLocked (clamped);
            return;
        }

        requestLoadLocked (file, loop);
    }

    void clear()
    {
        Graveyard graveyard;
        const std::lock_guard<std::mutex> lock (stateMutex);

        loadSerialAtomic.store (++loadSerial);
        bumpRegionSerial();
        pendingLoad.reset();
        pendingRegion.reset();
        uninstallLocked (graveyard);
        loopForState = {};
        loadSampleRate = 0.0;

        info = ReferenceInfo();
        bumpInfo();
        publishReferenceProfile ({});
    }

    void applyLoopLocked (const LoopRegion& loop)   // stateMutex held, uiTrack != nullptr
    {
        loopForState = loop;
        const auto [regionStart, regionEnd] = regionSamples (*uiTrack, loop);
        setSlotRegion (regionStart, regionEnd);

        info.hasLoop = loop.hasLoop;
        info.loopStart = loop.hasLoop ? loop.start : 0.0;
        info.loopEnd = loop.hasLoop ? loop.end : uiTrack->lengthSeconds();
        info.analysing = true;
        bumpInfo();

        bumpRegionSerial();
        pendingRegion = RegionJob { uiTrackGen, loop, regionSerial };
        loaderThread.notify();
    }

    void setLoop (const LoopRegion& requested)
    {
        const std::lock_guard<std::mutex> lock (stateMutex);

        if (info.state != ReferenceState::Ready || uiTrack == nullptr)
            return;

        const auto loop = clampLoop (*uiTrack, requested);

        if (requested.hasLoop && ! loop.hasLoop)
            return;   // invalid input

        if (! (loop == loopForState))
            applyLoopLocked (loop);
    }

    bool isLoaderIdle()
    {
        const std::lock_guard<std::mutex> lock (stateMutex);
        return ! pendingLoad.has_value() && ! pendingRegion.has_value() && ! loaderBusy;
    }

    //==============================================================================
    // Loader thread
    void loaderLoop()
    {
        while (! loaderThread.threadShouldExit())
        {
            std::optional<LoadJob> load;
            std::optional<RegionJob> region;

            {
                const std::lock_guard<std::mutex> lock (stateMutex);

                if (pendingLoad.has_value())
                    std::swap (load, pendingLoad);
                else if (pendingRegion.has_value())
                    std::swap (region, pendingRegion);

                loaderBusy = load.has_value() || region.has_value();
            }

            if (! load.has_value() && ! region.has_value())
            {
                loaderThread.wait (200);
                continue;
            }

            try
            {
                if (load.has_value())
                    runLoadJob (*load);
                else
                    runRegionJob (*region);
            }
            catch (const std::exception&)
            {
                // Out of memory while decoding / analysing: report it (unless a newer request took over), keep the
                // loader alive.
                const std::lock_guard<std::mutex> lock (stateMutex);

                if (load.has_value() && load->serial == loadSerial)
                {
                    if (info.state == ReferenceState::Loading)
                    {
                        info.state = ReferenceState::Error;
                        info.error = "Not enough memory to load this file: " + load->file.getFileName();
                    }

                    if (! pendingRegion.has_value())
                        info.analysing = false;

                    bumpInfo();
                }
                else if (region.has_value() && region->serial == regionSerial && region->trackGen == uiTrackGen)
                {
                    info.analysing = false;
                    bumpInfo();
                }
            }
        }

        const std::lock_guard<std::mutex> lock (stateMutex);
        loaderBusy = false;
    }

    void runLoadJob (const LoadJob& job)
    {
        const auto serial = job.serial;
        auto cancelled = [this, serial] { return loaderThread.threadShouldExit() || loadSerialAtomic.load() != serial; };

        juce::String error;
        const auto track = rm::loadReferenceTrack (job.file, job.sampleRate, error, cancelled);

        juce::uint32 gen = 0;
        juce::uint64 regionSerialAtInstall = 0;
        LoopRegion loop;

        {
            Graveyard graveyard;
            const std::lock_guard<std::mutex> lock (stateMutex);

            // Superseded (a newer request updates the info), or the host rate changed meanwhile.
            if (cancelled() || (track != nullptr && track->sampleRate != targetSampleRate()))
                return;

            if (track == nullptr)
            {
                uninstallLocked (graveyard);
                info.state = ReferenceState::Error;
                info.file = job.file;
                info.displayName = job.file.getFileNameWithoutExtension();
                info.error = (error.isNotEmpty() ? error : juce::String ("Could not load the file")) + ": "
                             + job.file.getFileName();
                info.lengthSeconds = info.fileSampleRate = 0.0;
                info.fileChannels = 0;
                info.hasLoop = false;
                info.loopStart = info.loopEnd = 0.0;
                info.analysing = false;
                bumpInfo();
                publishReferenceProfile ({});
                return;
            }

            gen = nextTrackGen++;
            loop = clampLoop (*track, job.loop);
            const auto [regionStart, regionEnd] = regionSamples (*track, loop);
            installTrack (track, gen, regionStart, regionEnd, graveyard);
            graveyard.add (std::move (uiTrack));
            uiTrack = track;
            uiTrackGen = gen;
            wholeFile = {};
            wholeFileGen = 0;
            loopForState = loop;
            regionSerialAtInstall = regionSerial;

            info.state = ReferenceState::Ready;
            info.file = job.file;
            info.displayName = track->displayName;
            info.error = {};
            info.lengthSeconds = track->lengthSeconds();
            info.fileSampleRate = track->fileSampleRate;
            info.fileChannels = track->fileChannels;
            info.hasLoop = loop.hasLoop;
            info.loopStart = loop.hasLoop ? loop.start : 0.0;
            info.loopEnd = loop.hasLoop ? loop.end : track->lengthSeconds();
            info.analysing = true;
            bumpInfo();
            publishReferenceProfile ({});   // the previous reference's profile no longer applies
        }

        // A newer load or region request makes this analysis obsolete: stop early, that job publishes.
        rm::RegionAnalysis result;
        const bool done = rm::analyseReferenceRegion (*track, loop.hasLoop ? loop.start : 0.0, loop.hasLoop ? loop.end : 0.0,
                                                      result, [&] { return cancelled() || regionSerialAtomic.load() != regionSerialAtInstall; });

        const std::lock_guard<std::mutex> lock (stateMutex);

        if (! done || uiTrackGen != gen || regionSerial != regionSerialAtInstall)
            return;

        if (! loop.hasLoop)
        {
            wholeFile = result;
            wholeFileGen = gen;
        }

        publishReferenceProfile (result);
        info.analysing = false;
        bumpInfo();
    }

    void runRegionJob (const RegionJob& job)
    {
        TrackPtr track;

        {
            const std::lock_guard<std::mutex> lock (stateMutex);

            if (uiTrackGen != job.trackGen || uiTrack == nullptr || regionSerial != job.serial)
                return;

            if (! job.loop.hasLoop && wholeFileGen == job.trackGen)
            {
                publishReferenceProfile (wholeFile);
                info.analysing = false;
                bumpInfo();
                return;
            }

            track = uiTrack;
        }

        const auto serial = job.serial;
        rm::RegionAnalysis result;
        const bool done = rm::analyseReferenceRegion (*track, job.loop.hasLoop ? job.loop.start : 0.0,
                                                      job.loop.hasLoop ? job.loop.end : 0.0, result, [this, serial]
                                                      {
                                                          return loaderThread.threadShouldExit()
                                                                 || regionSerialAtomic.load() != serial;
                                                      });

        const std::lock_guard<std::mutex> lock (stateMutex);

        if (! done || uiTrackGen != job.trackGen)
            return;

        if (! job.loop.hasLoop)
        {
            wholeFile = result;
            wholeFileGen = job.trackGen;
        }

        if (regionSerial != job.serial)
            return;   // a newer region request is pending

        publishReferenceProfile (result);
        info.analysing = false;
        bumpInfo();
    }

    void publishReferenceProfile (const rm::RegionAnalysis& result)
    {
        {
            const std::lock_guard<std::mutex> lock (resultsMutex);
            refProfile = result.profile;
            comparison = rm::compareProfiles (mixProfile, refProfile);   // new reference / region: fresh severities
            holdBands = comparison.valid();
            analysisVersion.fetch_add (1);
        }

        refIntegratedLufs.store (result.profile.integratedLufs);
        refMonoIntegratedLufs.store (result.monoIntegratedLufs);
        refProfileVersion.fetch_add (1);
    }

    //==============================================================================
    // Analysis thread
    void analysisLoop()
    {
        while (! analysisThread.threadShouldExit())
        {
            try
            {
                analysisIteration (false);
            }
            catch (const std::exception&)
            {
                // Out of memory building a profile: skip this round.
            }

            analysisThread.wait (kAnalysisIntervalMs);
        }
    }

    // Returns true when audio was read. Caller holds analysisMutex.
    bool drainFifo (bool pushToAnalyser)
    {
        bool any = false;
        const bool mono = fifoChannels.load() == 1;

        for (int pass = 0; pass < 2; ++pass)
        {
            const int ready = fifo.getNumReady();
            if (ready <= 0)
                break;

            const auto scope = fifo.read (ready);

            if (pushToAnalyser)
            {
                auto push = [&] (int start, int size)
                {
                    if (size > 0)
                        mixAnalyser.push (fifoData[0] + start, mono ? nullptr : fifoData[1] + start, size);
                };

                push (scope.startIndex1, scope.blockSize1);
                push (scope.startIndex2, scope.blockSize2);
            }

            any = true;
        }

        return any;
    }

    // `fresh`: judge band severities from scratch (after a reset or an averaging switch) instead of holding them with
    // the hysteresis of rm::compareProfiles, so colours do not flicker while a difference sits near a threshold.
    void publishMix (const rm::TrackProfile& profile, bool fresh)
    {
        const std::lock_guard<std::mutex> lock (resultsMutex);
        mixProfile = profile;
        comparison = rm::compareProfiles (mixProfile, refProfile, holdBands && ! fresh ? &comparison : nullptr);
        holdBands = comparison.valid();
        analysisVersion.fetch_add (1);
    }

    void analysisIteration (bool forcePublish)
    {
        releaseRetiredTracks (false);

        {
            const std::lock_guard<std::mutex> lock (analysisMutex);

            float mixIntegrated = rm::kNoValue, mixShortTerm = rm::kNoValue;
            bool mixFull = false;

            if (analyserPrepared)
            {
                const auto averaging = averagingFromParam();

                if (averaging != mixAnalyser.getAveraging())
                {
                    mixAnalyser.setAveraging (averaging);
                    analysisDirty = true;
                    freshComparison = true;
                }

                // Read before draining: every sample the audio thread counted is in the FIFO by then (release /
                // acquire), so after the drain the meter's window really holds that much continuous audio. Read
                // again after it: a gap or a silent step the audio thread found meanwhile (in audio this drain may
                // already contain) rejects the window too.
                const bool fullBefore = mixWindowFull.load (std::memory_order_acquire);

                if (drainFifo (true))
                    analysisDirty = true;

                mixFull = fullBefore && mixWindowFull.load (std::memory_order_acquire);
                mixIntegrated = mixAnalyser.loudnessMeter().integratedLufs();
                mixShortTerm = mixAnalyser.loudnessMeter().shortTermLufs();
            }

            updateGainTarget (mixIntegrated, mixShortTerm, mixFull);

            const int refVersion = refProfileVersion.load();
            if (refVersion != seenRefProfileVersion)
            {
                seenRefProfileVersion = refVersion;
                analysisDirty = true;
            }

            const double now = juce::Time::getMillisecondCounterHiRes();

            if (forcePublish || (analysisDirty && now - lastPublishMs >= kPublishIntervalMs))
            {
                publishMix (analyserPrepared ? mixAnalyser.profile() : rm::TrackProfile(), freshComparison);
                analysisDirty = false;
                freshComparison = false;
                lastPublishMs = now;
            }
        }
    }

    // analysisMutex held. `mixWindowFullNow`: the mix meter's 3 s window holds continuous played audio.
    void updateGainTarget (float mixIntegrated, float mixShortTerm, bool mixWindowFullNow)
    {
        const auto mode = (rm::MatchMode) paramIndex (matchParam, 2);

        // On a mono bus the mix is measured as one channel and the reference plays folded to (L+R)/2: compare with
        // the fold-down's loudness.
        const bool monoBus = busChannels.load() == 1;
        const float refIntegrated = monoBus ? refMonoIntegratedLufs.load() : refIntegratedLufs.load();

        // A new mode, track or loop region invalidates the held short-term target.
        const auto regionGen = audioRegionGen.load();

        if (mode != gainState.mode || regionGen != gainState.regionGen)
        {
            gainState.mode = mode;
            gainState.regionGen = regionGen;
            gainState.haveShortTerm = false;
        }

        float target = 0.0f;

        if (mode == rm::MatchMode::Integrated)
        {
            target = rm::referenceMatchGainDb (mixIntegrated, refIntegrated);
        }
        else if (mode == rm::MatchMode::ShortTerm)
        {
            // Both 3 s windows describe what was actually heard (the reference is measured as it plays, so loop
            // wraps, host loops, seeks and the mono fold-down are included). They are compared only while both are
            // full of continuous, non-silent audio; otherwise (host stopped, the first 3 s after play / load / a
            // pause, silence or a noise floor) the last short-term target is held, so stop / start never pumps the
            // reference. Before the first valid pair the integrated match is used.
            const float refShortTerm = refMatchShortTermLufs.load();
            const bool mixValid = mixWindowFullNow && rm::hasValue (mixShortTerm)
                                  && mixShortTerm > rm::LoudnessMeter::kAbsoluteGateLufs;

            if (mixValid && rm::hasValue (refShortTerm))
            {
                gainState.shortTermDb = rm::referenceMatchGainDb (mixShortTerm, refShortTerm);
                gainState.haveShortTerm = std::isfinite (gainState.shortTermDb);
            }

            target = gainState.haveShortTerm ? gainState.shortTermDb
                                             : rm::referenceMatchGainDb (mixIntegrated, refIntegrated);
        }

        targetGainDb.store (std::isfinite (target) ? target : 0.0f);
    }

    void resetMix()
    {
        const std::lock_guard<std::mutex> lock (analysisMutex);

        if (analyserPrepared)
        {
            drainFifo (false);   // audio from before the reset
            mixAnalyser.reset();
        }

        publishMix (analyserPrepared ? mixAnalyser.profile() : rm::TrackProfile(), true);
        analysisDirty = false;
        freshComparison = false;
        lastPublishMs = juce::Time::getMillisecondCounterHiRes();
    }

    //==============================================================================
    // Host lifecycle (message thread, audio stopped)
    void prepare (double newSampleRate, int maximumBlockSize)
    {
        analysisThread.stopThread (kThreadStopTimeoutMs);
        audioPrepared.store (false);

        const double rate = newSampleRate > 0.0 ? newSampleRate : kFallbackSampleRate;
        const int maxBlock = juce::jmax (kMinChunk, maximumBlockSize);
        sampleRate = rate;
        numChannels = juce::jlimit (1, 2, owner.getTotalNumOutputChannels());
        busChannels.store (numChannels);

        refScratch.setSize (2, maxBlock);
        monoScratch.assign ((size_t) maxBlock, 0.0f);
        listenLength = samplesForMs (kListenFadeMs, rate);
        jumpLength = samplesForMs (kJumpFadeMs, rate);
        monoLength = samplesForMs (kMonoFadeMs, rate);
        syncTolerance = samplesForMs (kHostSyncToleranceMs, rate);
        shortTermValidSamples = (long long) std::ceil (kShortTermValidSeconds * rate);
        silenceStepLength = samplesForMs (100.0, rate);
        listenTable = makeQuarterSine (listenLength);
        jumpTable = makeQuarterSine (jumpLength);

        refMeter.prepare (rate);   // allocates (gating histograms): here, never on the audio thread
        refShortTermLufs.store (rm::kNoValue);
        refMatchShortTermLufs.store (rm::kNoValue);
        mixWindowFull.store (false);

        {
            const std::lock_guard<std::mutex> lock (analysisMutex);
            const int fifoSize = juce::jmax ((int) std::ceil (kFifoSeconds * rate), 4 * maxBlock);
            fifoBuffer.setSize (2, fifoSize);
            fifoBuffer.clear();
            fifoData[0] = fifoBuffer.getWritePointer (0);
            fifoData[1] = fifoBuffer.getWritePointer (1);
            fifo.setTotalSize (fifoSize);
            fifo.reset();
            fifoChannels.store (juce::jlimit (1, 2, owner.getTotalNumInputChannels()));

            if (! analyserPrepared || analyserRate != rate || analyserChannels != fifoChannels.load())
            {
                mixAnalyser.prepare (rate, averagingFromParam());
                analyserPrepared = true;
                analyserRate = rate;
                analyserChannels = fifoChannels.load();
                analysisDirty = true;
            }

            gainState = GainState();   // no held short-term target across a re-prepare
        }

        const float targetDb = targetGainDb.load();
        gain.reset (rate, kGainSmoothMs / 1000.0);
        gain.setCurrentAndTargetValue ((float) rm::dbToGain (targetDb));

        resetAudioState();
        audio.lastTargetDb = targetDb;
        audio.seenSeekCount = seekCount.load();
        audio.monoStep = paramOn (monoParam) ? monoLength : 0;
        matchGainDb.store (targetDb);

        // The reference buffer is at the host rate: re-load the file when the rate changed (the mix is heard until
        // then, see haveTrack).
        {
            Graveyard graveyard;
            const std::lock_guard<std::mutex> lock (stateMutex);
            hostSampleRate.store (rate);   // under stateMutex: every request from now on loads at this rate

            const bool wrongRateTrack = uiTrack != nullptr && uiTrack->sampleRate != rate;
            const bool wantsTrack = (info.state == ReferenceState::Ready || info.state == ReferenceState::Loading)
                                    && info.file != juce::File();

            if (wrongRateTrack || (wantsTrack && loadSampleRate != rate))
            {
                uninstallLocked (graveyard);

                if (wantsTrack)
                    requestLoadLocked (info.file, loopForState);
            }
        }

        // Start on the reference only if there is one to play (no one-block A/B ramp on the first block).
        {
            const juce::SpinLock::ScopedLockType lock (slotLock);
            audio.haveTrack = slot.current != nullptr && slot.regionEnd > slot.regionStart;
        }

        audio.listenStep = paramOn (listenParam) && audio.haveTrack ? listenLength : 0;
        listenPosition.store ((float) audio.listenStep / (float) listenLength);

        releaseRetiredTracks (true);
        audioPrepared.store (true);
        analysisThread.startThread (juce::Thread::Priority::low);
    }

    void release()
    {
        analysisThread.stopThread (kThreadStopTimeoutMs);
        audioPrepared.store (false);
        releaseRetiredTracks (true);
    }

    //==============================================================================
    // State
    void writeState (juce::MemoryBlock& dest)
    {
        juce::XmlElement root (kStateTag);
        root.setAttribute ("version", kStateVersion);
        root.setAttribute ("editorW", editorWidth.load());
        root.setAttribute ("editorH", editorHeight.load());

        if (auto params = apvts.copyState().createXml())
            root.addChildElement (params.release());

        {
            const std::lock_guard<std::mutex> lock (stateMutex);

            if (info.state != ReferenceState::Empty && info.file != juce::File())
            {
                auto* ref = root.createNewChildElement (kReferenceTag);
                ref->setAttribute ("path", info.file.getFullPathName());
                ref->setAttribute ("hasLoop", loopForState.hasLoop ? 1 : 0);
                ref->setAttribute ("loopStart", loopForState.hasLoop ? loopForState.start : 0.0);
                ref->setAttribute ("loopEnd", loopForState.hasLoop ? loopForState.end : 0.0);
            }
        }

        juce::AudioProcessor::copyXmlToBinary (root, dest);
    }

    // Sanitises every parameter value (missing / non-finite -> default, clamped) and forces Listen to Mix.
    void applyParams (const juce::XmlElement* xml)
    {
        auto tree = xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree();

        if (! tree.isValid() || tree.getType() != apvts.state.getType())
            tree = apvts.copyState();

        for (auto* p : owner.getParameters())
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p);
            if (rp == nullptr)
                continue;

            const auto& range = rp->getNormalisableRange();
            const double def = (double) rp->convertFrom0to1 (rp->getDefaultValue());
            auto child = tree.getChildWithProperty ("id", rp->paramID);

            if (! child.isValid())
            {
                child = juce::ValueTree ("PARAM");
                child.setProperty ("id", rp->paramID, nullptr);
                tree.appendChild (child, nullptr);
            }

            const auto v = child.getProperty ("value");
            double value = (v.isVoid() || v.isUndefined()) ? def : (double) v;

            if (! std::isfinite (value))
                value = def;

            value = juce::jlimit ((double) range.start, (double) range.end, value);

            if (rp->paramID == kParamListen)
                value = 0.0;   // never start on the reference

            child.setProperty ("value", value, nullptr);
        }

        apvts.replaceState (tree);
    }

    void readState (const void* data, int sizeInBytes)
    {
        if (data == nullptr || sizeInBytes <= 0)
            return;

        const auto xml = juce::AudioProcessor::getXmlFromBinary (data, sizeInBytes);
        if (xml == nullptr)
            return;

        const juce::XmlElement* params = nullptr;
        const juce::XmlElement* reference = nullptr;
        bool fullState = false;

        if (xml->hasTagName (kStateTag))
        {
            fullState = true;
            params = xml->getChildByName (kParamsTag);
            reference = xml->getChildByName (kReferenceTag);

            const int w = xml->getIntAttribute ("editorW", 0), h = xml->getIntAttribute ("editorH", 0);
            if (w > 0 && h > 0)
            {
                editorWidth.store (juce::jlimit (RefMatchEditor::kMinWidth, RefMatchEditor::kMaxWidth, w));
                editorHeight.store (juce::jlimit (RefMatchEditor::kMinHeight, RefMatchEditor::kMaxHeight, h));
            }
        }
        else if (xml->hasTagName (kParamsTag))
        {
            params = xml.get();   // bare APVTS state
        }
        else
        {
            return;
        }

        applyParams (params);

        if (! fullState)
            return;

        if (reference != nullptr)
        {
            const auto path = reference->getStringAttribute ("path").trim();

            if (path.isNotEmpty())
            {
                LoopRegion loop;
                loop.hasLoop = reference->getIntAttribute ("hasLoop", 0) != 0;
                loop.start = reference->getDoubleAttribute ("loopStart", 0.0);
                loop.end = reference->getDoubleAttribute ("loopEnd", 0.0);

                if (! std::isfinite (loop.start) || ! std::isfinite (loop.end))
                    loop = {};

                const auto file = juce::File::isAbsolutePath (path)
                                      ? juce::File (path)
                                      : juce::File::getCurrentWorkingDirectory().getChildFile (path);
                restoreReference (file, loop);
                return;
            }
        }

        clear();
    }

    //==============================================================================
    RefMatchProcessor& owner;
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* listenParam = nullptr;
    std::atomic<float>* matchParam = nullptr;
    std::atomic<float>* monoParam = nullptr;
    std::atomic<float>* freeRunParam = nullptr;
    std::atomic<float>* averagingParam = nullptr;

    std::atomic<int> editorWidth { RefMatchEditor::kDefaultWidth }, editorHeight { RefMatchEditor::kDefaultHeight };

    // ---- reference state (stateMutex) -------------------------------------------------------------------------
    std::mutex stateMutex;
    ReferenceInfo info;
    int infoVersion = 0;
    TrackPtr uiTrack;                    // current track for drawing / region analysis
    juce::uint32 uiTrackGen = 0;
    juce::uint32 nextTrackGen = 1;
    LoopRegion loopForState;             // loop to save and to apply when (re)loading
    double loadSampleRate = 0.0;         // rate of the current track / pending load
    rm::RegionAnalysis wholeFile;        // cached whole-file analysis for clearLoopRegion
    juce::uint32 wholeFileGen = 0;
    juce::uint64 loadSerial = 0, regionSerial = 0;
    std::atomic<juce::uint64> loadSerialAtomic { 0 }, regionSerialAtomic { 0 };   // for cancellation checks
    std::optional<LoadJob> pendingLoad;
    std::optional<RegionJob> pendingRegion;
    bool loaderBusy = false;
    std::atomic<double> hostSampleRate { 0.0 };   // written under stateMutex

    juce::SpinLock slotLock;
    Slot slot;
    std::atomic<bool> retiredPending { false };   // some slot.retired entry holds a track; written under slotLock
    std::atomic<bool> audioPrepared { false };

    std::atomic<double> seekSeconds { 0.0 };
    std::atomic<int> seekCount { 0 };

    // ---- audio thread -----------------------------------------------------------------------------------------
    AudioState audio;
    juce::AudioBuffer<float> refScratch;
    std::vector<float> monoScratch;   // mono-bus fold-down of refScratch for refMeter
    rm::LoudnessMeter refMeter;       // the reference as heard (unity gain); realtime-safe after prepare
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gain { 1.0f };
    std::vector<float> listenTable, jumpTable;
    int listenLength = 1, jumpLength = 1, monoLength = 1;
    long long syncTolerance = 1;           // samples (kHostSyncToleranceMs)
    long long shortTermValidSamples = 1;   // samples (kShortTermValidSeconds)
    int silenceStepLength = 1;             // 100 ms (RunCounter)
    double sampleRate = kFallbackSampleRate;
    int numChannels = 2;

    // ---- live values for the UI -------------------------------------------------------------------------------
    std::atomic<double> playheadSeconds { 0.0 };
    std::atomic<float> matchGainDb { 0.0f }, listenPosition { 0.0f };
    std::atomic<bool> hostPlaying { false }, referenceSounding { false };
    std::atomic<int> busChannels { 2 };
    std::atomic<float> refShortTermLufs { rm::kNoValue };   // reference as heard, for the UI
    std::atomic<int> referenceDropouts { 0 };

    // ---- audio thread -> analysis thread (short-term matching) ------------------------------------------------
    std::atomic<bool> mixWindowFull { false };                   // the mix FIFO got kShortTermValidSeconds in a row
    std::atomic<float> refMatchShortTermLufs { rm::kNoValue };   // valid reference window, else kNoValue
    std::atomic<juce::uint32> audioRegionGen { 0 };              // slot.regionGen the values above belong to

    // ---- analysis (analysisMutex) -----------------------------------------------------------------------------
    std::mutex analysisMutex;
    rm::TrackAnalyser mixAnalyser;
    bool analyserPrepared = false;
    double analyserRate = 0.0;
    int analyserChannels = 0;
    juce::AbstractFifo fifo { 1 };
    juce::AudioBuffer<float> fifoBuffer;
    float* fifoData[2] = { nullptr, nullptr };
    std::atomic<int> fifoChannels { 2 };
    std::atomic<int> droppedBlocks { 0 };
    bool analysisDirty = true;
    bool freshComparison = false;   // next publish judges band severities from scratch (averaging switched)
    int seenRefProfileVersion = -1;
    double lastPublishMs = 0.0;
    std::atomic<float> targetGainDb { 0.0f };
    std::atomic<float> refIntegratedLufs { rm::kNoValue }, refMonoIntegratedLufs { rm::kNoValue };
    std::atomic<int> refProfileVersion { 0 };

    struct GainState
    {
        rm::MatchMode mode = rm::MatchMode::Integrated;
        juce::uint32 regionGen = 0;
        bool haveShortTerm = false;   // a short-term target was measured for this mode / track / region
        float shortTermDb = 0.0f;     // the last one (held while the windows are not valid)
    };
    GainState gainState;

    // ---- published results (resultsMutex) ---------------------------------------------------------------------
    std::mutex resultsMutex;
    rm::TrackProfile mixProfile, refProfile;
    rm::Comparison comparison;
    bool holdBands = false;   // `comparison` is a valid anchor for band-severity hysteresis
    std::atomic<int> analysisVersion { 0 };

    // Last: started after (and stopped before) everything they use.
    WorkerThread loaderThread { "RefMatch loader", [this] { loaderLoop(); } };
    WorkerThread analysisThread { "RefMatch analysis", [this] { analysisLoop(); } };
};

//==============================================================================
RefMatchProcessor::RefMatchProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    impl = std::make_unique<Impl> (*this);
}

RefMatchProcessor::~RefMatchProcessor() = default;

juce::AudioProcessorValueTreeState& RefMatchProcessor::getAPVTS() noexcept { return impl->apvts; }

RefMatchProcessor::ReferenceInfo RefMatchProcessor::getReferenceInfo() const
{
    const std::lock_guard<std::mutex> lock (impl->stateMutex);
    return impl->info;
}

void RefMatchProcessor::loadReference (const juce::File& file) { impl->requestUserLoad (file); }
void RefMatchProcessor::clearReference() { impl->clear(); }

void RefMatchProcessor::setLoopRegion (double startSeconds, double endSeconds)
{
    if (std::isfinite (startSeconds) && std::isfinite (endSeconds))
        impl->setLoop ({ true, startSeconds, endSeconds });
}

void RefMatchProcessor::clearLoopRegion() { impl->setLoop ({}); }

bool RefMatchProcessor::isSupportedAudioFile (const juce::File& f)
{
    return f.hasFileExtension ("wav;aif;aiff;flac;mp3;ogg");
}

juce::String RefMatchProcessor::getSupportedWildcard() { return "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg"; }

std::shared_ptr<const rm::ReferenceTrack> RefMatchProcessor::getReferenceTrack() const
{
    const std::lock_guard<std::mutex> lock (impl->stateMutex);
    return impl->uiTrack;
}

double RefMatchProcessor::getReferencePlayheadSeconds() const noexcept { return impl->playheadSeconds.load(); }

void RefMatchProcessor::seekReference (double seconds)
{
    if (! std::isfinite (seconds))
        return;

    impl->seekSeconds.store (juce::jmax (0.0, seconds));
    impl->seekCount.fetch_add (1, std::memory_order_release);
}

rm::TrackProfile RefMatchProcessor::getMixProfile() const
{
    const std::lock_guard<std::mutex> lock (impl->resultsMutex);
    return impl->mixProfile;
}

rm::TrackProfile RefMatchProcessor::getReferenceProfile() const
{
    const std::lock_guard<std::mutex> lock (impl->resultsMutex);
    return impl->refProfile;
}

rm::Comparison RefMatchProcessor::getComparison() const
{
    const std::lock_guard<std::mutex> lock (impl->resultsMutex);
    return impl->comparison;
}

int RefMatchProcessor::getAnalysisVersion() const noexcept { return impl->analysisVersion.load(); }
void RefMatchProcessor::resetMixAnalysis() { impl->resetMix(); }

float RefMatchProcessor::getMatchGainDb() const noexcept { return impl->matchGainDb.load(); }
float RefMatchProcessor::getListenPosition() const noexcept { return impl->listenPosition.load(); }
bool RefMatchProcessor::isHostPlaying() const noexcept { return impl->hostPlaying.load(); }
bool RefMatchProcessor::isReferenceSounding() const noexcept { return impl->referenceSounding.load(); }
float RefMatchProcessor::getReferenceShortTermLufs() const noexcept { return impl->refShortTermLufs.load(); }

bool RefMatchProcessor::waitUntilReferenceIdle (int timeoutMs) const
{
    const double end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;

    for (;;)
    {
        if (impl->isLoaderIdle())
            return true;

        if (juce::Time::getMillisecondCounterHiRes() >= end)
            return false;

        juce::Thread::sleep (2);
    }
}

void RefMatchProcessor::runAnalysisNow() { impl->analysisIteration (true); }
int RefMatchProcessor::getReferenceDropoutCount() const noexcept { return impl->referenceDropouts.load(); }

void RefMatchProcessor::prepareToPlay (double sampleRate, int samplesPerBlock) { impl->prepare (sampleRate, samplesPerBlock); }
void RefMatchProcessor::releaseResources() { impl->release(); }

bool RefMatchProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet(), out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

void RefMatchProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    const juce::ScopedNoDenormals noDenormals;
    impl->process (buffer);
}

juce::AudioProcessorEditor* RefMatchProcessor::createEditor() { return new RefMatchEditor (*this); }

void RefMatchProcessor::getStateInformation (juce::MemoryBlock& dest) { impl->writeState (dest); }
void RefMatchProcessor::setStateInformation (const void* data, int size) { impl->readState (data, size); }

juce::Point<int> RefMatchProcessor::getEditorSize() const noexcept
{
    return { impl->editorWidth.load(), impl->editorHeight.load() };
}

void RefMatchProcessor::setEditorSize (juce::Point<int> s) noexcept
{
    impl->editorWidth.store (s.x);
    impl->editorHeight.store (s.y);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new RefMatchProcessor(); }
