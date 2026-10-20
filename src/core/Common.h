#pragma once

#include <array>
#include <cmath>

// Shared constants for the RefMatch analysis core (pure C++17, no JUCE).
namespace rm
{
// Sentinel for "no measurement yet" (silence, not enough audio). Every dB / LUFS getter returns this instead of
// -infinity so arithmetic never produces NaN. Test with hasValue().
inline constexpr float kNoValue = -200.0f;

inline bool hasValue (float dbOrLufs) noexcept { return dbOrLufs > -150.0f; }

inline float powerToDb (double power) noexcept
{
    return power > 1.0e-20 ? (float) (10.0 * std::log10 (power)) : kNoValue;
}

inline float gainToDb (double gain) noexcept
{
    return gain > 1.0e-10 ? (float) (20.0 * std::log10 (gain)) : kNoValue;
}

inline double dbToGain (double db) noexcept { return std::pow (10.0, db / 20.0); }

//==============================================================================
// Frequency bands used everywhere (tonal balance, stereo width, hints).
inline constexpr int kNumBands = 6;

enum class Band
{
    Sub = 0,     //    20 -    60 Hz  felt more than heard: sub bass, kick tail
    Low,         //    60 -   200 Hz  bass and kick body, warmth
    LowMid,      //   200 -   500 Hz  "mud" / boxiness
    Mid,         //   500 -  2000 Hz  body of instruments and vocals, "honk"
    HighMid,     //  2000 -  6000 Hz  presence, clarity, harshness
    Air          //  6000 - 20000 Hz  brightness, sizzle, air
};

struct BandInfo
{
    const char* name;        // "Low mids"
    const char* shortName;   // "Low-mid" (fits a 70 px column)
    float lowHz;
    float highHz;
    const char* character;   // one-line plain-language description of what lives there
};

// Band edges are contiguous: band i's highHz == band i+1's lowHz. Defined in Analysis.cpp.
const BandInfo& bandInfo (int bandIndex);
inline const BandInfo& bandInfo (Band b) { return bandInfo ((int) b); }

// Low-end mono compatibility is judged below this frequency.
inline constexpr float kLowEndMonoHz = 120.0f;

//==============================================================================
// Smoothed spectrum curve: kCurvePoints log-spaced points from kCurveMinHz to kCurveMaxHz (inclusive).
inline constexpr int kCurvePoints = 160;
inline constexpr float kCurveMinHz = 20.0f;
inline constexpr float kCurveMaxHz = 20000.0f;

inline float curveFrequency (int pointIndex) noexcept
{
    const double t = (double) pointIndex / (double) (kCurvePoints - 1);
    return (float) (kCurveMinHz * std::pow ((double) kCurveMaxHz / kCurveMinHz, t));
}

using Curve = std::array<float, kCurvePoints>;

} // namespace rm
