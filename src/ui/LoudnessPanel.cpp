#include "ui/LoudnessPanel.h"

namespace rm::ui
{
namespace
{
constexpr float kEase = 0.3f;
constexpr int kNumRows = 5;

struct Message
{
    juce::String full, compact;   // compact: used when the full text does not fit
    rm::Severity severity = rm::Severity::Info;
};

struct RowSpec
{
    const char* name;
    const char* unit;
    float barMin, barMax;
    const char* tooltip;
};

const RowSpec kRows[kNumRows] = {
    { "Integrated", "LUFS", -30.0f, 0.0f,
      "Integrated loudness (LUFS): the average loudness of everything you played since the last reset, as your ears "
      "hear it. Streaming services play songs at about -14 LUFS; club and pop masters are often -9 to -6 LUFS. A mix "
      "before mastering is usually quieter than a finished master - that is fine. RefMatch measures before FL's "
      "Master fader, so keep that fader at 0 dB." },
    { "Short-term", "LUFS", -30.0f, 0.0f,
      "Short-term loudness (LUFS): how loud the last 3 seconds were. The reference is measured as it plays, so loop "
      "the same part of both songs to compare them." },
    { "True peak", "dBTP", -12.0f, 3.0f,
      "True peak (dBTP): the highest point of the waveform since the last reset, including peaks that appear between "
      "samples when the audio is converted. Keep the finished master below -1 dBTP so it does not distort on "
      "streaming services or as MP3. RefMatch measures before FL's Master fader, so keep that fader at 0 dB." },
    { "Dynamics", "PLR", 0.0f, 20.0f,
      "Dynamics (PLR, peak-to-loudness ratio, in dB): the highest sample peak minus the integrated loudness - how far "
      "the peaks stick out above the average level. Loud, heavily limited masters are around 6 - 9 dB; dynamic mixes "
      "12 dB or more." },
    { "Loudness range", "LRA", 0.0f, 20.0f,
      "Loudness range (LRA, in LU): how much the loudness changes between the quieter and louder parts of the song. "
      "Pop and EDM masters are often 3 - 7 LU; a much higher value means big jumps between parts." },
};

juce::String abs1 (float v) { return fmt::number (std::abs (v), 1); }

// The value as the row shows it (true peak above 0 dBTP shows its sign: "+0.4").
juce::String valueText (int row, float v)
{
    return row == 2 ? fmt::signedNumber (v, 1) : fmt::number (v, 1);
}

// The one-liners follow the same rules (and thresholds) as the hint cards in Compare.cpp, so a row and a card never
// disagree about the same numbers.
Message integratedMessage (float m, float r, bool hasReference)
{
    if (! rm::hasValue (m))
        return hasReference ? Message { "Play the same part as the reference to measure it", "Play your song to measure it",
                                        rm::Severity::Info }
                            : Message { "Play your song to measure it", "Play your song to measure it", rm::Severity::Info };

    if (! rm::hasValue (r))
        return hasReference ? Message { "Waiting for the reference analysis", "Analysing the reference", rm::Severity::Info }
                            : Message { "Load a reference to compare", "Load a reference to compare", rm::Severity::Info };

    const float d = m - r;

    if (std::abs (d) <= rm::kLoudnessDiffLu)
        return { "About as loud as the reference", "As loud as the reference", rm::Severity::Good };

    if (d < 0.0f)
        return { abs1 (d) + " LU quieter than the reference - normal before mastering",
                 abs1 (d) + " LU quieter - normal before mastering", rm::Severity::Info };

    return { abs1 (d) + " LU louder than the reference - check the limiter isn't squashing it",
             abs1 (d) + " LU louder than the reference", d > rm::kLouderMajorLu ? rm::Severity::Major : rm::Severity::Minor };
}

Message shortTermMessage (float m, float r, bool hasReference)
{
    if (! rm::hasValue (m))
        return { "Loudness of the last 3 seconds while FL plays", "Last 3 seconds while FL plays", rm::Severity::Info };

    if (! hasReference)
        return { "Loudness of the last 3 seconds", "Last 3 seconds", rm::Severity::Info };

    if (! rm::hasValue (r))
        return { "Compared with the reference while it plays", "Compared while the reference plays", rm::Severity::Info };

    const float d = m - r;

    if (std::abs (d) <= 1.5f)
        return { "Right now about as loud as this part of the reference", "Right now as loud as the reference",
                 rm::Severity::Good };

    return { "Right now " + abs1 (d) + " LU " + (d < 0.0f ? "quieter" : "louder") + " than this part of the reference",
             abs1 (d) + " LU " + (d < 0.0f ? "quieter" : "louder") + " than the reference here", rm::Severity::Info };
}

// unmastered: the mix is quieter than the reference by more than kLouderMajorLu (the same test as Compare.cpp).
Message truePeakMessage (float m, bool referenceKnown, float r, bool unmastered)
{
    if (! rm::hasValue (m))
        return { "Highest peak so far - play the loudest part", "Play the loudest part", rm::Severity::Info };

    if (m > rm::kTruePeakCeilingDbtp)
    {
        // Minor when the reference peaks over -1 dBTP too and at least as high as the mix (loud club masters).
        if (referenceKnown && rm::hasValue (r) && r > rm::kTruePeakCeilingDbtp && m <= r)
            return { "Above -1 dBTP - the reference peaks this high too", "Above -1 dBTP, like the reference",
                     rm::Severity::Minor };

        return { "Above -1 dBTP - can distort on streaming and as MP3", "Above -1 dBTP - can distort", rm::Severity::Major };
    }

    if (m > -3.0f)
        return { "Below -1 dBTP - OK for most streaming", "Below -1 dBTP", rm::Severity::Good };

    return { unmastered ? "Plenty of headroom - fine before mastering" : "Plenty of headroom", "Plenty of headroom",
             rm::Severity::Good };
}

Message dynamicsMessage (float m, float r, bool unmastered)
{
    if (! rm::hasValue (m))
        return { "Play your song to measure its dynamics", "Play your song to measure it", rm::Severity::Info };

    if (! rm::hasValue (r))
    {
        if (m < 8.0f)
            return { "Heavily limited - the peaks barely rise above the average", "Heavily limited", rm::Severity::Minor };

        return m < 12.0f ? Message { "Typical for a loud master", "Typical for a loud master", rm::Severity::Info }
                         : Message { "Dynamic - the peaks rise well above the average", "Dynamic", rm::Severity::Info };
    }

    const float d = m - r;

    if (std::abs (d) <= rm::kPlrDiffDb)
        return { "Similar dynamics to the reference", "Similar to the reference", rm::Severity::Good };

    if (d > 0.0f)
    {
        if (unmastered)
            return { abs1 (d) + " dB more dynamic than the reference - mastering closes this gap",
                     abs1 (d) + " dB more dynamic - fine before mastering", rm::Severity::Info };

        return { abs1 (d) + " dB more dynamic than the reference - may sound less dense",
                 abs1 (d) + " dB more dynamic than the reference", rm::Severity::Minor };
    }

    return { abs1 (d) + " dB more squashed than the reference - ease off the limiter",
             abs1 (d) + " dB more squashed - ease off the limiter",
             d < -rm::kPlrMajorDb ? rm::Severity::Major : rm::Severity::Minor };
}

Message rangeMessage (bool mixMeasured, float m, bool refMeasured, float r)
{
    if (! mixMeasured)
        return { "Needs a minute or so of playback to measure", "Needs about a minute of playback", rm::Severity::Info };

    if (! refMeasured)
        return { "How much the loudness moves over the song", "Loudness movement over the song", rm::Severity::Info };

    const float d = m - r;

    if (std::abs (d) <= 2.0f)
        return { "Similar movement between quiet and loud parts", "Similar to the reference", rm::Severity::Good };

    if (d > 0.0f)
        return { "More contrast between quiet and loud parts than the reference", "More contrast than the reference",
                 rm::Severity::Info };

    return { "Flatter than the reference - less contrast between parts", "Flatter than the reference", rm::Severity::Info };
}

// Values (mix, reference) and the one-liner of every row for this data.
struct RowData
{
    float mix = rm::kNoValue, ref = rm::kNoValue;
    Message message;
};

std::array<RowData, kNumRows> rowsFor (const ViewData& data)
{
    const auto& mix = data.mix;
    const auto& ref = data.reference;
    const bool hasReference = referenceExpected (data);

    // LRA reads 0 until enough short-term values exist: only show (and compare) it once it really is measured.
    auto rangeMeasured = [] (const rm::TrackProfile& p)
    {
        return rm::hasValue (p.integratedLufs) && rm::hasValue (p.shortTermLufs)
               && (p.loudnessRangeLu > 0.0f || p.analysedSeconds >= 10.0);
    };

    const bool mixMeasured = rangeMeasured (mix);
    const bool refMeasured = ref.valid && rangeMeasured (ref);

    const float refIntegrated = ref.valid ? ref.integratedLufs : rm::kNoValue;
    const float loudnessDiff = rm::hasValue (mix.integratedLufs) && rm::hasValue (refIntegrated)
                                   ? mix.integratedLufs - refIntegrated : rm::kNoValue;
    const bool unmastered = rm::hasValue (loudnessDiff) && loudnessDiff < -rm::kLouderMajorLu;

    // The reference's short-term loudness as it plays (measured by the processor; "--" until it played ~3 s).
    const float refShortTerm = hasReference ? data.referenceShortTermLufs : rm::kNoValue;

    std::array<RowData, kNumRows> rows;
    rows[0] = { mix.integratedLufs, refIntegrated, integratedMessage (mix.integratedLufs, refIntegrated, hasReference) };
    rows[1] = { mix.shortTermLufs, refShortTerm, shortTermMessage (mix.shortTermLufs, refShortTerm, hasReference) };
    rows[2] = { mix.truePeakDb, ref.valid ? ref.truePeakDb : rm::kNoValue,
                truePeakMessage (mix.truePeakDb, ref.valid, ref.truePeakDb, unmastered) };
    rows[3] = { mix.plrDb, ref.valid ? ref.plrDb : rm::kNoValue,
                dynamicsMessage (mix.plrDb, ref.valid ? ref.plrDb : rm::kNoValue, unmastered) };
    rows[4] = { mixMeasured ? mix.loudnessRangeLu : rm::kNoValue, refMeasured ? ref.loudnessRangeLu : rm::kNoValue,
                rangeMessage (mixMeasured, mix.loudnessRangeLu, refMeasured, ref.loudnessRangeLu) };
    return rows;
}

const char* severityName (rm::Severity s)
{
    switch (s)
    {
        case rm::Severity::Good:  return "good";
        case rm::Severity::Info:  return "info";
        case rm::Severity::Minor: return "minor";
        case rm::Severity::Major: return "major";
    }

    return "";
}

struct Value
{
    float shown = rm::kNoValue, target = rm::kNoValue;

    bool step (float newTarget, bool snap)
    {
        target = newTarget;

        if (! rm::hasValue (newTarget) || ! rm::hasValue (shown) || snap || std::abs (newTarget - shown) < 0.02f)
        {
            const bool changed = shown != newTarget;
            shown = newTarget;
            return changed;
        }

        shown += (newTarget - shown) * kEase;
        return true;
    }
};

struct Row
{
    Value mix, ref;
    Message message;
};
} // namespace

//==============================================================================
struct LoudnessPanel::Impl
{
    explicit Impl (LoudnessPanel& o) : owner (o) {}

    void layout()
    {
        auto inner = owner.getLocalBounds().toFloat().reduced (14.0f, 10.0f);
        titleRow = inner.removeFromTop (20.0f);
        inner.removeFromTop (4.0f);

        rowsArea = inner;
        const float rowH = rowsArea.getHeight() / (float) kNumRows;
        showMessages = rowH >= 34.0f;

        // (No size of its own above the default: the editor scales the whole UI uniformly.)
        const float w = inner.getWidth();
        nameW = juce::jlimit (96.0f, 170.0f, w * 0.32f);
        valueW = juce::jlimit (48.0f, 66.0f, w * 0.16f);
    }

    juce::Rectangle<float> rowBounds (int i) const
    {
        const float rowH = rowsArea.getHeight() / (float) kNumRows;
        return { rowsArea.getX(), rowsArea.getY() + rowH * (float) i, rowsArea.getWidth(), rowH };
    }

    float mixColumnX() const noexcept { return rowsArea.getX() + nameW; }
    float refColumnX() const noexcept { return mixColumnX() + valueW + 4.0f; }
    float barsX() const noexcept      { return refColumnX() + valueW + 14.0f; }

    int rowAt (juce::Point<float> p) const
    {
        for (int i = 0; i < kNumRows; ++i)
            if (rowBounds (i).contains (p))
                return i;

        return -1;
    }

    void paintTitle (juce::Graphics& g) const
    {
        drawSectionTitle (g, "Loudness & dynamics", titleRow.toNearestInt());

        const auto f = font (10.5f, 700).withExtraKerningFactor (0.08f);
        g.setFont (f);
        g.setColour (Colours::mix);
        g.drawText ("MIX", juce::Rectangle<float> (mixColumnX(), titleRow.getY(), valueW, titleRow.getHeight()),
                    juce::Justification::centredRight, false);
        g.setColour (Colours::reference);
        g.drawText ("REF", juce::Rectangle<float> (refColumnX(), titleRow.getY(), valueW, titleRow.getHeight()),
                    juce::Justification::centredRight, false);
    }

    void paintRow (juce::Graphics& g, int i) const
    {
        const auto& spec = kRows[i];
        const auto& row = rows[(size_t) i];
        auto r = rowBounds (i);

        if (i == hoverRow)
        {
            g.setColour (juce::Colours::white.withAlpha (0.03f));
            g.fillRoundedRectangle (r.expanded (6.0f, 0.0f), 5.0f);
        }

        if (i > 0)
        {
            g.setColour (Colours::outline.withAlpha (0.45f));
            g.fillRect (r.withHeight (1.0f));
        }

        const float lineH = 20.0f;
        const float contentH = showMessages ? lineH + 15.0f : lineH;
        auto content = r.withSizeKeepingCentre (r.getWidth(), contentH);
        auto top = content.removeFromTop (lineH);

        // Name + unit
        {
            auto nameArea = top.withWidth (nameW - 6.0f);
            const auto nameFont = font (12.5f, 600), unitFont = font (10.5f, 600);
            const float nw = juce::jmin (nameArea.getWidth(), textWidth (nameFont, spec.name) + 1.0f);
            g.setFont (nameFont);
            g.setColour (Colours::text);
            drawFittedLine (g, spec.name, nameArea.removeFromLeft (nw), juce::Justification::centredLeft, 0.8f);

            if (nameArea.getWidth() > textWidth (unitFont, spec.unit) + 6.0f)
            {
                g.setFont (unitFont);
                g.setColour (Colours::textSubtle);
                g.drawText (spec.unit, nameArea.withTrimmedLeft (5.0f).translated (0.0f, 0.5f), juce::Justification::centredLeft, false);
            }
        }

        // Values
        g.setFont (font (15.0f, 700));
        g.setColour (rm::hasValue (row.mix.shown) ? Colours::mix.brighter (0.1f) : Colours::textFaint);
        g.drawText (valueText (i, row.mix.shown), juce::Rectangle<float> (mixColumnX(), top.getY(), valueW, top.getHeight()),
                    juce::Justification::centredRight, false);
        g.setColour (rm::hasValue (row.ref.shown) ? Colours::reference.brighter (0.15f) : Colours::textFaint);
        g.drawText (valueText (i, row.ref.shown), juce::Rectangle<float> (refColumnX(), top.getY(), valueW, top.getHeight()),
                    juce::Justification::centredRight, false);

        // Comparison bars (mix above, reference below)
        const float barH = 4.0f, barGap = 2.0f;
        const auto barsArea = juce::Rectangle<float> (barsX(), top.getCentreY() - barH - barGap * 0.5f,
                                                      top.getRight() - barsX(), 2.0f * barH + barGap);

        if (barsArea.getWidth() > 30.0f)
        {
            auto drawBar = [&] (float value, juce::Colour colour, float y)
            {
                const auto track = juce::Rectangle<float> (barsArea.getX(), y, barsArea.getWidth(), barH);
                g.setColour (Colours::background.withAlpha (0.7f));
                g.fillRoundedRectangle (track, barH * 0.5f);

                if (! rm::hasValue (value))
                    return;

                const float norm = juce::jlimit (0.0f, 1.0f, (value - spec.barMin) / (spec.barMax - spec.barMin));
                g.setColour (colour);
                g.fillRoundedRectangle (track.withWidth (juce::jmax (barH, track.getWidth() * norm)), barH * 0.5f);
            };

            drawBar (row.mix.shown, Colours::mix, barsArea.getY());
            drawBar (row.ref.shown, Colours::reference.withAlpha (0.9f), barsArea.getY() + barH + barGap);

            // -1 dBTP limit marker on the true peak row
            if (i == 2)
            {
                const float x = barsArea.getX() + barsArea.getWidth() * (rm::kTruePeakCeilingDbtp - spec.barMin)
                                                                      / (spec.barMax - spec.barMin);
                g.setColour (Colours::major.withAlpha (0.8f));
                g.fillRect (juce::Rectangle<float> (x - 0.5f, barsArea.getY() - 3.0f, 1.0f, barsArea.getHeight() + 6.0f));
            }
        }

        // One-liner
        if (showMessages)
        {
            auto line = content;
            const auto colour = severityColour (row.message.severity);
            g.setColour (colour);
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ line.getX() + 3.0f, line.getCentreY() }));
            line.removeFromLeft (12.0f);

            const auto f = font (11.5f);
            const auto& text = textWidth (f, row.message.full) <= line.getWidth() ? row.message.full : row.message.compact;
            g.setFont (f);
            g.setColour (row.message.severity == rm::Severity::Good ? Colours::textDim
                                                                     : blend (Colours::textDim, colour, 0.35f).brighter (0.1f));
            drawFittedLine (g, text, line, juce::Justification::centredLeft, 0.9f);
        }
    }

    LoudnessPanel& owner;
    juce::Rectangle<float> titleRow, rowsArea;
    float nameW = 110.0f, valueW = 56.0f;
    bool showMessages = true;
    std::array<Row, kNumRows> rows {};
    int hoverRow = -1;
};

//==============================================================================
LoudnessPanel::LoudnessPanel() : impl (std::make_unique<Impl> (*this))
{
    setTitle ("Loudness and dynamics");
}

LoudnessPanel::~LoudnessPanel() = default;

void LoudnessPanel::setData (const ViewData& data, bool snap)
{
    auto& d = *impl;
    const auto newRows = rowsFor (data);
    bool changed = false;

    for (int i = 0; i < kNumRows; ++i)
    {
        auto& row = d.rows[(size_t) i];
        const auto& next = newRows[(size_t) i];
        changed |= row.mix.step (next.mix, snap);
        changed |= row.ref.step (next.ref, snap);

        if (row.message.full != next.message.full || row.message.compact != next.message.compact
            || row.message.severity != next.message.severity)
        {
            row.message = next.message;
            changed = true;
        }
    }

    if (changed)
        repaint();
}

juce::StringArray LoudnessPanel::describeRows (const ViewData& data)
{
    juce::StringArray lines;
    const auto rows = rowsFor (data);

    for (int i = 0; i < kNumRows; ++i)
    {
        const auto& r = rows[(size_t) i];
        lines.add (juce::String (kRows[i].name) + " | " + valueText (i, r.mix) + " | " + valueText (i, r.ref) + " | "
                   + severityName (r.message.severity) + " | " + r.message.full);
    }

    return lines;
}

void LoudnessPanel::resized()
{
    impl->layout();
}

void LoudnessPanel::paint (juce::Graphics& g)
{
    drawPanel (g, getLocalBounds().toFloat());
    impl->paintTitle (g);

    for (int i = 0; i < kNumRows; ++i)
        impl->paintRow (g, i);
}

void LoudnessPanel::mouseMove (const juce::MouseEvent& e)
{
    const int row = impl->rowAt (e.position);

    if (row != impl->hoverRow)
    {
        impl->hoverRow = row;
        repaint();
    }
}

void LoudnessPanel::mouseExit (const juce::MouseEvent&)
{
    if (impl->hoverRow >= 0)
    {
        impl->hoverRow = -1;
        repaint();
    }
}

juce::String LoudnessPanel::getTooltip()
{
    const auto pos = getMouseXYRelative().toFloat();
    const int row = impl->rowAt (pos);

    if (row < 0)
        return impl->titleRow.contains (pos) ? "Loudness and dynamics of your mix (mint) next to the reference (purple)."
                                             : juce::String();

    juce::String tip (kRows[row].tooltip);
    const auto& r = impl->rows[(size_t) row];
    tip << "\n\nMix " << valueText (row, r.mix.target) << ", reference " << valueText (row, r.ref.target) << ": "
        << r.message.full << ".";
    return tip;
}

} // namespace rm::ui
