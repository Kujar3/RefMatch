#include "ui/StereoPanel.h"

namespace rm::ui
{
namespace
{
constexpr float kEase = 0.3f;
constexpr float kWidthScalePct = 100.0f;   // top of the width chart ("wide")

struct Eased
{
    float shown = 0.0f, target = 0.0f;
    bool valid = false;

    bool step (float newTarget, bool newValid, bool snap)
    {
        target = newTarget;

        if (! newValid)
        {
            const bool changed = valid;
            valid = false;
            return changed;
        }

        if (! valid || snap || std::abs (newTarget - shown) < 0.002f)
        {
            const bool changed = ! valid || shown != newTarget;
            shown = newTarget;
            valid = true;
            return changed;
        }

        shown += (newTarget - shown) * kEase;
        return true;
    }
};

struct Badge
{
    juce::String text;
    rm::Severity severity = rm::Severity::Info;
    bool known = false;
};

Badge bassBadge (const rm::TrackProfile& mix)
{
    if (! mix.valid)
        return { "Bass mono: --", rm::Severity::Info, false };

    if (mix.lowEndCorrelation >= 0.8f)
        return { "Bass mono: OK", rm::Severity::Good, true };

    if (mix.lowEndCorrelation >= 0.5f)
        return { "Bass a bit wide", rm::Severity::Minor, true };

    return { "Bass too wide", rm::Severity::Major, true };
}
} // namespace

//==============================================================================
struct StereoPanel::Impl
{
    explicit Impl (StereoPanel& o) : owner (o) {}

    enum class Area { none, chart, correlation, badge };

    void layout()
    {
        auto inner = owner.getLocalBounds().toFloat().reduced (14.0f, 10.0f);
        titleRow = inner.removeFromTop (20.0f);
        inner.removeFromTop (6.0f);

        const bool roomy = inner.getHeight() >= 150.0f;
        correlationArea = inner.removeFromBottom (roomy ? 44.0f : 32.0f);
        showScaleLabels = roomy;
        inner.removeFromBottom (6.0f);
        showSummary = inner.getHeight() >= 120.0f;
        summaryRow = showSummary ? inner.removeFromBottom (16.0f) : juce::Rectangle<float>();

        if (showSummary)
            inner.removeFromBottom (4.0f);

        chart = inner;
        chartNames = chart.removeFromBottom (14.0f);
        chart.removeFromBottom (3.0f);
        chartGutter = chart.removeFromLeft (38.0f);
        chartNames.removeFromLeft (38.0f);

        const auto f = font (11.0f, 600);
        badgeArea = titleRow.withTrimmedLeft (titleRow.getWidth() - (textWidth (f, badge.text) + 30.0f)).withSizeKeepingCentre (
            textWidth (f, badge.text) + 30.0f, 20.0f);
    }

    juce::Rectangle<float> groupBounds (int band) const
    {
        const float gw = chart.getWidth() / (float) rm::kNumBands;
        return { chart.getX() + gw * (float) band, chart.getY(), gw, chart.getHeight() };
    }

    Area areaAt (juce::Point<float> p, int& band) const
    {
        band = -1;

        if (badgeArea.contains (p))
            return Area::badge;

        if (correlationArea.contains (p))
            return Area::correlation;

        if (chart.withBottom (chartNames.getBottom()).withLeft (chartGutter.getX()).contains (p))
        {
            for (int b = 0; b < rm::kNumBands; ++b)
                if (p.x >= groupBounds (b).getX() && p.x < groupBounds (b).getRight())
                    band = b;

            return Area::chart;
        }

        return Area::none;
    }

    void paintTitle (juce::Graphics& g) const
    {
        drawSectionTitle (g, "Stereo image", titleRow.toNearestInt());

        const auto colour = badge.known ? severityColour (badge.severity) : Colours::textFaint;
        const auto pill = badgeArea;
        g.setColour (colour.withAlpha (badge.known ? 0.16f : 0.08f));
        g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
        g.setColour (colour.withAlpha (badge.known ? 0.65f : 0.4f));
        g.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);

        // Status glyph: tick (good), "!" (problem) or dot (unknown)
        const auto c = juce::Point<float> (pill.getX() + 12.0f, pill.getCentreY());
        g.setColour (colour.brighter (0.2f));

        if (badge.known && badge.severity == rm::Severity::Good)
        {
            juce::Path tick;
            tick.startNewSubPath (c.x - 3.5f, c.y + 0.2f);
            tick.lineTo (c.x - 1.0f, c.y + 2.8f);
            tick.lineTo (c.x + 3.8f, c.y - 2.8f);
            g.strokePath (tick, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else if (badge.known)
        {
            g.setFont (font (11.5f, 700));
            g.drawText ("!", juce::Rectangle<float> (10.0f, 14.0f).withCentre (c), juce::Justification::centred, false);
        }
        else
        {
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (c));
        }

        g.setFont (font (11.0f, 600));
        g.setColour (badge.known ? colour.brighter (0.25f) : Colours::textDim);
        g.drawText (badge.text, pill.withTrimmedLeft (21.0f), juce::Justification::centredLeft, false);
    }

    void paintChart (juce::Graphics& g) const
    {
        const float yZero = chart.getBottom();
        const float pxPerPct = chart.getHeight() / kWidthScalePct;

        // Guides: "wide" (100 %), half, "mono" (0 %)
        g.setFont (font (10.0f));

        for (const float pct : { 0.0f, 50.0f, 100.0f })
        {
            const float y = yZero - pct * pxPerPct;
            g.setColour (Colours::outline.withAlpha (pct == 0.0f ? 0.9f : 0.35f));
            g.fillRect (juce::Rectangle<float> (chart.getX(), y - 0.5f, chart.getWidth(), 1.0f));
        }

        g.setColour (Colours::textSubtle);
        g.drawText ("wide", juce::Rectangle<float> (chartGutter.getX(), chart.getY() - 6.0f, 34.0f, 12.0f),
                    juce::Justification::centredLeft, false);
        g.drawText ("mono", juce::Rectangle<float> (chartGutter.getX(), yZero - 12.0f, 34.0f, 12.0f),
                    juce::Justification::centredLeft, false);

        for (int b = 0; b < rm::kNumBands; ++b)
        {
            const auto group = groupBounds (b);

            if (b == hoverBand)
            {
                g.setColour (juce::Colours::white.withAlpha (0.035f));
                g.fillRoundedRectangle (group.withBottom (chartNames.getBottom()).reduced (2.0f, -2.0f), 4.0f);
            }

            const float barW = juce::jlimit (5.0f, 13.0f, group.getWidth() * 0.22f);
            const float gap = 3.0f;
            const float x0 = group.getCentreX() - barW - gap * 0.5f;

            auto drawBar = [&] (const Eased& v, juce::Colour colour, float x)
            {
                if (! v.valid)
                {
                    g.setColour (Colours::textFaint.withAlpha (0.4f));
                    g.fillRoundedRectangle (juce::Rectangle<float> (x, yZero - 2.0f, barW, 2.0f), 1.0f);
                    return;
                }

                const float h = juce::jmax (2.0f, juce::jmin (v.shown, kWidthScalePct) * pxPerPct);
                const auto bar = juce::Rectangle<float> (x, yZero - h, barW, h);
                g.setGradientFill (juce::ColourGradient::vertical (colour, bar.getY(), colour.withAlpha (0.55f), bar.getBottom()));
                g.fillRoundedRectangle (bar, juce::jmin (3.0f, barW * 0.4f));

                if (v.shown > kWidthScalePct)
                {
                    juce::Path arrow;
                    arrow.addTriangle (x - 1.0f, bar.getY() - 1.0f, x + barW + 1.0f, bar.getY() - 1.0f, x + barW * 0.5f, bar.getY() - 5.0f);
                    g.setColour (colour);
                    g.fillPath (arrow);
                }
            };

            drawBar (mixWidth[(size_t) b], Colours::mix, x0);
            drawBar (refWidth[(size_t) b], Colours::reference, x0 + barW + gap);

            g.setFont (font (10.5f, 600));
            g.setColour (Colours::textDim);
            drawFittedLine (g, rm::bandInfo (b).shortName, group.withY (chartNames.getY()).withHeight (chartNames.getHeight()),
                            juce::Justification::centred, 0.7f);
        }
    }

    void paintSummary (juce::Graphics& g) const
    {
        if (! showSummary || ! mixOverall.valid || ! refOverall.valid)
            return;

        const float d = mixOverall.target - refOverall.target;
        juce::String text;
        auto severity = rm::Severity::Good;

        if (std::abs (d) < 10.0f)
        {
            text = "Overall about as wide as the reference";
        }
        else
        {
            text = juce::String ("Overall ") + (d < 0.0f ? "narrower" : "wider") + " than the reference ("
                   + juce::String (juce::roundToInt (mixOverall.target)) + "% vs " + juce::String (juce::roundToInt (refOverall.target)) + "%)";
            severity = std::abs (d) >= 25.0f ? rm::Severity::Minor : rm::Severity::Info;
        }

        auto r = summaryRow;
        g.setColour (severityColour (severity));
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ r.getX() + 3.0f, r.getCentreY() }));
        r.removeFromLeft (12.0f);
        g.setFont (font (11.5f));
        g.setColour (Colours::textDim);
        drawFittedLine (g, text, r, juce::Justification::centredLeft, 0.85f);
    }

    void paintCorrelation (juce::Graphics& g) const
    {
        auto r = correlationArea;
        auto labelRow = r.removeFromTop (15.0f);

        g.setFont (font (11.5f, 600));
        g.setColour (Colours::textDim.brighter (0.1f));
        g.drawText ("Phase correlation", labelRow, juce::Justification::centredLeft, false);

        // Values, right aligned: "Mix +0.62   Ref +0.71"
        {
            const auto vf = font (11.5f, 700);
            const auto refText = "Ref " + (refCorr.valid ? fmt::signedNumber (refCorr.target, 2) : juce::String ("--"));
            const auto mixText = "Mix " + (mixCorr.valid ? fmt::signedNumber (mixCorr.target, 2) : juce::String ("--"));
            float x = labelRow.getRight() - textWidth (vf, refText);
            g.setFont (vf);
            g.setColour (refCorr.valid ? Colours::reference.brighter (0.15f) : Colours::textFaint);
            g.drawText (refText, labelRow.withLeft (x), juce::Justification::centredLeft, false);
            x -= textWidth (vf, mixText) + 12.0f;
            g.setColour (mixCorr.valid ? Colours::mix.brighter (0.1f) : Colours::textFaint);
            g.drawText (mixText, labelRow.withLeft (x), juce::Justification::centredLeft, false);
        }

        r.removeFromTop (4.0f);
        const auto track = juce::Rectangle<float> (r.getX() + 6.0f, r.getY() + 5.0f, r.getWidth() - 12.0f, 6.0f);
        juce::ColourGradient grad (Colours::major.withAlpha (0.55f), track.getX(), 0.0f,
                                   Colours::good.withAlpha (0.65f), track.getRight(), 0.0f, false);
        grad.addColour (0.5, Colours::minor.withAlpha (0.45f));
        g.setGradientFill (grad);
        g.fillRoundedRectangle (track, 3.0f);

        auto xFor = [&track] (float c) { return track.getX() + (juce::jlimit (-1.0f, 1.0f, c) + 1.0f) * 0.5f * track.getWidth(); };

        g.setColour (Colours::panel);
        g.fillRect (juce::Rectangle<float> (xFor (0.0f) - 0.5f, track.getY(), 1.0f, track.getHeight()));

        if (showScaleLabels)
        {
            g.setFont (font (10.0f));
            g.setColour (Colours::textSubtle);
            const float ly = track.getBottom() + 6.0f;
            g.drawText ("-1  out of phase", juce::Rectangle<float> (track.getX() - 2.0f, ly, 120.0f, 12.0f), juce::Justification::centredLeft, false);
            g.drawText ("0", juce::Rectangle<float> (xFor (0.0f) - 10.0f, ly, 20.0f, 12.0f), juce::Justification::centred, false);
            g.drawText ("mono  +1", juce::Rectangle<float> (track.getRight() - 118.0f, ly, 120.0f, 12.0f), juce::Justification::centredRight, false);
        }

        // Markers: mix above the track (pointing down), reference below (pointing up)
        if (mixCorr.valid)
        {
            const float x = xFor (mixCorr.shown);
            juce::Path p;
            p.addTriangle (x - 5.0f, track.getY() - 5.5f, x + 5.0f, track.getY() - 5.5f, x, track.getY() + 2.0f);
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.strokePath (p, juce::PathStrokeType (2.0f));
            g.setColour (Colours::mix);
            g.fillPath (p);
        }

        if (refCorr.valid)
        {
            const float x = xFor (refCorr.shown);
            juce::Path p;
            p.addTriangle (x - 5.0f, track.getBottom() + 5.5f, x + 5.0f, track.getBottom() + 5.5f, x, track.getBottom() - 2.0f);
            g.setColour (juce::Colours::black.withAlpha (0.4f));
            g.strokePath (p, juce::PathStrokeType (2.0f));
            g.setColour (Colours::reference);
            g.fillPath (p);
        }
    }

    StereoPanel& owner;
    juce::Rectangle<float> titleRow, chart, chartGutter, chartNames, summaryRow, correlationArea, badgeArea;
    bool showSummary = false, showScaleLabels = true;

    std::array<Eased, rm::kNumBands> mixWidth {}, refWidth {};
    Eased mixCorr, refCorr, mixOverall, refOverall;
    float mixLowEnd = 1.0f;
    Badge badge { "Bass mono: --" };
    int hoverBand = -1;
};

//==============================================================================
StereoPanel::StereoPanel() : impl (std::make_unique<Impl> (*this))
{
    setTitle ("Stereo image");
}

StereoPanel::~StereoPanel() = default;

void StereoPanel::setData (const ViewData& data, bool snap)
{
    auto& d = *impl;
    bool changed = false;
    const bool mixValid = data.mix.valid, refValid = data.reference.valid;

    for (int b = 0; b < rm::kNumBands; ++b)
    {
        changed |= d.mixWidth[(size_t) b].step (data.mix.bands[(size_t) b].widthPct, mixValid, snap);
        changed |= d.refWidth[(size_t) b].step (data.reference.bands[(size_t) b].widthPct, refValid, snap);
    }

    changed |= d.mixCorr.step (data.mix.correlation, mixValid, snap);
    changed |= d.refCorr.step (data.reference.correlation, refValid, snap);
    changed |= d.mixOverall.step (data.mix.widthPct, mixValid, snap);
    changed |= d.refOverall.step (data.reference.widthPct, refValid, snap);
    d.mixLowEnd = data.mix.lowEndCorrelation;

    const auto newBadge = bassBadge (data.mix);

    if (newBadge.text != d.badge.text)
    {
        d.badge = newBadge;
        d.layout();
        changed = true;
    }

    if (changed)
        repaint();
}

void StereoPanel::resized()
{
    impl->layout();
}

void StereoPanel::paint (juce::Graphics& g)
{
    drawPanel (g, getLocalBounds().toFloat());
    impl->paintTitle (g);
    impl->paintChart (g);
    impl->paintSummary (g);
    impl->paintCorrelation (g);
}

void StereoPanel::mouseMove (const juce::MouseEvent& e)
{
    int band = -1;
    impl->areaAt (e.position, band);

    if (band != impl->hoverBand)
    {
        impl->hoverBand = band;
        repaint();
    }
}

void StereoPanel::mouseExit (const juce::MouseEvent&)
{
    if (impl->hoverBand >= 0)
    {
        impl->hoverBand = -1;
        repaint();
    }
}

juce::String StereoPanel::getTooltip()
{
    auto& d = *impl;
    int band = -1;

    switch (d.areaAt (getMouseXYRelative().toFloat(), band))
    {
        case Impl::Area::badge:
        {
            juce::String tip ("Bass mono check: below 120 Hz almost every commercial track is mono, so the bass keeps its "
                              "power on mono systems, phones and in clubs. 'Too wide' means your bass would lose power "
                              "when summed to mono - remove stereo wideners from the bass, 808 and kick, or turn the "
                              "stereo separation knob on their own mixer track (not the Master) fully right, which makes "
                              "it mono.");

            if (d.badge.known)
                tip << "\n\nYour mix's low-end correlation: " << fmt::signedNumber (d.mixLowEnd, 2) << " (+1 = perfectly mono).";

            return tip;
        }

        case Impl::Area::correlation:
            return "Phase correlation: +1 = mono, 0 = very wide, below 0 = left and right cancel each other when played "
                   "in mono (phone speakers, clubs). Most masters sit between +0.2 and +0.8. The mint marker is your "
                   "mix, the purple marker the reference.";

        case Impl::Area::chart:
        {
            juce::String tip ("Stereo width per band (mint = your mix, purple = reference): how much of each range is "
                              "spread left / right instead of in the middle. 0% = mono, 100% = as wide as two unrelated "
                              "channels. Sub and bass should be close to mono; highs are usually the widest.");

            if (band >= 0)
            {
                const auto& m = d.mixWidth[(size_t) band];
                const auto& r = d.refWidth[(size_t) band];
                tip << "\n\n" << rm::bandInfo (band).name << ": mix "
                    << (m.valid ? juce::String (juce::roundToInt (m.target)) + "%" : juce::String ("--")) << ", reference "
                    << (r.valid ? juce::String (juce::roundToInt (r.target)) + "%" : juce::String ("--")) << ".";
            }

            return tip;
        }

        case Impl::Area::none:
            break;
    }

    return {};
}

} // namespace rm::ui
