#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include "core/Analysis.h"

#include <functional>
#include <memory>
#include <vector>

// Decoding, resampling and offline analysis of a reference file. Everything here runs on the processor's loader
// thread (never the audio thread, never blocking the message thread).
namespace rm
{
// An immutable, fully decoded reference, already resampled to the host sample rate. Shared between the loader thread
// (creates it), the audio thread (reads samples) and the message thread (overview for drawing).
struct ReferenceTrack
{
    juce::File file;
    juce::String displayName;          // file name without extension
    double fileSampleRate = 0.0;       // sample rate of the file on disk
    int fileChannels = 0;
    double sampleRate = 0.0;           // sample rate of `audio` (= host rate at load time)
    juce::AudioBuffer<float> audio;    // always 2 channels (mono files duplicated), at `sampleRate`

    std::vector<float> shortTermCurve; // rm::shortTermLoudnessCurve of the whole track (100 ms steps)
    std::vector<float> monoShortTermCurve; // the same for the mono fold-down (L+R)/2 measured as one channel
                                           // (what plays on a mono bus)
    WaveformOverview overview;         // kOverviewColumns columns of the whole track

    static constexpr int kOverviewColumns = 2048;

    long long numSamples() const noexcept { return audio.getNumSamples(); }
    double lengthSeconds() const noexcept { return sampleRate > 0.0 ? (double) numSamples() / sampleRate : 0.0; }
};

// Formats RefMatch can open (WAV, AIFF, FLAC, MP3 via JUCE's built-in readers; Ogg if available).
void registerReferenceFormats (juce::AudioFormatManager&);

// Maximum accepted reference length; longer files are rejected with an error (memory: ~11 MB per stereo minute at
// 48 kHz, so 20 minutes ~ 220 MB).
inline constexpr double kMaxReferenceSeconds = 20.0 * 60.0;

// Decodes `file`, resamples it to `targetSampleRate` (high-quality windowed-sinc when the rates differ), builds the
// short-term curve and the overview. Returns nullptr and sets `error` to a short user-facing message on failure
// ("File not found", "Unsupported or damaged audio file", "File is longer than 20 minutes", ...).
// `shouldExit` is polled regularly so a load can be abandoned (returns nullptr with an empty error).
std::shared_ptr<const ReferenceTrack> loadReferenceTrack (const juce::File& file, double targetSampleRate,
                                                          juce::String& error,
                                                          const std::function<bool()>& shouldExit = {});

// Analyses [startSeconds, endSeconds) of the track (whole track when end <= start) with rm::analyseRegion.
TrackProfile analyseReferenceRegion (const ReferenceTrack&, double startSeconds, double endSeconds);

// The same analysis with cancellation (`shouldExit` is polled between chunks; returns false when cancelled). Also
// measures the integrated loudness of the region folded to mono ((L+R)/2 as one channel), for a mono bus.
struct RegionAnalysis
{
    TrackProfile profile;
    float monoIntegratedLufs = kNoValue;
};

bool analyseReferenceRegion (const ReferenceTrack&, double startSeconds, double endSeconds, RegionAnalysis& result,
                             const std::function<bool()>& shouldExit);

} // namespace rm
