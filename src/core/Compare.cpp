#include "core/Compare.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

//==============================================================================
// Hints
//
// Written for a producer who mixes by ear: every hint names the measured number, says what the difference usually
// sounds like and suggests one concrete thing to try in FL Studio. Differences are always "compared with your
// reference" - the reference is a target the user chose, not a law. Text is ASCII only.
//
// Ranking: Setup hints first, then severity, then `rank` = size of the difference relative to its topic's threshold
// (Tone 3 dB, Loudness 3 LU, PLR 4 dB, width 40 points...), so differences in different units compare fairly.
//==============================================================================

namespace rm
{
float referenceMatchGainDb (float mixLufs, float referenceLufs) noexcept
{
    if (! hasValue (referenceLufs) || ! std::isfinite (referenceLufs))
        return 0.0f;

    const float mix = hasValue (mixLufs) && std::isfinite (mixLufs) ? mixLufs : kFallbackMixLufs;
    return std::clamp (mix - referenceLufs, kMinMatchGainDb, kMaxMatchGainDb);
}

Severity bandSeverity (float levelDiffDb) noexcept
{
    const float size = std::abs (levelDiffDb);

    if (size >= kBandMajorDb)
        return Severity::Major;

    if (size >= kBandMinorDb)
        return Severity::Minor;

    return Severity::Good;   // also for NaN
}

Severity bandSeverity (float levelDiffDb, Severity previous) noexcept
{
    const float size = std::abs (levelDiffDb);

    if (std::isnan (size))
        return Severity::Good;

    const auto threshold = [] (Severity s) { return s == Severity::Major ? kBandMajorDb : kBandMinorDb; };
    Severity held = (previous == Severity::Minor || previous == Severity::Major) ? previous : Severity::Good;

    // Up: only when the higher threshold is passed by the margin.
    if (held != Severity::Major && size >= kBandMajorDb + kBandHysteresisDb)
        return Severity::Major;

    if (held == Severity::Good && size >= kBandMinorDb + kBandHysteresisDb)
        return Severity::Minor;

    // Down: only when the current threshold is undercut by the margin (possibly by more than one level).
    while (held != Severity::Good && size < threshold (held) - kBandHysteresisDb)
        held = held == Severity::Major ? Severity::Minor : Severity::Good;

    return held;
}

//==============================================================================
std::string formatSigned (float value, int decimals)
{
    if (! std::isfinite (value) || value == kNoValue)
        return "--";

    decimals = std::clamp (decimals, 0, 3);
    const double scale = std::pow (10.0, decimals);
    double rounded = std::round ((double) value * scale) / scale;

    if (rounded == 0.0)
        rounded = 0.0;   // turns -0.0 into 0.0

    char text[64];
    std::snprintf (text, sizeof (text), rounded > 0.0 ? "+%.*f" : "%.*f", decimals, rounded);
    return text;
}

std::string formatValue (float value, int decimals, const char* unit)
{
    if (! std::isfinite (value) || ! hasValue (value))
        return "--";

    decimals = std::clamp (decimals, 0, 3);
    const double scale = std::pow (10.0, decimals);
    double rounded = std::round ((double) value * scale) / scale;

    if (rounded == 0.0)
        rounded = 0.0;

    char text[64];
    std::snprintf (text, sizeof (text), "%.*f", decimals, rounded);

    std::string result (text);
    if (unit != nullptr && *unit != 0)
        result += std::string (" ") + unit;

    return result;
}

//==============================================================================
namespace
{
struct RankedHint
{
    Hint hint;
    float rank = 0.0f;
};

struct BandAdvice
{
    const char* more;   // the mix has more of this band than the reference
    const char* less;
};

// Indexed by Band. Each: what the difference usually sounds like + one thing to try.
const BandAdvice bandAdvice[kNumBands] = {
    { "Compared with your reference, your mix has more sub bass. That eats headroom and can sound boomy on big "
      "speakers. Try turning the sub bass (or 808) down 1-2 dB, or a small cut around 40-60 Hz on the kick. With "
      "Fruity Parametric EQ 2, low-cut pads, leads and FX below about 100 Hz so only kick and bass fill this range.",
      "Compared with your reference, your mix has less sub bass, so it may feel weak on club systems and good "
      "headphones. Try turning the sub bass (or 808) up 1-2 dB, or a gentle boost around 40-60 Hz on it or the kick "
      "with Fruity Parametric EQ 2. Breakdowns without kick have much less sub, so compare the same part (e.g. the "
      "drop). Laptop speakers can't play this range." },
    { "Compared with your reference, your mix has more bass and kick body, which can sound boomy or muddy. Try "
      "lowering the bass or kick by 1-2 dB, or cut 100-200 Hz slightly on pads, keys and guitars with Fruity "
      "Parametric EQ 2 so kick and bass get their own space.",
      "Compared with your reference, your mix has less bass and kick body, which can sound thin. Try turning the "
      "bass or kick up 1-2 dB, or a gentle boost around 80-120 Hz on them with Fruity Parametric EQ 2." },
    { "Compared with your reference, your mix has more low mids - this is where mud and boxiness live. Try cutting "
      "200-500 Hz a little on pads, guitars and synths with Fruity Parametric EQ 2 (wide band, 2-3 dB).",
      "Compared with your reference, your mix has fewer low mids, which can sound thin or cold. Try a gentle, wide "
      "boost around 250-400 Hz on vocals, keys or snare, or lower the low-cut filters on those tracks." },
    { "Compared with your reference, your mix has more mids, which can sound honky, boxy or crowded. Try a gentle, "
      "wide cut around 800 Hz - 1.5 kHz on the busiest parts (synths, guitars, backing vocals) with Fruity "
      "Parametric EQ 2.",
      "Compared with your reference, your mix has fewer mids, so vocals and instruments can sound hollow or far "
      "away. Try turning up the lead vocal or main melody, or a gentle boost around 1 kHz on them." },
    { "Compared with your reference, your mix has more high mids, which can sound harsh or tiring. Try a small cut "
      "around 2-5 kHz on bright synths, guitars or hi-hats, or a de-esser on the vocal.",
      "Compared with your reference, your mix has fewer high mids: it can sound dull, and vocals lose clarity. Try "
      "a gentle boost around 3 kHz on the vocal or lead with Fruity Parametric EQ 2." },
    { "Compared with your reference, your mix has more air and brightness, which can sound sharp or hissy. Try a "
      "gentle high-shelf cut above 8 kHz on hi-hats, cymbals or the vocal.",
      "Compared with your reference, your mix has less air, which can sound dull or closed-in. Try a gentle "
      "high-shelf boost above 8-10 kHz on vocals, hats or the whole mix with Fruity Parametric EQ 2." },
};

std::string lowerCase (std::string s)
{
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z')
            c = (char) (c - 'A' + 'a');

    return s;
}

std::string percent (float value)
{
    return formatValue (std::round (value), 0, nullptr) + "%";
}

void add (std::vector<RankedHint>& out, Severity severity, Topic topic, int band, float amount, float rank,
          std::string title, std::string detail)
{
    RankedHint h;
    h.hint.severity = severity;
    h.hint.topic = topic;
    h.hint.band = band;
    h.hint.amount = amount;
    h.hint.title = std::move (title);
    h.hint.detail = std::move (detail);
    h.rank = rank;
    out.push_back (std::move (h));
}

//==============================================================================
// Setup: what the user has to do before there is anything to compare.
void addSetupHints (const TrackProfile& mix, const TrackProfile& ref, std::vector<RankedHint>& out)
{
    if (! ref.valid)
    {
        if (ref.analysedSeconds > 0.0)
            add (out, Severity::Info, Topic::Setup, -1, 0.0f, 2.0f, "Reference region is too short",
                 "Select at least a few seconds of the reference that contain music (drag across the waveform), or "
                 "double-click the waveform to use the whole track.");
        else
            add (out, Severity::Info, Topic::Setup, -1, 0.0f, 2.0f, "Load a reference track",
                 "Drop a WAV, MP3, FLAC, AIFF or OGG file on RefMatch (or click Load). Pick a released song you like in a "
                 "similar style - RefMatch plays it at the same loudness as your mix and shows how the two differ.");
    }

    if (! mix.valid)
        add (out, Severity::Info, Topic::Setup, -1, 0.0f, 1.0f, "Play your track for a few seconds",
             "Put RefMatch as the last plugin on FL Studio's Master track (after your limiter, if you use one), then "
             "press play - ideally on the same section as the reference, e.g. the drop.");
}

//==============================================================================
// Checks that need only the mix (they also run without a reference).
void addTruePeakHint (const TrackProfile& mix, const TrackProfile& ref, std::vector<RankedHint>& out)
{
    if (! hasValue (mix.truePeakDb) || mix.truePeakDb <= kTruePeakCeilingDbtp)
        return;

    // A limiter ceiling caps the samples, not the peaks between them (1-3 dB higher on clipped masters), and RefMatch
    // sits in the master's effect slots, before the FL master fader - so neither "ceiling -1.0" nor the fader fixes it.
    // Maximus's CEIL is a soft-saturation ceiling, so for Maximus the output (POST) gain is the control to lower. The
    // true peak is a maximum since reset: the user has to reset the analysis to see the effect of a change.
    std::string detail = "Your mix peaks at " + formatSigned (mix.truePeakDb, 1)
                       + " dBTP; above -1 dBTP the MP3/AAC versions on streaming services can distort. Peaks between "
                         "samples overshoot a limiter's ceiling: set Fruity Limiter's ceiling to about -2 dB (in "
                         "Maximus, turn the master POST down 2 dB), then click Reset analysis and play again. The FL "
                         "master fader won't help - RefMatch measures before it.";

    // The reference is over -1 dBTP too (common for loud club masters): say so, and when it peaks at least as high as
    // the mix the hint is only Minor.
    const bool referenceAsHot = ref.valid && hasValue (ref.truePeakDb) && ref.truePeakDb > kTruePeakCeilingDbtp;

    if (referenceAsHot)
        detail += " Your reference peaks at " + formatSigned (ref.truePeakDb, 1)
                + " dBTP too - common for loud club masters.";

    const bool minor = referenceAsHot && mix.truePeakDb <= ref.truePeakDb;
    const float excess = mix.truePeakDb - kTruePeakCeilingDbtp;
    add (out, minor ? Severity::Minor : Severity::Major, Topic::TruePeak, -1, excess, 1.0f + excess,
         "True peak " + formatSigned (mix.truePeakDb, 1) + " dBTP - may distort when streamed", detail);
}

void addLowEndHint (const TrackProfile& mix, const TrackProfile& ref, std::vector<RankedHint>& out)
{
    const float corr = mix.lowEndCorrelation;

    if (! mix.valid || mix.isMono || ! (corr < kLowEndCorrMinor))
        return;

    // Only meaningful when the mix actually has low end (otherwise the correlation measures noise).
    const float lowEndDb = std::max (mix.bands[(size_t) Band::Sub].levelDb, mix.bands[(size_t) Band::Low].levelDb);
    if (! hasValue (lowEndDb) || ! hasValue (mix.spectralLoudnessDb) || lowEndDb < mix.spectralLoudnessDb - 30.0f)
        return;

    // Severity comes from the mix alone (mono compatibility is a technical property); the reference only adds context.
    const bool major = corr < kLowEndCorrMajor;

    std::string detail = "Below 120 Hz your left and right channels differ";
    if (ref.valid)
        detail += " (reference: " + formatValue (ref.lowEndCorrelation, 2, nullptr) + ")";

    if (corr < 0.0f)
        detail += ", so the bass partly cancels itself when played in mono (club systems, phones, Bluetooth "
                  "speakers). ";
    else if (major)
        detail += ", so bass and kick lose power when played in mono (club systems, phones, Bluetooth speakers). ";
    else
        detail += ", so bass and kick may lose some power when played in mono (club systems, phones, Bluetooth "
                  "speakers). ";

    detail += "Keep the low end mono: remove stereo wideners, chorus and stereo reverb from the bass, 808 and kick, or "
              "turn the stereo separation knob on their own mixer track (not the Master) fully right, which makes it "
              "mono. Keep any wide layers above about 150 Hz. Check with the Mono button.";

    if (ref.valid && ref.lowEndCorrelation < kLowEndCorrMinor && corr >= 0.0f)
        detail += " Your reference is not fully mono down low either, so part of this may be a style choice.";

    add (out, major ? Severity::Major : Severity::Minor, Topic::LowEnd, -1, corr,
         (1.0f - corr) / (1.0f - kLowEndCorrMajor),
         (major ? "Bass is not mono-compatible (correlation " : "Bass is a bit wide (correlation ")
             + formatValue (corr, 2, nullptr) + ")",
         detail);
}

//==============================================================================
// Mix vs reference.
void addLoudnessHint (const Comparison& c, const TrackProfile& mix, const TrackProfile& ref,
                      std::vector<RankedHint>& out)
{
    const float diff = c.loudnessDiffLu;

    if (! hasValue (diff) || std::abs (diff) <= kLoudnessDiffLu)
        return;

    const std::string levels = " (" + formatValue (mix.integratedLufs, 1, "LUFS") + " vs "
                             + formatValue (ref.integratedLufs, 1, "LUFS") + "; 1 LU is about 1 dB)";

    if (diff < 0.0f)
    {
        add (out, Severity::Info, Topic::Loudness, -1, diff, std::abs (diff) / kLouderMajorLu,
             "Mix is " + formatValue (-diff, 1, "LU") + " quieter",
             "Compared with your reference, your mix is quieter" + levels
                 + ". That is normal before mastering - RefMatch plays both at the same loudness so you can judge "
                   "the sound, not the volume. When you master, bring the level up with a limiter on the master "
                   "(Fruity Limiter or Maximus) and keep the true peak below -1 dBTP.");
    }
    else
    {
        add (out, diff > kLouderMajorLu ? Severity::Major : Severity::Minor, Topic::Loudness, -1, diff,
             diff / kLouderMajorLu, "Mix is " + formatValue (diff, 1, "LU") + " louder",
             "Compared with your reference, your mix is louder" + levels
                 + ". Extra loudness usually comes from heavy limiting, which costs punch and clarity. Try backing "
                   "off the limiter or clipper on the master by a dB or two and compare again.");
    }
}

void addDynamicsHint (const Comparison& c, std::vector<RankedHint>& out)
{
    const float diff = c.plrDiffDb;

    if (! hasValue (diff) || std::abs (diff) <= kPlrDiffDb)
        return;

    const std::string plr = "PLR (peak-to-loudness ratio) is how far your peaks rise above the average level. ";
    const float rank = std::abs (diff) / kPlrMajorDb;

    if (diff > 0.0f)
    {
        // Peakier than the reference: expected for a mix that has not been mastered (and is therefore quieter).
        const bool unmastered = hasValue (c.loudnessDiffLu) && c.loudnessDiffLu < -kLouderMajorLu;

        add (out, unmastered ? Severity::Info : Severity::Minor, Topic::Dynamics, -1, diff, rank,
             "More dynamic than reference (PLR " + formatSigned (diff, 1) + " dB)",
             plr + (unmastered
                        ? "Yours is higher than the reference's - normal for a mix that has not been mastered yet. "
                          "Compression and limiting when you master will bring it closer; do it gently and compare "
                          "by ear."
                        : "Yours is higher than the reference's, so your mix may sound less dense and powerful at "
                          "the same loudness. Try gentle bus compression on the drums and the whole mix (Fruity "
                          "Compressor, 2-3 dB of gain reduction) or a little more limiting on the master."));
    }
    else
    {
        add (out, diff < -kPlrMajorDb ? Severity::Major : Severity::Minor, Topic::Dynamics, -1, diff, rank,
             "More squashed than reference (PLR " + formatSigned (diff, 1) + " dB)",
             plr + "Yours is lower than the reference's, which usually means more compression or limiting (or a "
                   "very dense arrangement). That can sound loud but flat and tiring. If you use a limiter or clipper "
                   "on the master, try backing it off a little, or use less compression on the drum bus.");
    }
}

void addToneHints (const Comparison& c, std::vector<RankedHint>& out)
{
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& bc = c.bands[(size_t) b];

        if (! bc.valid || bc.severity == Severity::Good)
            continue;

        const auto& advice = bandAdvice[b];
        add (out, bc.severity, Topic::Tone, b, bc.levelDiffDb, std::abs (bc.levelDiffDb) / kBandMajorDb,
             std::string (bandInfo (b).name) + " " + formatSigned (bc.levelDiffDb, 1) + " dB vs reference",
             bc.levelDiffDb > 0.0f ? advice.more : advice.less);
    }
}

void addStereoHints (const Comparison& c, const TrackProfile& mix, const TrackProfile& ref,
                     std::vector<RankedHint>& out)
{
    if (mix.correlation < 0.0f)
    {
        add (out, Severity::Major, Topic::Stereo, -1, mix.correlation, 1.0f - mix.correlation,
             "Left and right partly cancel (correlation " + formatValue (mix.correlation, 2, nullptr) + ")",
             "Overall your left and right channels are more opposite than alike, so parts of your mix can disappear "
             "in mono. Look for a channel with flipped polarity, extreme stereo wideners or phase effects, and check "
             "with the Mono button.");
        return;
    }

    if (mix.isMono && ! ref.isMono && ref.widthPct >= kWidthMinorPct)
    {
        add (out, Severity::Major, Topic::Stereo, -1, -ref.widthPct, 2.0f, "Mix is mono, reference is stereo",
             "Left and right of your mix are identical (reference width " + percent (ref.widthPct)
                 + "). Pan instruments left and right (hats, percussion, doubled guitars, backing vocals) and use "
                   "stereo reverb or delay for space - keep kick, bass and lead vocal in the centre.");
        return;
    }

    // Per-band width from the low mids up (below that, the low-end mono check speaks). One hint per direction,
    // about the band with the biggest difference; the other bands are mentioned in its text.
    for (int direction : { -1, 1 })
    {
        int worst = -1;
        std::string others;

        for (int b = (int) Band::LowMid; b < kNumBands; ++b)
        {
            const auto& bc = c.bands[(size_t) b];
            const float d = bc.widthDiffPct * (float) direction;

            if (! bc.valid || d < kWidthMinorPct)
                continue;

            if (worst < 0 || d > c.bands[(size_t) worst].widthDiffPct * (float) direction)
                worst = b;
        }

        if (worst < 0)
            continue;

        for (int b = (int) Band::LowMid; b < kNumBands; ++b)
        {
            const auto& bc = c.bands[(size_t) b];
            if (b != worst && bc.valid && bc.widthDiffPct * (float) direction >= kWidthMinorPct)
                others += (others.empty() ? "" : ", ") + lowerCase (bandInfo (b).name);
        }

        const auto& bc = c.bands[(size_t) worst];
        const std::string bandName = bandInfo (worst).name;
        const std::string also = others.empty() ? "" : " (also in the " + others + ")";
        const Severity severity = std::abs (bc.widthDiffPct) >= kWidthMajorPct ? Severity::Major : Severity::Minor;
        const std::string widths = "(width " + percent (bc.mixWidthPct) + " vs " + percent (bc.refWidthPct) + ")";

        if (direction < 0)
            add (out, severity, Topic::Stereo, worst, bc.widthDiffPct, std::abs (bc.widthDiffPct) / kWidthMajorPct,
                 bandName + " narrower than reference " + widths,
                 "Compared with your reference, your mix is narrower in the " + lowerCase (bandName) + also
                     + ". Try panning hats, percussion and doubled parts left and right, or widen pads and synths a "
                       "little (Fruity Stereo Shaper, stereo reverb or delay). Keep kick, bass and lead vocal "
                       "centred.");
        else
            add (out, severity, Topic::Stereo, worst, bc.widthDiffPct, std::abs (bc.widthDiffPct) / kWidthMajorPct,
                 bandName + " wider than reference " + widths,
                 "Compared with your reference, your mix is wider in the " + lowerCase (bandName) + also
                     + ". Very wide sound can feel hollow and partly disappears in mono and on phone speakers. "
                       "Check with the Mono button and turn down stereo wideners or chorus on the widest parts.");
    }
}
} // namespace

//==============================================================================
Comparison compareProfiles (const TrackProfile& mix, const TrackProfile& reference)
{
    return compareProfiles (mix, reference, nullptr);
}

Comparison compareProfiles (const TrackProfile& mix, const TrackProfile& reference, const Comparison* previous)
{
    // Only a valid previous comparison holds severities (and it is only read, so it may alias the result's target).
    if (previous != nullptr && ! previous->valid())
        previous = nullptr;

    Comparison c;
    c.mixReady = mix.valid;
    c.referenceReady = reference.valid;

    const auto diffOf = [] (float a, float b) { return hasValue (a) && hasValue (b) ? a - b : kNoValue; };

    if (c.valid())
    {
        c.loudnessDiffLu = diffOf (mix.integratedLufs, reference.integratedLufs);
        c.truePeakDiffDb = diffOf (mix.truePeakDb, reference.truePeakDb);
        c.plrDiffDb = diffOf (mix.plrDb, reference.plrDb);

        if (hasValue (mix.integratedLufs) && hasValue (reference.integratedLufs))
            c.lraDiffLu = mix.loudnessRangeLu - reference.loudnessRangeLu;

        const bool spectralKnown = hasValue (mix.spectralLoudnessDb) && hasValue (reference.spectralLoudnessDb);

        if (spectralKnown)
            c.normalisationOffsetDb = reference.spectralLoudnessDb - mix.spectralLoudnessDb;

        for (size_t b = 0; b < (size_t) kNumBands; ++b)
        {
            const auto& m = mix.bands[b];
            const auto& r = reference.bands[b];
            auto& bc = c.bands[b];

            bc.mixWidthPct = m.widthPct;
            bc.refWidthPct = r.widthPct;
            bc.widthDiffPct = m.widthPct - r.widthPct;
            bc.valid = spectralKnown && hasValue (m.levelDb) && hasValue (r.levelDb);

            if (bc.valid)
            {
                bc.levelDiffDb = (m.levelDb - mix.spectralLoudnessDb) - (r.levelDb - reference.spectralLoudnessDb);
                bc.severity = previous != nullptr && previous->bands[b].valid
                                  ? bandSeverity (bc.levelDiffDb, previous->bands[b].severity)
                                  : bandSeverity (bc.levelDiffDb);
            }
        }

        for (size_t i = 0; i < (size_t) kCurvePoints; ++i)
            if (hasValue (mix.curveDb[i]) && hasValue (reference.curveDb[i]))
                c.curveDiffDb[i] = mix.curveDb[i] + c.normalisationOffsetDb - reference.curveDb[i];
    }

    std::vector<RankedHint> ranked;
    addSetupHints (mix, reference, ranked);

    if (c.mixReady)
    {
        addTruePeakHint (mix, reference, ranked);
        addLowEndHint (mix, reference, ranked);
    }

    if (c.valid())
    {
        addLoudnessHint (c, mix, reference, ranked);
        addDynamicsHint (c, ranked);
        addToneHints (c, ranked);
        addStereoHints (c, mix, reference, ranked);

        const bool anythingToDo = std::any_of (ranked.begin(), ranked.end(), [] (const RankedHint& h)
        {
            return h.hint.severity == Severity::Minor || h.hint.severity == Severity::Major;
        });

        if (! anythingToDo)
            add (ranked, Severity::Good, Topic::Tone, -1, 0.0f, 0.0f, "Close to your reference",
                 "Tonal balance, peaks, dynamics and stereo image are all within normal differences of your "
                 "reference. Now trust your ears: switch between Mix and Reference on the same section and listen "
                 "for details.");
    }

    std::stable_sort (ranked.begin(), ranked.end(), [] (const RankedHint& a, const RankedHint& b)
    {
        const bool setupA = a.hint.topic == Topic::Setup, setupB = b.hint.topic == Topic::Setup;
        if (setupA != setupB)
            return setupA;

        if (a.hint.severity != b.hint.severity)
            return (int) a.hint.severity > (int) b.hint.severity;

        return a.rank > b.rank;
    });

    for (auto& h : ranked)
    {
        if ((int) c.hints.size() >= kMaxHints)
            break;

        c.hints.push_back (std::move (h.hint));
    }

    return c;
}

} // namespace rm
