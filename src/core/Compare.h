#pragma once

#include "core/Analysis.h"

#include <string>
#include <vector>

// Mix-vs-reference comparison and the plain-language advice shown to the producer. Pure C++17, deterministic.
namespace rm
{
//==============================================================================
// Loudness matching for the A/B comparison. RefMatch never changes the level of the user's mix: it turns the
// reference up or down so both play at the same loudness.
enum class MatchMode
{
    Off = 0,          // reference plays at its own level
    Integrated = 1,   // mix integrated loudness (since reset) vs reference integrated loudness (loop region)
    ShortTerm = 2     // mix short-term loudness (last 3 s) vs reference short-term loudness at its playhead
};

inline constexpr float kMinMatchGainDb = -40.0f;
inline constexpr float kMaxMatchGainDb = 12.0f;

// Until the mix has been measured, the reference is matched to this typical level instead.
inline constexpr float kFallbackMixLufs = -14.0f;

// Gain in dB to apply to the reference so it plays as loud as the mix: clamp (mixLufs - referenceLufs) to
// [kMinMatchGainDb, kMaxMatchGainDb]. When referenceLufs is unknown returns 0; when only mixLufs is unknown uses
// kFallbackMixLufs.
float referenceMatchGainDb (float mixLufs, float referenceLufs) noexcept;

//==============================================================================
enum class Severity
{
    Good = 0,    // close to the reference / nothing to do
    Info = 1,    // explanation or instruction (e.g. "load a reference"), no problem implied
    Minor = 2,   // noticeable difference, worth a look
    Major = 3    // big difference / technical problem
};

enum class Topic
{
    Setup = 0,   // missing reference / not enough audio yet
    Loudness,
    TruePeak,
    Dynamics,
    Tone,
    Stereo,
    LowEnd
};

// Tonal-balance thresholds on |band level difference| (dB, loudness-normalised).
inline constexpr float kBandMinorDb = 1.5f;
inline constexpr float kBandMajorDb = 3.0f;

Severity bandSeverity (float levelDiffDb) noexcept;

// Hysteresis for live band severities, so bars and tone hints do not flicker while a difference hovers around
// kBandMinorDb / kBandMajorDb: starting from the severity shown before, step up only when |diff| reaches the higher
// threshold + kBandHysteresisDb, step down only when |diff| falls below the current threshold - kBandHysteresisDb.
inline constexpr float kBandHysteresisDb = 0.4f;

// bandSeverity with hold. `previous` Info is treated as Good. NaN gives Good.
Severity bandSeverity (float levelDiffDb, Severity previous) noexcept;

// Other hint thresholds (public so the UI can colour readouts the same way).
// True peak above kTruePeakCeilingDbtp: Major ("may distort when streamed"), Minor when the reference peaks at least
// as high (loud club masters often do; -1 dBTP mainly protects the streaming versions).
inline constexpr float kTruePeakCeilingDbtp = -1.0f;
inline constexpr float kLoudnessDiffLu = 1.0f;         // |mix - ref integrated| above this gets a hint
inline constexpr float kLouderMajorLu = 3.0f;          // mix louder than the reference by more than this: Major
inline constexpr float kPlrDiffDb = 2.0f;              // |mix - ref PLR| above this gets a hint
inline constexpr float kPlrMajorDb = 4.0f;             // mix PLR lower (more squashed) by more than this: Major
inline constexpr float kLowEndCorrMinor = 0.8f;        // low-end correlation below this: Minor
inline constexpr float kLowEndCorrMajor = 0.5f;        // ... below this: Major
inline constexpr float kWidthMinorPct = 20.0f;         // per-band |width difference| (percentage points): Minor
inline constexpr float kWidthMajorPct = 40.0f;         // ... Major

struct Hint
{
    Severity severity = Severity::Info;
    Topic topic = Topic::Setup;
    int band = -1;             // band index for Tone / Stereo hints, else -1. Also -1 for hints about the whole mix
                               // (the Good hint, broadband Stereo hints): always check band >= 0 before indexing.
    // The measured difference the hint is about (dB / LU / %), signed, for sorting & tests:
    //   Tone: band levelDiffDb; Loudness: loudnessDiffLu; TruePeak: mix true peak - kTruePeakCeilingDbtp;
    //   Dynamics: plrDiffDb; Stereo: widthDiffPct (or the broadband correlation); LowEnd: mix low-end correlation.
    float amount = 0.0f;
    std::string title;         // short, e.g. "Low mids +3.1 dB vs reference"
    std::string detail;        // one or two plain-language sentences: what it means and what to try
};

struct BandComparison
{
    bool valid = false;
    float levelDiffDb = 0.0f;  // (mix band - mix spectral loudness) - (ref band - ref spectral loudness); + = mix has more
    float mixWidthPct = 0.0f;
    float refWidthPct = 0.0f;
    float widthDiffPct = 0.0f; // mix - ref
    Severity severity = Severity::Good;
};

struct Comparison
{
    bool mixReady = false;         // mix profile valid
    bool referenceReady = false;   // reference profile valid
    bool valid() const noexcept { return mixReady && referenceReady; }

    float loudnessDiffLu = kNoValue;   // mix integrated - ref integrated (kNoValue when either is unknown)
    float truePeakDiffDb = kNoValue;   // mix - ref
    float plrDiffDb = kNoValue;        // mix - ref (TrackProfile::plrDb is sample-peak based: ignores the overshoot
                                       // between samples that clipping creates)
    float lraDiffLu = kNoValue;        // mix - ref

    // Add this to the mix's curve / band levels to overlay them on the reference at equal loudness:
    // ref.spectralLoudnessDb - mix.spectralLoudnessDb (0 when unknown).
    float normalisationOffsetDb = 0.0f;

    std::array<BandComparison, kNumBands> bands {};

    // (mix curve + normalisationOffsetDb) - ref curve, per curve point; kNoValue where either is unknown.
    Curve curveDiffDb {};

    // Most important first (severity, then size of the difference). At most kMaxHints. Never empty: when nothing
    // stands out there is one Good hint saying so; when data is missing there are Setup hints saying what to do.
    // Setup hints always come first (they are what to do next). The size of differences in different units is
    // compared relative to each topic's threshold, so e.g. a true peak over -1 dBTP is not outranked by a width in %.
    // Without a reference, the mix-only checks (true peak, low-end mono) still produce hints.
    std::vector<Hint> hints;

    Comparison() { curveDiffDb.fill (kNoValue); }
};

inline constexpr int kMaxHints = 8;

Comparison compareProfiles (const TrackProfile& mix, const TrackProfile& reference);

// The same, for live updates: when `previous` is not null and previous->valid(), every band that was valid there and
// is valid now gets bandSeverity (levelDiffDb, previous band severity) instead of bandSeverity (levelDiffDb), and the
// Tone hints follow those held severities. Pass the last Comparison published for the SAME reference / region and
// averaging mode; pass nullptr (start fresh) after resetting the mix analysis, changing the reference or its region,
// or switching the averaging mode. `previous` may be the object the result is assigned to (it is only read).
Comparison compareProfiles (const TrackProfile& mix, const TrackProfile& reference, const Comparison* previous);

//==============================================================================
// Text helpers (ASCII only so every font renders them).

// "+3.1", "-0.4", "0.0" (no "-0.0"). decimals 0..3.
std::string formatSigned (float value, int decimals = 1);

// "-14.2 LUFS" style; "--" for kNoValue.
std::string formatValue (float value, int decimals, const char* unit);

} // namespace rm
