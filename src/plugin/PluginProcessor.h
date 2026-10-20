#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "core/Compare.h"
#include "plugin/ReferenceTrack.h"

#include <memory>

// RefMatch processor. Public API = contract for the UI and the dev tool; everything private lives in Impl.
//
// Signal flow (see ARCHITECTURE.md): plugin input = the user's mix (plugin on the master). The input is copied into a
// lock-free FIFO for the analysis thread (except while a host transport that has played is stopped: that silence is
// not part of the mix). Output = equal-power crossfade between the input ("Mix", A) and the reference player
// ("Reference", B, loudness-matched), then the optional mono fold-down. Without an installed reference track the A/B
// target is Mix whatever the Listen parameter says, so an empty reference never silences the mix. With Listen = Mix,
// mono off and no fade in progress the output is bit-identical to the input. During offline rendering
// (isNonRealtime()) the output is ALWAYS the untouched input, so a forgotten "Reference" setting can never end up in an
// export.
class RefMatchProcessor final : public juce::AudioProcessor
{
public:
    //==============================================================================
    // Parameter IDs (APVTS, all automatable)
    static constexpr const char* kParamListen = "listen";        // bool: false = Mix (A), true = Reference (B)
    static constexpr const char* kParamMatch = "matchMode";      // choice: "Off", "Integrated", "Short-term" (rm::MatchMode), default Integrated
    static constexpr const char* kParamMono = "monoCheck";       // bool, default off: fold the output to mono
    static constexpr const char* kParamFreeRun = "freeRun";      // bool, default on: reference keeps playing while the host is stopped
    static constexpr const char* kParamAveraging = "averaging";  // choice: "Recent (30 s)", "Since reset" (rm::Averaging), default Recent

    static constexpr double kListenFadeMs = 30.0;   // A/B equal-power crossfade
    static constexpr double kJumpFadeMs = 8.0;      // crossfade when the reference playhead jumps or loops
    static constexpr double kMonoFadeMs = 20.0;     // mono fold-down ramp
    static constexpr double kGainSmoothMs = 250.0;  // matching-gain ramp
    static constexpr double kMinLoopSeconds = 0.5;

    RefMatchProcessor();
    ~RefMatchProcessor() override;

    juce::AudioProcessorValueTreeState& getAPVTS() noexcept;

    //==============================================================================
    // Reference track (message thread). Loading / resampling / analysis run on a background loader thread; the old
    // reference keeps playing until the new one is ready.
    enum class ReferenceState
    {
        Empty,     // nothing loaded
        Loading,   // decoding / resampling / analysing
        Ready,
        Error      // `error` says why; `file` keeps the requested path (it is saved in the state anyway)
    };

    struct ReferenceInfo
    {
        ReferenceState state = ReferenceState::Empty;
        juce::File file;
        juce::String displayName;
        juce::String error;
        double lengthSeconds = 0.0;
        double fileSampleRate = 0.0;
        int fileChannels = 0;
        bool hasLoop = false;          // false = the whole file is the region
        double loopStart = 0.0;        // seconds (whole file: 0)
        double loopEnd = 0.0;          // seconds (whole file: lengthSeconds)
        bool analysing = false;        // region profile being (re)computed
        int version = 0;               // increments on every change of anything above
    };

    ReferenceInfo getReferenceInfo() const;

    // Loads a new reference without a loop region. Exception (relink): while the reference is in the Error state
    // (e.g. "File not found" after a project moved), a file with the same name (case-insensitive) keeps the saved
    // loop region, clamped to the new file.
    void loadReference (const juce::File&);
    void clearReference();

    // Loop region in seconds, clamped to the file and to at least kMinLoopSeconds; re-analyses the region in the
    // background (the reference profile keeps its previous value until then). Ignored when no reference is ready.
    void setLoopRegion (double startSeconds, double endSeconds);
    void clearLoopRegion();

    static bool isSupportedAudioFile (const juce::File&);
    static juce::String getSupportedWildcard();   // "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg"

    // The current reference (nullptr when none) for drawing its overview / short-term curve. Message thread.
    std::shared_ptr<const rm::ReferenceTrack> getReferenceTrack() const;

    // Reference playhead in seconds within the file (updated by the audio thread every block).
    double getReferencePlayheadSeconds() const noexcept;

    // Moves the reference playhead (click in the waveform). Takes effect on the next block, with a jump crossfade.
    // While the host is playing the host position wins again on the next block, offset so that the clicked point
    // plays now: the reference keeps following the host from there (offset is reset by clearReference / a new file).
    void seekReference (double seconds);

    //==============================================================================
    // Analysis results (message thread). Published by the analysis thread ~30 times per second.
    rm::TrackProfile getMixProfile() const;
    rm::TrackProfile getReferenceProfile() const;   // of the loop region; valid == false when none
    rm::Comparison getComparison() const;
    int getAnalysisVersion() const noexcept;        // increments whenever any of the three above changes

    // Clears the mix measurements (integrated loudness, LRA, peaks, spectrum, stereo).
    void resetMixAnalysis();

    //==============================================================================
    // Live state for the UI
    float getMatchGainDb() const noexcept;    // gain currently applied to the reference (after smoothing)
    float getListenPosition() const noexcept; // 0 = mix, 1 = reference (crossfade position)
    bool isHostPlaying() const noexcept;
    bool isReferenceSounding() const noexcept;   // reference is being played (B audible and not silent-stopped)

    // Short-term loudness (last 3 s, LUFS) of the reference exactly as it plays, measured on the audio thread before
    // the matching gain (on a mono bus: of the (L+R)/2 fold-down that is heard). Loop wraps, host loops, seeks and
    // jumps are therefore included as heard. Keeps its value while the reference is paused; kNoValue until the
    // current track has played for about 3 s.
    float getReferenceShortTermLufs() const noexcept;

    //==============================================================================
    // For the dev tool / tests
    // Waits (sleeping, no message pumping needed) until no load / region analysis is pending. True if idle in time.
    bool waitUntilReferenceIdle (int timeoutMs) const;
    // Drains the analysis FIFO and publishes results synchronously on the calling thread (deterministic tests).
    void runAnalysisNow();
    // Number of audio blocks whose reference part was silenced because another thread held the track hand-over lock
    // (since construction). Diagnostics / tests; should stay 0.
    int getReferenceDropoutCount() const noexcept;

    //==============================================================================
    // juce::AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;   // mono->mono, stereo->stereo
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "RefMatch"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // State = parameters + reference path + loop region (+ editor size). The reference audio is NOT stored; it is
    // re-loaded from the path. Restoring a state always sets Listen back to Mix (safety: never start on the reference).
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Editor size persisted in the state (default 1100 x 690).
    juce::Point<int> getEditorSize() const noexcept;
    void setEditorSize (juce::Point<int>) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RefMatchProcessor)
};
