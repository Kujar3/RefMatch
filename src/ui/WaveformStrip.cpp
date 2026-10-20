#include "ui/WaveformStrip.h"

#include <optional>
#include <tuple>

namespace rm::ui
{
namespace
{
using RefState = RefMatchProcessor::ReferenceState;

constexpr float kEdgeGrabPx = 6.0f;       // distance from a loop edge that grabs it
constexpr float kDragThresholdPx = 4.0f;  // a press that moves further than this is a drag (loop), else a click (seek)
constexpr float kLoudnessTopLufs = -2.0f, kLoudnessBottomLufs = -40.0f;   // loudness range when too few values
constexpr float kSilhouetteRangeLu = 14.0f;   // loudness shown from the centre line (bottom) to the full height (top)
constexpr float kSilhouetteHeadroomLu = 1.5f; // top of the range above the track's 95th-percentile short-term loudness
constexpr double kShortTermWindow = 3.0;      // seconds per short-term value (drawn at the window's centre)
constexpr juce::uint32 kCommittedRegionHoldMs = 1000;

const juce::String kQuoteOpen = juce::String::fromUTF8 ("\xe2\x80\x9c"), kQuoteClose = juce::String::fromUTF8 ("\xe2\x80\x9d");
const juce::String kDash = juce::String::fromUTF8 (" \xe2\x80\x93 ");   // " – "

void drawDashedRoundedRect (juce::Graphics& g, juce::Rectangle<float> r, float radius, float thickness)
{
    juce::Path outline, dashed;
    outline.addRoundedRectangle (r, radius);
    const float dashes[] = { 6.0f, 5.0f };
    juce::PathStrokeType (thickness).createDashedStroke (dashed, outline, dashes, 2);
    g.fillPath (dashed);
}
} // namespace

//==============================================================================
struct WaveformStrip::Impl
{
    explicit Impl (WaveformStrip& o, EditorActions& a) : owner (o), actions (a) {}

    enum class Drag { none, pending, create, startEdge, endEdge };

    // ---- geometry -------------------------------------------------------------------------------------------------
    void layout()
    {
        auto inner = owner.getLocalBounds().toFloat().reduced (12.0f, 8.0f);
        const float gutterW = owner.getWidth() < 1000 ? 112.0f : 128.0f;
        gutter = inner.removeFromLeft (gutterW);
        inner.removeFromLeft (10.0f);
        ruler = inner.removeFromBottom (15.0f);
        inner.removeFromBottom (1.0f);
        wave = inner;
        dropZone = owner.getLocalBounds().toFloat().reduced (10.0f, 8.0f);
        cachedFor = {};
    }

    bool hasTrack() const noexcept { return track != nullptr && length() > 0.0; }

    double length() const noexcept
    {
        if (info.lengthSeconds > 0.0)
            return info.lengthSeconds;

        return track != nullptr ? track->lengthSeconds() : 0.0;
    }

    float xForTime (double t) const noexcept
    {
        const double len = length();
        return len > 0.0 ? wave.getX() + (float) (juce::jlimit (0.0, len, t) / len) * wave.getWidth() : wave.getX();
    }

    double timeForX (float x) const noexcept
    {
        return wave.getWidth() > 0.0f ? juce::jlimit (0.0, length(), (double) ((x - wave.getX()) / wave.getWidth()) * length()) : 0.0;
    }

    // The loop region shown: the one being dragged / just committed, else the processor's.
    bool regionShown (double& start, double& end) const noexcept
    {
        if (localRegion.has_value())
        {
            start = localRegion->first;
            end = localRegion->second;
            return true;
        }

        start = info.loopStart;
        end = info.loopEnd;
        return info.hasLoop;
    }

    // ---- cached paths ---------------------------------------------------------------------------------------------
    void updateCache()
    {
        const auto key = std::make_tuple (track.get(), wave, length());

        if (key == cachedFor)
            return;

        cachedFor = key;
        wavePath.clear();
        loudnessPath.clear();
        loudnessMirrorPath.clear();
        silhouettePath.clear();
        hasLoudness = false;

        if (! hasTrack() || wave.isEmpty())
            return;

        // Waveform: min / max of the overview per pixel column (filled polygon, top edge then bottom edge back).
        const auto& ov = track->overview;
        const int columns = (int) juce::jmin (ov.minimum.size(), ov.maximum.size());
        const int pixels = juce::jmax (1, (int) wave.getWidth());
        const float centreY = wave.getCentreY(), half = wave.getHeight() * 0.5f - 1.0f;

        if (columns > 0)
        {
            std::vector<float> lo ((size_t) pixels), hi ((size_t) pixels);

            for (int px = 0; px < pixels; ++px)
            {
                const int c0 = juce::jlimit (0, columns - 1, (int) ((double) px / pixels * columns));
                const int c1 = juce::jlimit (c0 + 1, columns, (int) ((double) (px + 1) / pixels * columns));
                float mn = 0.0f, mx = 0.0f;

                for (int c = c0; c < c1; ++c)
                {
                    mn = juce::jmin (mn, ov.minimum[(size_t) c]);
                    mx = juce::jmax (mx, ov.maximum[(size_t) c]);
                }

                lo[(size_t) px] = juce::jlimit (-1.0f, 1.0f, mn);
                hi[(size_t) px] = juce::jlimit (-1.0f, 1.0f, mx);
            }

            const float minHalfPx = 0.6f;   // silence still shows a hairline
            wavePath.startNewSubPath (wave.getX(), centreY - juce::jmax (minHalfPx, hi[0] * half));

            for (int px = 0; px < pixels; ++px)
                wavePath.lineTo (wave.getX() + (float) px + 0.5f, centreY - juce::jmax (minHalfPx, hi[(size_t) px] * half));

            for (int px = pixels - 1; px >= 0; --px)
                wavePath.lineTo (wave.getX() + (float) px + 0.5f, centreY + juce::jmax (minHalfPx, -lo[(size_t) px] * half));

            wavePath.closeSubPath();
        }

        // Short-term loudness as a mirrored silhouette (the song's structure: intro, breakdowns, drops), with the
        // white loudness line on its top edge. The range is per track: its loud parts (95th percentile + headroom)
        // nearly fill the height, kSilhouetteRangeLu below that is the centre line.
        const auto& curve = track->shortTermCurve;
        const double len = length();
        float top = kLoudnessTopLufs, bottom = kLoudnessBottomLufs;
        {
            std::vector<float> values;
            values.reserve (curve.size());

            for (const float v : curve)
                if (rm::hasValue (v) && v > -70.0f)
                    values.push_back (v);

            if (values.size() >= 10)
            {
                const auto k = (size_t) std::floor (0.95 * (double) (values.size() - 1));
                std::nth_element (values.begin(), values.begin() + (std::ptrdiff_t) k, values.end());
                top = juce::jmin (0.0f, values[k] + kSilhouetteHeadroomLu);
                bottom = top - kSilhouetteRangeLu;
            }
        }

        // Loudest value per pixel column (element i is the 3 s window ending at (i + 1) * 0.1 s: drawn at its centre).
        // The first and last 1.5 s have no window centred on them: they use the first / last measured window (which
        // covers them), so a track does not look silent at its ends.
        const int numValues = (int) curve.size();
        int firstKnown = 0;

        while (firstKnown < numValues && ! rm::hasValue (curve[(size_t) firstKnown]))
            ++firstKnown;

        if (firstKnown >= numValues)
            return;   // no loudness values (track shorter than 3 s): the waveform alone

        const int lastIndex = numValues - 1;
        auto indexAt = [] (double t) { return (int) std::ceil ((t + kShortTermWindow * 0.5) * 10.0 - 1.0e-6) - 1; };
        std::vector<float> halfPx ((size_t) pixels, -1.0f);   // half-height in px, -1 = unknown
        const float maxHalf = wave.getHeight() * 0.5f - 1.0f;

        for (int px = 0; px < pixels; ++px)
        {
            const double t0 = len * (double) px / (double) pixels, t1 = len * (double) (px + 1) / (double) pixels;
            int i0 = juce::jmax (0, indexAt (t0)), i1 = juce::jmin (numValues, indexAt (t1));

            if (i1 <= i0)   // fewer values than pixels: the nearest one
            {
                i0 = juce::jlimit (0, numValues, (int) std::lround (((t0 + t1) * 0.5 + kShortTermWindow * 0.5) * 10.0) - 1);
                i1 = juce::jmin (numValues, i0 + 1);
            }

            if (i1 <= firstKnown)        // before the first centred window
            {
                i0 = firstKnown;
                i1 = firstKnown + 1;
            }
            else if (i0 > lastIndex)     // after the last one
            {
                i0 = lastIndex;
                i1 = numValues;
            }

            float loudest = rm::kNoValue;

            for (int i = i0; i < i1; ++i)
                if (rm::hasValue (curve[(size_t) i]))
                    loudest = rm::hasValue (loudest) ? juce::jmax (loudest, curve[(size_t) i]) : curve[(size_t) i];

            if (rm::hasValue (loudest))
            {
                const float norm = juce::jlimit (0.0f, 1.0f, (loudest - bottom) / (top - bottom));
                halfPx[(size_t) px] = juce::jmax (1.0f, norm * maxHalf);
                hasLoudness = true;
            }
        }

        if (! hasLoudness)
            return;

        const float hairline = 0.6f;
        auto halfAt = [&] (int px) { return halfPx[(size_t) px] < 0.0f ? hairline : halfPx[(size_t) px]; };
        auto xAt = [this] (int px) { return wave.getX() + (float) px + 0.5f; };

        silhouettePath.startNewSubPath (wave.getX(), centreY - halfAt (0));

        for (int px = 0; px < pixels; ++px)
            silhouettePath.lineTo (xAt (px), centreY - halfAt (px));

        silhouettePath.lineTo (wave.getRight(), centreY - halfAt (pixels - 1));
        silhouettePath.lineTo (wave.getRight(), centreY + halfAt (pixels - 1));

        for (int px = pixels - 1; px >= 0; --px)
            silhouettePath.lineTo (xAt (px), centreY + halfAt (px));

        silhouettePath.lineTo (wave.getX(), centreY + halfAt (0));
        silhouettePath.closeSubPath();

        bool penDown = false;

        for (int px = 0; px < pixels; ++px)
        {
            if (halfPx[(size_t) px] < 0.0f)
            {
                penDown = false;
                continue;
            }

            const juce::Point<float> p (xAt (px), centreY - halfPx[(size_t) px]);
            const juce::Point<float> mirrored (p.x, centreY + halfPx[(size_t) px]);

            if (penDown)
            {
                loudnessPath.lineTo (p);
                loudnessMirrorPath.lineTo (mirrored);
            }
            else
            {
                loudnessPath.startNewSubPath (p);
                loudnessMirrorPath.startNewSubPath (mirrored);
            }

            penDown = true;
        }
    }

    // ---- painting -------------------------------------------------------------------------------------------------
    void paintDropZone (juce::Graphics& g) const
    {
        const bool error = info.state == RefState::Error;
        const bool loading = info.state == RefState::Loading;
        const bool hot = (hover || dropHighlight) && ! loading;
        const auto tint = error ? Colours::major : Colours::reference;

        g.setColour (tint.withAlpha (hot ? 0.10f : 0.045f));
        g.fillRoundedRectangle (dropZone, 8.0f);
        g.setColour (tint.withAlpha (hot ? 0.75f : 0.38f));
        drawDashedRoundedRect (g, dropZone.reduced (0.75f), 8.0f, 1.5f);

        juce::String line1, line2, line2Accent;

        if (loading)
        {
            line1 = "Loading " + kQuoteOpen + info.displayName + kQuoteClose + "...";
            line2 = "Decoding and analysing the track, this takes a few seconds";
        }
        else if (error)
        {
            line1 = "Couldn't load " + kQuoteOpen + info.displayName + kQuoteClose;
            line2 = info.error + kDash + "drop another file or ";
            line2Accent = "click to browse";
        }
        else
        {
            line1 = "Drop a reference track here";
            line2 = kSupportedFormatsText + kDash;
            line2Accent = "or click to browse";
        }

        const bool roomy = dropZone.getHeight() >= 64.0f;
        const auto f1 = font (roomy ? 16.0f : 14.0f, 600), f2 = font (roomy ? 12.5f : 11.5f);
        const float iconD = roomy ? 38.0f : 30.0f;
        const float textW = juce::jmin (dropZone.getWidth() - iconD - 60.0f,
                                        juce::jmax (textWidth (f1, line1), textWidth (f2, line2 + line2Accent)) + 4.0f);
        const float blockW = iconD + 14.0f + textW;
        const float x0 = std::round (dropZone.getCentreX() - blockW * 0.5f);
        const float cy = dropZone.getCentreY();

        // Icon disc
        const auto icon = juce::Rectangle<float> (x0, cy - iconD * 0.5f, iconD, iconD);
        g.setColour (tint.withAlpha (0.16f));
        g.fillEllipse (icon);
        g.setColour (tint.withAlpha (0.55f));
        g.drawEllipse (icon.reduced (0.6f), 1.2f);
        const auto c = icon.getCentre();
        g.setColour (tint.brighter (0.25f));

        if (loading)
        {
            const float angle = animationPhase (1000) * juce::MathConstants<float>::twoPi;
            juce::Path arc;
            arc.addCentredArc (c.x, c.y, iconD * 0.26f, iconD * 0.26f, angle, 0.0f, 4.2f, true);
            g.strokePath (arc, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else if (error)
        {
            g.setFont (font (iconD * 0.5f, 700));
            g.drawText ("!", icon.translated (0.0f, 0.5f), juce::Justification::centred, false);
        }
        else
        {
            // Arrow down into a tray
            const float s = iconD * 0.2f;
            juce::Path p;
            p.startNewSubPath (c.x, c.y - s * 1.3f);
            p.lineTo (c.x, c.y + s * 0.5f);
            p.startNewSubPath (c.x - s * 0.8f, c.y - s * 0.3f);
            p.lineTo (c.x, c.y + s * 0.5f);
            p.lineTo (c.x + s * 0.8f, c.y - s * 0.3f);
            p.startNewSubPath (c.x - s * 1.4f, c.y + s * 0.6f);
            p.lineTo (c.x - s * 1.4f, c.y + s * 1.3f);
            p.lineTo (c.x + s * 1.4f, c.y + s * 1.3f);
            p.lineTo (c.x + s * 1.4f, c.y + s * 0.6f);
            g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Text
        const float tx = icon.getRight() + 14.0f;
        const float lineH1 = roomy ? 21.0f : 18.0f, lineH2 = roomy ? 17.0f : 15.0f;
        const float ty = cy - (lineH1 + lineH2 + (loading ? 8.0f : 0.0f)) * 0.5f;

        g.setFont (f1);
        g.setColour (Colours::text);
        drawFittedLine (g, line1, { tx, ty, textW, lineH1 }, juce::Justification::centredLeft, 0.85f);

        g.setFont (f2);
        g.setColour (error ? Colours::major.brighter (0.15f) : Colours::textDim);
        const float w2 = juce::jmin (textW, textWidth (f2, line2) + 1.0f);
        drawFittedLine (g, line2, { tx, ty + lineH1, w2, lineH2 }, juce::Justification::centredLeft, 0.85f);

        if (line2Accent.isNotEmpty() && w2 < textW - 20.0f)
        {
            g.setColour (Colours::reference.brighter (0.2f));
            drawFittedLine (g, line2Accent, { tx + w2, ty + lineH1, textW - w2, lineH2 }, juce::Justification::centredLeft, 0.85f);
        }

        if (loading)
        {
            // Indeterminate progress: a short bar sliding along a track.
            const auto bar = juce::Rectangle<float> (tx, ty + lineH1 + lineH2 + 4.0f, juce::jmin (220.0f, textW), 3.0f);
            g.setColour (Colours::outline);
            g.fillRoundedRectangle (bar, 1.5f);
            const float phase = animationPhase (1400);
            const float segW = bar.getWidth() * 0.3f;
            const float sx = bar.getX() + (bar.getWidth() + segW) * phase - segW;
            g.setColour (Colours::reference);
            g.fillRoundedRectangle (juce::Rectangle<float> (sx, bar.getY(), segW, bar.getHeight()).getIntersection (bar), 1.5f);
        }
    }

    void paintGutter (juce::Graphics& g) const
    {
        auto r = gutter;
        drawSectionTitle (g, "Reference", r.removeFromTop (16.0f).toNearestInt());
        r.removeFromTop (2.0f);

        double start = 0.0, end = 0.0;
        const bool loop = regionShown (start, end);

        g.setFont (font (12.5f, 600));
        g.setColour (loop ? Colours::reference.brighter (0.25f) : Colours::text);
        drawFittedLine (g, loop ? "Loop " + fmt::duration (end - start) : juce::String ("Whole track"),
                        r.removeFromTop (17.0f), juce::Justification::centredLeft, 0.85f);

        if (r.getHeight() >= 15.0f)
        {
            g.setFont (font (11.5f));
            g.setColour (Colours::textDim);
            const juce::String pos = fmt::time (info.state == RefState::Ready || track != nullptr ? playhead : 0.0)
                                     + " / " + fmt::time (length());
            drawFittedLine (g, pos, r.removeFromTop (15.0f), juce::Justification::centredLeft, 0.85f);
        }

        if (r.getHeight() >= 14.0f)
        {
            g.setFont (font (10.5f));
            g.setColour (Colours::textSubtle);
            drawFittedLine (g, loop ? "Double-click clears" : "Drag to loop a part", r.removeFromTop (14.0f),
                            juce::Justification::centredLeft, 0.85f);
        }
    }

    void paintWave (juce::Graphics& g) const
    {
        // Background well
        g.setColour (Colours::background.withAlpha (0.55f));
        g.fillRoundedRectangle (wave.expanded (0.0f, 1.0f), 4.0f);

        // Centre line
        g.setColour (Colours::outline.withAlpha (0.5f));
        g.fillRect (wave.getX(), wave.getCentreY() - 0.5f, wave.getWidth(), 1.0f);

        // Waveform peaks: faint and flat (a loud master peaks near full scale everywhere, so they show no structure)
        g.setColour (Colours::reference.withAlpha (hasLoudness ? 0.13f : 0.55f));
        g.fillPath (wavePath);

        // Loudness silhouette: where the song gets louder or quieter
        if (hasLoudness)
        {
            juce::ColourGradient grad (Colours::reference.withAlpha (0.42f), 0.0f, wave.getY(),
                                       Colours::reference.withAlpha (0.42f), 0.0f, wave.getBottom(), false);
            grad.addColour (0.5, Colours::reference.withAlpha (0.56f));
            g.setGradientFill (grad);
            g.fillPath (silhouettePath);
        }

        // Loop region: dim outside, tint inside, edges with grab tabs.
        double start = 0.0, end = 0.0;

        if (regionShown (start, end))
        {
            const float x0 = xForTime (start), x1 = xForTime (end);
            g.setColour (Colours::panel.withAlpha (0.62f));
            g.fillRect (juce::Rectangle<float> (wave.getX(), wave.getY() - 1.0f, x0 - wave.getX(), wave.getHeight() + 2.0f));
            g.fillRect (juce::Rectangle<float> (x1, wave.getY() - 1.0f, wave.getRight() - x1, wave.getHeight() + 2.0f));

            g.setColour (Colours::reference.withAlpha (0.08f));
            g.fillRect (juce::Rectangle<float> (x0, wave.getY() - 1.0f, x1 - x0, wave.getHeight() + 2.0f));
            g.setColour (Colours::reference.withAlpha (0.5f));
            g.fillRect (juce::Rectangle<float> (x0, wave.getY() - 1.0f, x1 - x0, 2.0f));

            for (const float x : { x0, x1 })
            {
                g.setColour (Colours::reference.brighter (0.3f));
                g.fillRect (juce::Rectangle<float> (x - 1.0f, wave.getY() - 1.0f, 2.0f, wave.getHeight() + 2.0f));
                const auto tab = juce::Rectangle<float> (6.0f, juce::jmin (16.0f, wave.getHeight() * 0.4f)).withCentre ({ x, wave.getCentreY() });
                g.fillRoundedRectangle (tab, 3.0f);
                g.setColour (Colours::panel.withAlpha (0.8f));
                g.fillRect (tab.withSizeKeepingCentre (1.0f, tab.getHeight() * 0.5f));
            }
        }

        // Short-term loudness line (on the silhouette's top edge; a faint copy outlines the bottom edge)
        g.setColour (Colours::text.withAlpha (0.28f));
        g.strokePath (loudnessMirrorPath, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.strokePath (loudnessPath, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (Colours::text.withAlpha (0.8f));
        g.strokePath (loudnessPath, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Hover position
        if (hover && drag == Drag::none && hoverX >= wave.getX() && hoverX <= wave.getRight())
        {
            g.setColour (Colours::text.withAlpha (0.22f));
            g.fillRect (juce::Rectangle<float> (hoverX - 0.5f, wave.getY(), 1.0f, wave.getHeight()));
        }

        paintPlayhead (g);
    }

    juce::Rectangle<float> playheadArea (double t) const
    {
        const float x = xForTime (t);
        return { x - 6.0f, wave.getY() - 2.0f, 12.0f, wave.getHeight() + 4.0f };
    }

    void paintPlayhead (juce::Graphics& g) const
    {
        const float x = xForTime (playhead);
        const auto colour = sounding ? Colours::text : Colours::textDim.withAlpha (0.7f);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (x - 1.5f, wave.getY(), 3.0f, wave.getHeight()));
        g.setColour (colour);
        g.fillRect (juce::Rectangle<float> (x - 0.75f, wave.getY(), 1.5f, wave.getHeight()));

        juce::Path tri;
        tri.addTriangle (x - 4.5f, wave.getY() - 1.0f, x + 4.5f, wave.getY() - 1.0f, x, wave.getY() + 4.5f);
        g.fillPath (tri);
    }

    void paintRuler (juce::Graphics& g) const
    {
        const double len = length();

        if (len <= 0.0)
            return;

        double start = 0.0, end = 0.0;
        const bool loop = regionShown (start, end);
        const auto f = font (10.5f);
        g.setFont (f);

        // Tick spacing: the smallest step that keeps labels ~64 px apart.
        const double steps[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
        double step = steps[std::size (steps) - 1];

        for (const double s : steps)
            if (s / len * wave.getWidth() >= 64.0)
            {
                step = s;
                break;
            }

        std::vector<juce::Rectangle<float>> taken;

        auto labelArea = [&] (const juce::String& text, float centreX, const juce::Font& labelFont)
        {
            const float w = textWidth (labelFont, text) + 2.0f;
            const float x = juce::jlimit (wave.getX(), wave.getRight() - w, centreX - w * 0.5f);
            return juce::Rectangle<float> (x, ruler.getY() + 1.0f, w, ruler.getHeight() - 1.0f);
        };

        auto drawLabel = [&] (const juce::String& text, juce::Rectangle<float> area, juce::Colour colour, const juce::Font& labelFont)
        {
            taken.push_back (area);
            g.setFont (labelFont);
            g.setColour (colour);
            g.drawText (text, area, juce::Justification::centred, false);
        };

        if (loop)
        {
            // Loop ends in bold lavender; one "start - end" label when they would overlap (short loops).
            const auto bold = font (10.5f, 700);
            const auto colour = Colours::reference.brighter (0.35f);
            const auto startText = fmt::time (start), endText = fmt::time (end);
            const auto a = labelArea (startText, xForTime (start), bold), b = labelArea (endText, xForTime (end), bold);

            if (a.expanded (4.0f, 0.0f).intersects (b))
            {
                const auto text = startText + kDash + endText;
                drawLabel (text, labelArea (text, (xForTime (start) + xForTime (end)) * 0.5f, bold), colour, bold);
            }
            else
            {
                drawLabel (startText, a, colour, bold);
                drawLabel (endText, b, colour, bold);
            }
        }

        for (double t = step; t < len - step * 0.25; t += step)
        {
            const float x = xForTime (t);
            g.setColour (Colours::outline.brighter (0.15f));
            g.fillRect (juce::Rectangle<float> (x - 0.5f, ruler.getY() - 1.0f, 1.0f, 3.0f));

            const auto text = fmt::time (t);
            const auto area = labelArea (text, x, f);
            bool free = true;

            for (const auto& r : taken)
                free = free && ! r.expanded (10.0f, 0.0f).intersects (area);

            if (free)
                drawLabel (text, area, Colours::textSubtle, f);
        }
    }

    void paintPill (juce::Graphics& g, const juce::String& text) const
    {
        const auto f = font (11.0f, 600);
        const float w = textWidth (f, text) + 30.0f;
        const auto pill = juce::Rectangle<float> (wave.getRight() - w - 6.0f, wave.getY() + 6.0f, w, 20.0f);
        g.setColour (Colours::panel.withAlpha (0.92f));
        g.fillRoundedRectangle (pill, 10.0f);
        g.setColour (Colours::reference.withAlpha (0.6f));
        g.drawRoundedRectangle (pill.reduced (0.5f), 10.0f, 1.0f);

        const float angle = animationPhase (1000) * juce::MathConstants<float>::twoPi;
        juce::Path arc;
        arc.addCentredArc (pill.getX() + 12.0f, pill.getCentreY(), 4.5f, 4.5f, angle, 0.0f, 4.2f, true);
        g.setColour (Colours::reference);
        g.strokePath (arc, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        g.setFont (f);
        g.setColour (Colours::text);
        g.drawText (text, pill.withTrimmedLeft (21.0f), juce::Justification::centredLeft, false);
    }

    // ---- interaction ----------------------------------------------------------------------------------------------
    Drag hitTestEdges (float x) const
    {
        double start = 0.0, end = 0.0;

        if (! regionShown (start, end))
            return Drag::none;

        const float x0 = xForTime (start), x1 = xForTime (end);
        const float d0 = std::abs (x - x0), d1 = std::abs (x - x1);

        if (juce::jmin (d0, d1) > kEdgeGrabPx)
            return Drag::none;

        return d0 <= d1 ? Drag::startEdge : Drag::endEdge;
    }

    bool interactive() const noexcept { return hasTrack(); }

    WaveformStrip& owner;
    EditorActions& actions;

    RefMatchProcessor::ReferenceInfo info;
    std::shared_ptr<const rm::ReferenceTrack> track;
    double playhead = 0.0;
    bool sounding = false;

    juce::Rectangle<float> gutter, wave, ruler, dropZone;
    juce::Path wavePath, loudnessPath, loudnessMirrorPath, silhouettePath;
    bool hasLoudness = false;   // the track has short-term loudness values (silhouette + line drawn)
    std::tuple<const rm::ReferenceTrack*, juce::Rectangle<float>, double> cachedFor {};

    bool hover = false, dropHighlight = false;
    float hoverX = 0.0f;
    Drag drag = Drag::none;
    float downX = 0.0f;
    double anchorTime = 0.0;
    std::optional<std::pair<double, double>> localRegion;   // being dragged, or committed and not yet confirmed
    int localRegionInfoVersion = -1;                        // info.version when the region was committed
    juce::uint32 localRegionCommitMs = 0;
};

//==============================================================================
WaveformStrip::WaveformStrip (EditorActions& actions) : impl (std::make_unique<Impl> (*this, actions))
{
    setTitle ("Reference waveform");
    setDescription ("Click to jump, drag to loop a section, double-click to clear the loop");
}

WaveformStrip::~WaveformStrip() = default;

void WaveformStrip::setData (const ViewData& data)
{
    auto& d = *impl;
    bool infoChanged = data.referenceInfo.version != d.info.version || data.referenceInfo.state != d.info.state
                             || data.referenceInfo.hasLoop != d.info.hasLoop || data.referenceInfo.loopStart != d.info.loopStart
                             || data.referenceInfo.loopEnd != d.info.loopEnd || data.referenceInfo.analysing != d.info.analysing
                             || data.referenceInfo.lengthSeconds != d.info.lengthSeconds
                             || data.referenceInfo.displayName != d.info.displayName || data.referenceInfo.error != d.info.error;
    const bool trackChanged = data.track != d.track;
    const double oldPlayhead = d.playhead;
    const bool oldSounding = d.sounding;

    d.info = data.referenceInfo;
    d.track = data.track;

    if (trackChanged)
    {
        // A drag on the old track must not set a loop on the new one.
        d.drag = Impl::Drag::none;
        d.localRegion.reset();
    }

    d.playhead = data.playheadSeconds;
    d.sounding = data.referenceSounding;

    // A committed region stays on screen until the processor reports a new reference state (no flicker back to the
    // old region), or for at most a second when the processor ignored the request.
    if (d.localRegion.has_value() && d.drag == Impl::Drag::none
        && (d.info.version != d.localRegionInfoVersion
            || juce::Time::getMillisecondCounter() - d.localRegionCommitMs > kCommittedRegionHoldMs))
    {
        d.localRegion.reset();
        infoChanged = true;
    }

    if (infoChanged || trackChanged || oldSounding != d.sounding)
    {
        repaint();
        return;
    }

    if (d.info.state == RefState::Loading || d.info.analysing)
    {
        repaint();   // spinners
        return;
    }

    if (d.hasTrack() && std::abs (oldPlayhead - d.playhead) > 1.0e-4)
    {
        repaint (d.playheadArea (oldPlayhead).getSmallestIntegerContainer());
        repaint (d.playheadArea (d.playhead).getSmallestIntegerContainer());
        repaint (d.gutter.getSmallestIntegerContainer());
    }
}

void WaveformStrip::setDropHighlight (bool shouldHighlight)
{
    if (impl->dropHighlight != shouldHighlight)
    {
        impl->dropHighlight = shouldHighlight;
        repaint();
    }
}

void WaveformStrip::resized()
{
    impl->layout();
}

void WaveformStrip::paint (juce::Graphics& g)
{
    auto& d = *impl;
    drawPanel (g, getLocalBounds().toFloat());

    if (! d.hasTrack())
    {
        d.paintDropZone (g);
        return;
    }

    d.updateCache();
    d.paintGutter (g);
    d.paintWave (g);
    d.paintRuler (g);

    if (d.info.state == RefState::Loading)
        d.paintPill (g, "Loading new reference...");
    else if (d.info.analysing)
        d.paintPill (g, d.info.hasLoop ? "Analysing loop..." : "Analysing the track...");
    else if (d.info.state == RefState::Error)
    {
        // Keeps the previous track; say why the new one failed.
        const auto f = font (11.0f, 600);
        const auto text = "Couldn't load: " + d.info.error;
        const float w = juce::jmin (d.wave.getWidth() - 12.0f, textWidth (f, text) + 20.0f);
        const auto pill = juce::Rectangle<float> (d.wave.getRight() - w - 6.0f, d.wave.getY() + 6.0f, w, 20.0f);
        g.setColour (Colours::panel.withAlpha (0.92f));
        g.fillRoundedRectangle (pill, 10.0f);
        g.setColour (Colours::major.withAlpha (0.7f));
        g.drawRoundedRectangle (pill.reduced (0.5f), 10.0f, 1.0f);
        g.setFont (f);
        g.setColour (Colours::major.brighter (0.2f));
        drawFittedLine (g, text, pill.reduced (10.0f, 0.0f), juce::Justification::centred, 0.8f);
    }

    if (d.dropHighlight)
    {
        g.setColour (Colours::reference.withAlpha (0.7f));
        drawDashedRoundedRect (g, getLocalBounds().toFloat().reduced (3.0f), 7.0f, 1.5f);
    }
}

void WaveformStrip::mouseMove (const juce::MouseEvent& e)
{
    auto& d = *impl;
    d.hover = true;
    d.hoverX = e.position.x;

    if (d.interactive() && (d.wave.expanded (0.0f, 4.0f).contains (e.position) || d.ruler.contains (e.position)))
        setMouseCursor (d.hitTestEdges (e.position.x) != Impl::Drag::none ? juce::MouseCursor::LeftRightResizeCursor
                                                                           : juce::MouseCursor::PointingHandCursor);
    else
        setMouseCursor (d.hasTrack() ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);

    repaint();
}

void WaveformStrip::mouseExit (const juce::MouseEvent&)
{
    impl->hover = false;
    repaint();
}

void WaveformStrip::mouseDown (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! isLeftClick (e))
        return;

    if (! d.hasTrack())
    {
        if (d.info.state != RefState::Loading)
            d.actions.browseForReference();

        return;
    }

    if (! (d.wave.expanded (0.0f, 4.0f).contains (e.position) || d.ruler.contains (e.position)))
        return;

    d.downX = e.position.x;
    d.drag = d.hitTestEdges (e.position.x);

    if (d.drag == Impl::Drag::none)
        d.drag = Impl::Drag::pending;
    else
        d.localRegion = std::make_pair (d.info.loopStart, d.info.loopEnd);
}

void WaveformStrip::mouseDrag (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! isLeftClick (e) || d.drag == Impl::Drag::none)
        return;

    const double t = d.timeForX (e.position.x);
    const double minLen = RefMatchProcessor::kMinLoopSeconds;

    switch (d.drag)
    {
        case Impl::Drag::pending:
            if (std::abs (e.position.x - d.downX) < kDragThresholdPx)
                return;

            d.drag = Impl::Drag::create;
            d.anchorTime = d.timeForX (d.downX);
            [[fallthrough]];

        case Impl::Drag::create:
            d.localRegion = std::make_pair (juce::jmin (d.anchorTime, t), juce::jmax (d.anchorTime, t));
            break;

        case Impl::Drag::startEdge:
            if (d.localRegion.has_value())
                d.localRegion->first = juce::jlimit (0.0, juce::jmax (0.0, d.localRegion->second - minLen), t);
            break;

        case Impl::Drag::endEdge:
            if (d.localRegion.has_value())
                d.localRegion->second = juce::jlimit (juce::jmin (d.length(), d.localRegion->first + minLen), d.length(), t);
            break;

        case Impl::Drag::none:
            break;
    }

    d.hoverX = e.position.x;
    repaint();
}

void WaveformStrip::mouseUp (const juce::MouseEvent& e)
{
    auto& d = *impl;
    const auto mode = d.drag;
    d.drag = Impl::Drag::none;

    if (! isLeftClick (e) || mode == Impl::Drag::none)
        return;

    if (mode == Impl::Drag::pending)
    {
        d.actions.seekReference (d.timeForX (e.position.x));
        return;
    }

    if (d.localRegion.has_value())
    {
        const auto [start, end] = *d.localRegion;

        if (end - start >= RefMatchProcessor::kMinLoopSeconds)
        {
            d.localRegionInfoVersion = d.info.version;
            d.localRegionCommitMs = juce::Time::getMillisecondCounter();
            d.actions.setLoopRegion (start, end);
        }
        else
        {
            d.localRegion.reset();   // too short: treat as a click
            d.actions.seekReference (d.timeForX (e.position.x));
        }
    }

    repaint();
}

void WaveformStrip::mouseDoubleClick (const juce::MouseEvent& e)
{
    auto& d = *impl;

    if (! isLeftClick (e) || ! d.hasTrack())
        return;

    double start = 0.0, end = 0.0;

    if (d.regionShown (start, end))
    {
        d.localRegion.reset();
        d.drag = Impl::Drag::none;
        d.actions.clearLoopRegion();
        repaint();
    }
}

juce::String WaveformStrip::getTooltip()
{
    auto& d = *impl;

    if (! d.hasTrack())
        return d.info.state == RefState::Loading ? juce::String()
                                                 : "Load a reference: drop a song here or click to browse. Pick a released "
                                                   "track in the same genre that sounds the way you want yours to sound.";

    const auto pos = getMouseXYRelative().toFloat();

    if (d.gutter.contains (pos))
        return "The reference track. The shape and its white edge show its loudness over time (the faint outline behind "
               "it is the waveform): the loud parts (drop, chorus) are the best parts to compare with the same part of "
               "your song.";

    if (d.hitTestEdges (pos.x) != Impl::Drag::none)
        return "Drag to move this end of the loop";

    return fmt::time (d.timeForX (pos.x)) + ": click to jump here. Drag across the waveform to loop a section (e.g. the "
                                            "drop), double-click to clear the loop.";
}

} // namespace rm::ui
