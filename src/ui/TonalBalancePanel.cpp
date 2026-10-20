#include "ui/TonalBalancePanel.h"

namespace rm::ui
{
namespace
{
constexpr float kEase = 0.28f;            // per 30 Hz tick
constexpr float kBarRangeDb = 6.0f;       // band bars: +-6 dB
constexpr float kAboveCentreDb = 12.0f;   // plot range around the curves' centre
constexpr float kBelowCentreDb = 21.0f;

const juce::String kDot = juce::String::fromUTF8 (" \xc2\xb7 ");

float logPosition (float hz) noexcept
{
    return (float) (std::log (juce::jlimit (rm::kCurveMinHz, rm::kCurveMaxHz, hz) / rm::kCurveMinHz)
                    / std::log (rm::kCurveMaxHz / rm::kCurveMinHz));
}

// Plain-language explanation of each band for the tooltips.
const char* bandExplanation (int band)
{
    switch (band)
    {
        case 0: return "Sub bass (20 - 60 Hz): the deep rumble you feel more than hear - sub bass and the tail of the kick. "
                       "Too much eats headroom and booms on big systems; too little feels weak in a club.";
        case 1: return "Bass (60 - 200 Hz): the body of the bass and the punch of the kick. Too much sounds boomy, too "
                       "little sounds thin.";
        case 2: return "Low mids (200 - 500 Hz): warmth, but also mud and boxiness. Too much makes the mix cloudy, too "
                       "little sounds thin or cold.";
        case 3: return "Mids (500 Hz - 2 kHz): the body of vocals, synths and guitars. Too much sounds honky or nasal, too "
                       "little sounds scooped and far away.";
        case 4: return "High mids (2 - 6 kHz): presence and clarity, where the ear is most sensitive. Too much sounds "
                       "harsh and tiring, too little sounds distant.";
        case 5: return "Air (6 - 20 kHz): brightness, cymbals, breath and sizzle. Too much sounds sharp or hissy, too "
                       "little sounds dull.";
        default: return "";
    }
}

struct Smoothed
{
    float shown = 0.0f, target = 0.0f;
    bool valid = false;

    // Returns true when the shown value moved.
    bool step (float newTarget, bool newValid, bool snap)
    {
        target = newTarget;

        if (! newValid)
        {
            const bool changed = valid;
            valid = false;
            return changed;
        }

        if (! valid || snap)
        {
            const bool changed = ! valid || shown != newTarget;
            shown = newTarget;
            valid = true;
            return changed;
        }

        const float delta = target - shown;

        if (std::abs (delta) < 0.01f)
        {
            const bool changed = shown != target;
            shown = target;
            return changed;
        }

        shown += delta * kEase;
        return true;
    }
};
} // namespace

//==============================================================================
struct TonalBalancePanel::Impl
{
    explicit Impl (TonalBalancePanel& o) : owner (o) {}

    void layout()
    {
        auto inner = owner.getLocalBounds().toFloat().reduced (14.0f, 10.0f);
        titleRow = inner.removeFromTop (20.0f);
        inner.removeFromTop (6.0f);

        const float barsH = juce::jlimit (40.0f, 130.0f, inner.getHeight() * 0.3f);
        bandArea = inner.removeFromBottom (15.0f + barsH + 15.0f + 13.0f);
        inner.removeFromBottom (2.0f);
        plot = inner;

        auto b = bandArea;
        namesRow = b.removeFromTop (15.0f);
        valueRow = b.removeFromBottom (13.0f + 15.0f);
        bars = b.reduced (0.0f, 2.0f);
    }

    float xForHz (float hz) const noexcept { return plot.getX() + logPosition (hz) * plot.getWidth(); }

    juce::Rectangle<float> bandColumn (int band, juce::Rectangle<float> rowArea) const
    {
        const auto& info = rm::bandInfo (band);
        const float x0 = xForHz (info.lowHz), x1 = xForHz (info.highHz);
        return { x0, rowArea.getY(), x1 - x0, rowArea.getHeight() };
    }

    int bandAt (juce::Point<float> p) const
    {
        if (! (plot.contains (p) || bandArea.contains (p)) || p.x < plot.getX() || p.x > plot.getRight())
            return -1;

        for (int b = 0; b < rm::kNumBands; ++b)
        {
            const auto& info = rm::bandInfo (b);

            if (p.x >= xForHz (info.lowHz) && p.x <= xForHz (info.highHz))
                return b;
        }

        return -1;
    }

    float yForDb (float db) const noexcept
    {
        const float top = centre.shown + kAboveCentreDb, bottom = centre.shown - kBelowCentreDb;
        return plot.getY() + (top - db) / (top - bottom) * plot.getHeight();
    }

    // Centre of the displayed range: median of the tilted curve between 60 Hz and 12 kHz (reference preferred).
    static float curveCentre (const std::array<Smoothed, rm::kCurvePoints>& curve, bool& ok)
    {
        std::vector<float> values;
        values.reserve ((size_t) rm::kCurvePoints);

        for (int i = 0; i < rm::kCurvePoints; ++i)
        {
            const float hz = rm::curveFrequency (i);

            if (hz >= 60.0f && hz <= 12000.0f && curve[(size_t) i].valid)
                values.push_back (curve[(size_t) i].target);
        }

        ok = values.size() > 8;

        if (! ok)
            return 0.0f;

        std::nth_element (values.begin(), values.begin() + (std::ptrdiff_t) (values.size() / 2), values.end());
        return values[values.size() / 2];
    }

    juce::Path curvePath (const std::array<Smoothed, rm::kCurvePoints>& curve, bool closedToBottom) const
    {
        juce::Path p;
        bool penDown = false;
        float firstX = 0.0f, lastX = 0.0f;
        const float yMin = plot.getY() - 40.0f, yMax = plot.getBottom() + 40.0f;

        for (int i = 0; i < rm::kCurvePoints; ++i)
        {
            const auto& pt = curve[(size_t) i];

            if (! pt.valid)
            {
                if (penDown && closedToBottom)
                {
                    p.lineTo (lastX, plot.getBottom());
                    p.lineTo (firstX, plot.getBottom());
                    p.closeSubPath();
                }

                penDown = false;
                continue;
            }

            const float x = xForHz (rm::curveFrequency (i));
            const float y = juce::jlimit (yMin, yMax, yForDb (pt.shown));

            if (! penDown)
            {
                firstX = x;

                if (closedToBottom)
                {
                    p.startNewSubPath (x, plot.getBottom());
                    p.lineTo (x, y);
                }
                else
                {
                    p.startNewSubPath (x, y);
                }

                penDown = true;
            }
            else
            {
                p.lineTo (x, y);
            }

            lastX = x;
        }

        if (penDown && closedToBottom)
        {
            p.lineTo (lastX, plot.getBottom());
            p.lineTo (firstX, plot.getBottom());
            p.closeSubPath();
        }

        return p;
    }

    // ---- painting -------------------------------------------------------------------------------------------------
    void paintTitle (juce::Graphics& g) const
    {
        drawSectionTitle (g, "Tonal balance", titleRow.toNearestInt());

        // Legend, right aligned: [line] Your mix   [area] Reference   (equal loudness)
        const auto f = font (11.5f, 600);
        const juce::String mixText ("Your mix"), refText ("Reference"), note ("at equal loudness");
        const auto noteFont = font (11.0f);
        const bool showNote = titleRow.getWidth() > 470.0f;
        float x = titleRow.getRight();

        if (showNote)
        {
            x -= textWidth (noteFont, note);
            g.setFont (noteFont);
            g.setColour (Colours::textSubtle);
            g.drawText (note, juce::Rectangle<float> (x, titleRow.getY(), titleRow.getRight() - x, titleRow.getHeight()),
                        juce::Justification::centredLeft, false);
            x -= 16.0f;
        }

        auto legendItem = [&] (const juce::String& text, juce::Colour colour, bool area)
        {
            const float w = textWidth (f, text);
            x -= w;
            g.setFont (f);
            g.setColour (Colours::textDim.brighter (0.2f));
            g.drawText (text, juce::Rectangle<float> (x, titleRow.getY(), w + 2.0f, titleRow.getHeight()),
                        juce::Justification::centredLeft, false);
            x -= 20.0f;
            const auto swatch = juce::Rectangle<float> (x, titleRow.getCentreY() - (area ? 5.0f : 1.25f), 14.0f, area ? 10.0f : 2.5f);
            g.setColour (area ? colour.withAlpha (0.45f) : colour);
            g.fillRoundedRectangle (swatch, area ? 2.5f : 1.25f);

            if (area)
            {
                g.setColour (colour.withAlpha (0.85f));
                g.fillRect (swatch.withHeight (1.5f));
            }

            x -= 14.0f;
        };

        legendItem (refText, Colours::reference, true);
        legendItem (mixText, Colours::mix, false);
    }

    void paintPlot (juce::Graphics& g) const
    {
        // Well
        g.setColour (Colours::background.withAlpha (0.5f));
        g.fillRoundedRectangle (plot.expanded (0.0f, 0.0f), 5.0f);

        // Hovered band column
        if (hoverBand >= 0)
        {
            g.setColour (juce::Colours::white.withAlpha (0.035f));
            g.fillRect (bandColumn (hoverBand, plot));
        }

        // Horizontal grid every 6 dB
        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (plot.toNearestInt());
            const float top = centre.shown + kAboveCentreDb, bottom = centre.shown - kBelowCentreDb;

            for (float db = std::floor (bottom / 6.0f) * 6.0f; db <= top; db += 6.0f)
            {
                const float y = yForDb (db);
                g.setColour (Colours::outline.withAlpha (0.28f));
                g.fillRect (juce::Rectangle<float> (plot.getX(), y - 0.5f, plot.getWidth(), 1.0f));
            }
        }

        // Band separators with frequency labels
        g.setFont (font (10.0f));

        for (int b = 1; b < rm::kNumBands; ++b)
        {
            const float hz = rm::bandInfo (b).lowHz;
            const float x = std::round (xForHz (hz));
            g.setColour (Colours::outline.withAlpha (0.75f));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, plot.getY() + 4.0f, 1.0f, plot.getHeight() - 8.0f));
            g.setColour (Colours::textSubtle);
            const juce::String label = hz >= 1000.0f ? juce::String (juce::roundToInt (hz / 1000.0f)) + "k" : juce::String ((int) hz);
            g.drawText (label, juce::Rectangle<float> (x + 4.0f, plot.getBottom() - 15.0f, 40.0f, 13.0f),
                        juce::Justification::centredLeft, false);
        }

        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (plot.toNearestInt());

            // Reference: soft filled area + thin top line
            if (refValid)
            {
                const auto area = curvePath (ref, true);
                g.setGradientFill (juce::ColourGradient::vertical (Colours::reference.withAlpha (0.36f), plot.getY(),
                                                                   Colours::reference.withAlpha (0.04f), plot.getBottom()));
                g.fillPath (area);
                g.setColour (Colours::reference.withAlpha (0.75f));
                g.strokePath (curvePath (ref, false), juce::PathStrokeType (1.2f, juce::PathStrokeType::curved));
            }

            // Mix: crisp mint line with a soft glow
            if (mixValid)
            {
                const auto line = curvePath (mix, false);
                g.setColour (Colours::mix.withAlpha (0.16f));
                g.strokePath (line, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.setColour (Colours::mix);
                g.strokePath (line, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
        }

        // Messages for missing data
        juce::String line1, line2;

        if (! mixValid && ! refValid)
        {
            line1 = "Press play in FL Studio";
            line2 = hasReference ? "RefMatch measures your mix on the master track"
                                 : "RefMatch measures your mix on the master track. Then load a reference above.";
        }
        else if (! mixValid)
        {
            line1 = "Press play in FL Studio to measure your mix";
        }
        else if (! refValid)
        {
            line1 = hasReference ? "Analysing the reference..." : "Load a reference to compare";
        }

        if (line1.isNotEmpty())
        {
            const auto f1 = font (line2.isEmpty() ? 13.0f : 15.0f, 600), f2 = font (12.0f);
            const float w = juce::jmin (plot.getWidth() - 20.0f, juce::jmax (textWidth (f1, line1), textWidth (f2, line2)) + 36.0f);
            const float h = line2.isEmpty() ? 30.0f : 50.0f;
            const auto box = juce::Rectangle<float> (w, h).withCentre (plot.getCentre());
            g.setColour (Colours::panel.withAlpha (0.88f));
            g.fillRoundedRectangle (box, 8.0f);
            g.setColour (Colours::outline.brighter (0.1f));
            g.drawRoundedRectangle (box.reduced (0.5f), 8.0f, 1.0f);

            auto t = box.reduced (14.0f, 6.0f);
            g.setFont (f1);
            g.setColour (Colours::text);
            drawFittedLine (g, line1, line2.isEmpty() ? t : t.removeFromTop (20.0f), juce::Justification::centred, 0.8f);

            if (line2.isNotEmpty())
            {
                g.setFont (f2);
                g.setColour (Colours::textDim);
                drawFittedLine (g, line2, t, juce::Justification::centred, 0.8f);
            }
        }
    }

    void paintBands (juce::Graphics& g) const
    {
        const float zeroY = bars.getCentreY();
        const float dbPx = (bars.getHeight() * 0.5f) / kBarRangeDb;

        // "Close to the reference" zone (+-1.5 dB) and the zero line
        g.setColour (Colours::good.withAlpha (0.07f));
        g.fillRect (juce::Rectangle<float> (plot.getX(), zeroY - rm::kBandMinorDb * dbPx, plot.getWidth(), 2.0f * rm::kBandMinorDb * dbPx));
        g.setColour (Colours::outline.brighter (0.1f));
        g.fillRect (juce::Rectangle<float> (plot.getX(), zeroY - 0.5f, plot.getWidth(), 1.0f));

        const auto nameFont = font (11.5f, 600), valueFont = font (12.5f, 700), wordFont = font (11.0f, 600);

        for (int b = 0; b < rm::kNumBands; ++b)
        {
            const auto column = bandColumn (b, bandArea);
            const auto& band = bands[(size_t) b];
            const auto colour = severityColour (bandSeverity[(size_t) b]);

            if (b == hoverBand)
            {
                g.setColour (juce::Colours::white.withAlpha (0.035f));
                g.fillRoundedRectangle (column.reduced (1.0f, 0.0f), 4.0f);
            }

            if (b > 0)
            {
                g.setColour (Colours::outline.withAlpha (0.5f));
                g.fillRect (juce::Rectangle<float> (std::round (column.getX()) - 0.5f, bars.getY(), 1.0f, bars.getHeight()));
            }

            // Name
            g.setFont (nameFont);
            g.setColour (Colours::textDim.brighter (0.1f));
            drawFittedLine (g, rm::bandInfo (b).name, bandColumn (b, namesRow).reduced (2.0f, 0.0f), juce::Justification::centred, 0.75f);

            // Bar
            if (band.valid)
            {
                const float v = juce::jlimit (-kBarRangeDb, kBarRangeDb, band.shown);
                const float barW = juce::jlimit (10.0f, 34.0f, column.getWidth() * 0.42f);
                const float y1 = zeroY - v * dbPx;
                auto bar = juce::Rectangle<float> (column.getCentreX() - barW * 0.5f, juce::jmin (zeroY, y1), barW, std::abs (y1 - zeroY));

                if (bar.getHeight() < 2.0f)
                    bar = bar.withSizeKeepingCentre (barW, 2.0f).withY (zeroY - 1.0f);

                g.setGradientFill (juce::ColourGradient::vertical (colour.withAlpha (v >= 0.0f ? 0.95f : 0.55f), bar.getY(),
                                                                   colour.withAlpha (v >= 0.0f ? 0.55f : 0.95f), bar.getBottom()));
                g.fillRoundedRectangle (bar, 3.0f);

                if (std::abs (band.shown) > kBarRangeDb)
                {
                    // Beyond the scale: small arrow at the end
                    const float tipY = v > 0.0f ? bar.getY() - 5.0f : bar.getBottom() + 5.0f;
                    const float baseY = v > 0.0f ? bar.getY() - 1.0f : bar.getBottom() + 1.0f;
                    juce::Path arrow;
                    arrow.addTriangle (column.getCentreX() - 5.0f, baseY, column.getCentreX() + 5.0f, baseY, column.getCentreX(), tipY);
                    g.setColour (colour);
                    g.fillPath (arrow);
                }
            }

            // Value + word
            auto valueArea = bandColumn (b, valueRow).reduced (2.0f, 0.0f);
            g.setFont (valueFont);
            g.setColour (band.valid ? colour.brighter (0.1f) : Colours::textFaint);
            drawFittedLine (g, band.valid ? fmt::signedNumber (band.target, 1) + " dB" : juce::String ("--"),
                            valueArea.removeFromTop (15.0f), juce::Justification::centred, 0.75f);

            if (band.valid)
            {
                g.setFont (wordFont);
                g.setColour (colour.withAlpha (0.8f));
                drawFittedLine (g, bandWord (b, band.target), valueArea, juce::Justification::centred, 0.75f);
            }
        }
    }

    TonalBalancePanel& owner;

    juce::Rectangle<float> titleRow, plot, bandArea, namesRow, bars, valueRow;

    std::array<Smoothed, rm::kCurvePoints> ref {}, mix {};
    std::array<Smoothed, rm::kNumBands> bands {};
    std::array<rm::Severity, rm::kNumBands> bandSeverity {};
    Smoothed centre;
    bool refValid = false, mixValid = false, hasReference = false;
    int hoverBand = -1;
};

//==============================================================================
TonalBalancePanel::TonalBalancePanel() : impl (std::make_unique<Impl> (*this))
{
    setTitle ("Tonal balance");
}

TonalBalancePanel::~TonalBalancePanel() = default;

float TonalBalancePanel::displayTiltDb (float frequencyHz) noexcept
{
    return 4.5f * std::log2 (juce::jmax (1.0f, frequencyHz) / 1000.0f);
}

juce::String TonalBalancePanel::bandWord (int band, float diff)
{
    // Comparative words: the bars show differences from one reference, not faults of the mix on their own.
    if (std::abs (diff) < rm::kBandMinorDb)
        return "close";

    const bool more = diff > 0.0f;

    switch (band)
    {
        case 0: return more ? "heavier" : "lighter";
        case 1: return more ? "boomier" : "thinner";
        case 2: return more ? "muddier" : "colder";
        case 3: return more ? "honkier" : "more scooped";
        case 4: return more ? "harsher" : "more distant";
        case 5: return more ? "brighter" : "duller";
        default: return {};
    }
}

juce::String TonalBalancePanel::bandDifferenceText (float diff)
{
    if (std::abs (diff) < 0.05f)
        return "Your mix: about the same as the reference (both at the same loudness)";

    return "Your mix: " + fmt::number (std::abs (diff), 1) + " dB " + (diff > 0.0f ? "more" : "less")
           + " than the reference (both at the same loudness)";
}

void TonalBalancePanel::setData (const ViewData& data, bool snap)
{
    auto& d = *impl;
    bool changed = false;

    const bool newRefValid = data.reference.valid;
    const bool newMixValid = data.mix.valid;
    const float offset = data.comparison.valid() ? data.comparison.normalisationOffsetDb : 0.0f;

    changed |= newRefValid != d.refValid || newMixValid != d.mixValid;
    d.refValid = newRefValid;
    d.mixValid = newMixValid;

    const bool newHasReference = referenceExpected (data);
    changed |= newHasReference != d.hasReference;
    d.hasReference = newHasReference;

    for (int i = 0; i < rm::kCurvePoints; ++i)
    {
        const float tilt = displayTiltDb (rm::curveFrequency (i));
        const float r = data.reference.curveDb[(size_t) i];
        const float m = data.mix.curveDb[(size_t) i];
        changed |= d.ref[(size_t) i].step (r + tilt, newRefValid && rm::hasValue (r), snap);
        changed |= d.mix[(size_t) i].step (m + offset + tilt, newMixValid && rm::hasValue (m), snap);
    }

    for (int b = 0; b < rm::kNumBands; ++b)
    {
        const auto& band = data.comparison.bands[(size_t) b];
        const bool valid = data.comparison.valid() && band.valid;
        changed |= d.bands[(size_t) b].step (band.levelDiffDb, valid, snap);
        changed |= d.bandSeverity[(size_t) b] != band.severity;
        d.bandSeverity[(size_t) b] = band.severity;
    }

    bool ok = false;
    float c = Impl::curveCentre (d.ref, ok);

    if (! ok)
        c = Impl::curveCentre (d.mix, ok);

    if (ok)
        changed |= d.centre.step (c, true, snap);

    if (changed)
        repaint();
}

void TonalBalancePanel::resized()
{
    impl->layout();
}

void TonalBalancePanel::paint (juce::Graphics& g)
{
    drawPanel (g, getLocalBounds().toFloat());
    impl->paintTitle (g);
    impl->paintPlot (g);
    impl->paintBands (g);
}

void TonalBalancePanel::mouseMove (const juce::MouseEvent& e)
{
    const int band = impl->bandAt (e.position);

    if (band != impl->hoverBand)
    {
        impl->hoverBand = band;
        repaint();
    }
}

void TonalBalancePanel::mouseExit (const juce::MouseEvent&)
{
    if (impl->hoverBand >= 0)
    {
        impl->hoverBand = -1;
        repaint();
    }
}

juce::String TonalBalancePanel::getTooltip()
{
    auto& d = *impl;
    const auto pos = getMouseXYRelative().toFloat();
    const int band = d.bandAt (pos);

    if (band >= 0)
    {
        juce::String tip (bandExplanation (band));
        const auto& b = d.bands[(size_t) band];

        if (b.valid)
        {
            const float diff = b.target;
            const auto size = std::abs (diff) < rm::kBandMinorDb ? juce::String ("close to the reference")
                            : (std::abs (diff) < rm::kBandMajorDb ? juce::String ("a noticeable difference")
                                                                  : juce::String ("a big difference"));
            tip << "\n\n" << bandDifferenceText (diff) << " - " << size << ".";
        }

        return tip;
    }

    if (d.titleRow.contains (pos) || d.plot.contains (pos))
        return "Tonal balance: the average spectrum of your mix (mint line) and of the reference (purple area), both at "
               "the same loudness. The picture is tilted by +4.5 dB per octave so a typical well-balanced song looks "
               "roughly flat. Where the line is above the area, your mix has more of those frequencies.";

    return {};
}

} // namespace rm::ui
