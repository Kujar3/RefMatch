#include "ui/Widgets.h"

#include "ui/ViewData.h"

#include "RefMatchBinary.h"

namespace rm::ui
{
namespace
{
//==============================================================================
// 2x2 box-filter downscale (premultiplied ARGB): a clean small logo from the 192 px PNG (same as Chordbloom).
juce::Image halveImage (const juce::Image& source)
{
    const auto src = source.convertedToFormat (juce::Image::ARGB);
    const int w = src.getWidth() / 2, h = src.getHeight() / 2;

    if (w < 1 || h < 1)
        return src;

    juce::Image dst (juce::Image::ARGB, w, h, true);
    const juce::Image::BitmapData s (src, juce::Image::BitmapData::readOnly);
    juce::Image::BitmapData d (dst, juce::Image::BitmapData::writeOnly);

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            juce::uint32 a = 0, r = 0, g = 0, b = 0;

            for (int dy = 0; dy < 2; ++dy)
                for (int dx = 0; dx < 2; ++dx)
                {
                    const auto* p = reinterpret_cast<const juce::PixelARGB*> (s.getPixelPointer (x * 2 + dx, y * 2 + dy));
                    a += p->getAlpha();
                    r += p->getRed();
                    g += p->getGreen();
                    b += p->getBlue();
                }

            auto* q = reinterpret_cast<juce::PixelARGB*> (d.getPixelPointer (x, y));
            q->setARGB ((juce::uint8) ((a + 2) / 4), (juce::uint8) ((r + 2) / 4),
                        (juce::uint8) ((g + 2) / 4), (juce::uint8) ((b + 2) / 4));
        }
    }

    return dst;
}

// The logo PNG at 96 and 48 px, decoded once (released at shutdown with the other JUCE singletons). Both are
// invalid when the resource is missing.
struct LogoImages final : public juce::DeletedAtShutdown
{
    LogoImages()
    {
        int size = 0;

        if (auto* data = RefMatchBinary::getNamedResource ("logo_png", size); data != nullptr && size > 0)
        {
            const auto full = juce::ImageFileFormat::loadFrom (data, (size_t) size);

            if (full.isValid())
            {
                large = full.getWidth() > 120 ? halveImage (full) : full;
                small = large.getWidth() > 60 ? halveImage (large) : large;
            }
        }
    }

    ~LogoImages() override { clearSingletonInstance(); }

    juce::Image large, small;

    JUCE_DECLARE_SINGLETON_INLINE (LogoImages, false)
};

void drawVectorLogo (juce::Graphics& g, juce::Rectangle<float> area)
{
    // Two soft overlapping discs: the mix (mint, top left) and the reference (lavender, bottom right).
    const float s = area.getWidth();
    const auto mint = juce::Rectangle<float> (s * 0.80f, s * 0.80f).withPosition (area.getX() + s * 0.02f, area.getY() + s * 0.02f);
    const auto lav = juce::Rectangle<float> (s * 0.66f, s * 0.66f).withPosition (area.getX() + s * 0.32f, area.getY() + s * 0.32f);

    g.setGradientFill (juce::ColourGradient (Colours::mix.brighter (0.25f), mint.getX(), mint.getY(),
                                             Colours::mix.darker (0.15f), mint.getRight(), mint.getBottom(), false));
    g.fillEllipse (mint);

    g.setColour (juce::Colours::black.withAlpha (0.18f));
    g.fillEllipse (lav.translated (-0.6f, -0.6f).expanded (0.8f));
    g.setGradientFill (juce::ColourGradient (Colours::reference.brighter (0.2f), lav.getX(), lav.getY(),
                                             Colours::reference.darker (0.2f), lav.getRight(), lav.getBottom(), false));
    g.fillEllipse (lav);
}
} // namespace

//==============================================================================
namespace fmt
{
juce::String number (float value, int decimals)
{
    if (! rm::hasValue (value))
        return "--";

    const float half = 0.5f * std::pow (10.0f, (float) -decimals);

    if (std::abs (value) < half)
        value = 0.0f;

    // (juce::String (x, 0) would print full precision, not zero decimals)
    return decimals > 0 ? juce::String (value, decimals) : juce::String (juce::roundToInt (value));
}

juce::String signedNumber (float value, int decimals)
{
    if (! rm::hasValue (value))
        return "--";

    const auto text = number (value, decimals);
    return value > 0.0f && text.getDoubleValue() != 0.0 ? "+" + text : text;
}

juce::String time (double seconds)
{
    const auto total = (long long) std::floor (juce::jmax (0.0, seconds));
    const auto h = total / 3600, m = (total / 60) % 60, s = total % 60;

    if (h > 0)
        return juce::String (h) + ":" + juce::String (m).paddedLeft ('0', 2) + ":" + juce::String (s).paddedLeft ('0', 2);

    return juce::String (m) + ":" + juce::String (s).paddedLeft ('0', 2);
}

juce::String duration (double seconds)
{
    if (seconds < 9.95)
        return juce::String (juce::jmax (0.0, seconds), 1) + " s";

    if (seconds < 59.5)
        return juce::String (juce::roundToInt (seconds)) + " s";

    return time (seconds);
}

juce::String sampleRate (double hz)
{
    if (hz <= 0.0)
        return {};

    const double khz = hz / 1000.0;
    const bool whole = std::abs (khz - std::round (khz)) < 0.05;
    return (whole ? juce::String (juce::roundToInt (khz)) : juce::String (khz, 1)) + " kHz";
}
} // namespace fmt

//==============================================================================
bool referenceExpected (const ViewData& data)
{
    using State = RefMatchProcessor::ReferenceState;
    return data.track != nullptr || data.referenceInfo.state == State::Ready || data.referenceInfo.state == State::Loading;
}

bool referencePlayable (const ViewData& data)
{
    return data.track != nullptr || data.referenceInfo.state == RefMatchProcessor::ReferenceState::Ready;
}

bool hearingReference (const ViewData& data)
{
    return data.listenReference && referencePlayable (data);
}

//==============================================================================
juce::AttributedString richText (const juce::String& markup, float size, juce::Colour normal, juce::Colour strong,
                                 float lineSpacing)
{
    juce::AttributedString s;
    s.setWordWrap (juce::AttributedString::byWord);
    s.setJustification (juce::Justification::topLeft);
    s.setLineSpacing (lineSpacing);

    int start = 0;
    bool bold = false;

    for (;;)
    {
        const int index = markup.indexOf (start, "**");
        const auto piece = index < 0 ? markup.substring (start) : markup.substring (start, index);

        if (piece.isNotEmpty())
            s.append (piece, font (size, bold ? 600 : 400), bold ? strong : normal);

        if (index < 0)
            break;

        start = index + 2;
        bold = ! bold;
    }

    return s;
}

void drawFittedLine (juce::Graphics& g, const juce::String& text, juce::Rectangle<float> area,
                     juce::Justification justification, float minScale)
{
    g.drawFittedText (text, area.toNearestInt(), justification, 1, minScale);
}

namespace
{
bool animationsFrozen = false;   // message thread only
}

float animationPhase (juce::uint32 periodMs)
{
    const auto now = animationsFrozen ? juce::uint32 { 250 } : juce::Time::getMillisecondCounter();
    return periodMs > 0 ? (float) (now % periodMs) / (float) periodMs : 0.0f;
}

void freezeAnimationsForSnapshots (bool shouldFreeze)
{
    animationsFrozen = shouldFreeze;
}

juce::Colour blend (juce::Colour a, juce::Colour b, float amountOfB)
{
    return a.interpolatedWith (b, juce::jlimit (0.0f, 1.0f, amountOfB));
}

void drawLogo (juce::Graphics& g, juce::Rectangle<float> area)
{
    auto& images = *LogoImages::getInstance();

    if (! images.small.isValid())
    {
        drawVectorLogo (g, area);
        return;
    }

    const float pixelScale = g.getInternalContext().getPhysicalPixelScaleFactor();
    const auto& image = (area.getWidth() * pixelScale > 48.0f && images.large.isValid()) ? images.large : images.small;
    juce::Graphics::ScopedSaveState state (g);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (image, area, juce::RectanglePlacement::centred);
}

//==============================================================================
IconButton::IconButton (const juce::String& name, Icon iconType) : juce::Button (name), icon (iconType)
{
    setTitle (name);
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
}

void IconButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const bool enabled = isEnabled();
    const bool on = getToggleState();
    highlighted = highlighted && enabled;
    down = down && enabled;

    if (highlighted || down || on)
    {
        g.setColour (Colours::panelHover.withAlpha (down ? 1.0f : 0.85f));

        if (roundBackground)
        {
            g.fillEllipse (b);
        }
        else
        {
            g.fillRoundedRectangle (b, 6.0f);
            g.setColour (on ? Colours::accent.withAlpha (0.45f) : Colours::outline.brighter (0.1f));
            g.drawRoundedRectangle (b, 6.0f, 1.0f);
        }
    }

    auto colour = on ? Colours::accent : (highlighted ? Colours::text : Colours::textDim);

    if (! enabled)
        colour = Colours::textFaint.withAlpha (0.55f);

    g.setColour (colour);
    paintIcon (g, icon, b.withSizeKeepingCentre (16.0f, 16.0f));
}

void IconButton::paintIcon (juce::Graphics& g, Icon type, juce::Rectangle<float> box)
{
    const float x0 = box.getX(), y0 = box.getY();
    const juce::PathStrokeType stroke (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);

    switch (type)
    {
        case Icon::help:
        {
            const auto circle = box.reduced (0.5f);
            g.drawEllipse (circle, 1.4f);
            g.setFont (font (11.0f, 700));
            g.drawText ("?", circle.translated (0.0f, 0.2f), juce::Justification::centred, false);
            break;
        }

        case Icon::reset:
        {
            // Circular arrow (counter-clockwise), arrow head at the top left.
            const auto c = box.getCentre();
            juce::Path arc;
            arc.addCentredArc (c.x, c.y, 5.6f, 5.6f, 0.0f, -2.2f, 2.6f, true);
            g.strokePath (arc, stroke);

            const auto tip = c.getPointOnCircumference (5.6f, -2.2f);
            juce::Path head;
            head.startNewSubPath (tip.x - 3.6f, tip.y - 0.6f);
            head.lineTo (tip.x, tip.y);
            head.lineTo (tip.x + 0.2f, tip.y - 3.6f);
            g.strokePath (head, stroke);
            break;
        }

        case Icon::options:
        {
            for (int i = 0; i < 3; ++i)
                g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre ({ x0 + 3.5f + 4.5f * (float) i, y0 + 8.0f }));
            break;
        }

        case Icon::close:
        {
            const auto c = box.getCentre();
            const float s = 4.0f;
            juce::Path p;
            p.startNewSubPath (c.x - s, c.y - s);
            p.lineTo (c.x + s, c.y + s);
            p.startNewSubPath (c.x + s, c.y - s);
            p.lineTo (c.x - s, c.y + s);
            g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;
        }

        case Icon::folder:
        {
            juce::Path p;
            p.startNewSubPath (x0 + 1.5f, y0 + 4.0f);
            p.lineTo (x0 + 6.0f, y0 + 4.0f);
            p.lineTo (x0 + 7.5f, y0 + 5.6f);
            p.lineTo (x0 + 14.5f, y0 + 5.6f);
            p.lineTo (x0 + 14.5f, y0 + 13.0f);
            p.lineTo (x0 + 1.5f, y0 + 13.0f);
            p.closeSubPath();
            g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            break;
        }
    }
}

//==============================================================================
ABSwitch::ABSwitch()
{
    setTitle ("Listen");
    setDescription ("A/B switch: hear your mix or the reference");
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setTooltip ("A/B switch. MIX = your track exactly as it is. REFERENCE = the reference track. With Level match on "
                "(the default) it is turned down or up to the same loudness as your mix, so you compare the sound and "
                "not the volume. Switch back and forth while FL plays.");
}

void ABSwitch::setState (const State& newState)
{
    if (newState == state)
        return;

    const bool valueChanged = newState.listenReference != state.listenReference;
    state = newState;
    repaint();

    if (valueChanged)
        if (auto* handler = getAccessibilityHandler())
            handler->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
}

void ABSwitch::paint (juce::Graphics& g)
{
    // A few pixels of margin around the body leave room for the active side's glow.
    const auto body = getLocalBounds().toFloat().reduced (4.0f);
    const float radius = juce::jmin (10.0f, body.getHeight() * 0.5f);

    g.setColour (juce::Colours::black.withAlpha (0.22f));
    g.fillRoundedRectangle (body.translated (0.0f, 1.0f), radius);
    g.setGradientFill (juce::ColourGradient::vertical (Colours::background.darker (0.12f), body.getY(),
                                                       Colours::background.brighter (0.02f), body.getBottom()));
    g.fillRoundedRectangle (body, radius);
    g.setColour (Colours::outline.withAlpha (0.9f));
    g.drawRoundedRectangle (body.reduced (0.5f), radius, 1.0f);

    const auto inner = body.reduced (3.0f);
    const float segW = inner.getWidth() * 0.5f;
    const float pos = juce::jlimit (0.0f, 1.0f, state.position);
    const auto highlight = juce::Rectangle<float> (inner.getX() + segW * pos, inner.getY(), segW, inner.getHeight());
    const auto colour = blend (Colours::mix, Colours::reference, pos);
    const float segRadius = radius - 2.5f;

    // Active side: glow, tinted fill, coloured rim.
    {
        juce::Path shape;
        shape.addRoundedRectangle (highlight, segRadius);
        juce::DropShadow (colour.withAlpha (0.45f), 14, {}).drawForPath (g, shape);

        g.setGradientFill (juce::ColourGradient::vertical (blend (Colours::panelRaised, colour, 0.34f), highlight.getY(),
                                                           blend (Colours::panel, colour, 0.20f), highlight.getBottom()));
        g.fillPath (shape);
        g.setColour (colour.withAlpha (0.85f));
        g.drawRoundedRectangle (highlight.reduced (0.5f), segRadius, 1.2f);
        g.setColour (juce::Colours::white.withAlpha (0.10f));
        g.fillRect (highlight.reduced (segRadius, 0.0f).withHeight (1.0f).translated (0.0f, 1.0f));
    }

    const auto mouse = getMouseXYRelative().toFloat();
    const int hover = isMouseOver() ? (mouse.x < inner.getCentreX() ? 0 : 1) : -1;
    const bool twoLines = inner.getHeight() >= 30.0f;

    for (int i = 0; i < 2; ++i)
    {
        const auto seg = juce::Rectangle<float> (inner.getX() + segW * (float) i, inner.getY(), segW, inner.getHeight());
        const bool isRef = i == 1;
        const float activeAmount = isRef ? pos : 1.0f - pos;
        const auto roleColour = isRef ? Colours::reference : Colours::mix;
        const bool enabled = ! isRef || state.referenceAvailable;

        auto labelColour = blend (hover == i && enabled ? Colours::text : Colours::textDim, roleColour.brighter (0.35f), activeAmount);
        auto subColour = blend (Colours::textSubtle, roleColour.brighter (0.1f), activeAmount);

        if (! enabled)
        {
            labelColour = Colours::textFaint;
            subColour = Colours::textFaint.withAlpha (0.8f);
        }

        // Hovering the side that is not selected: a faint fill and rim (like the other chips' hover), clearly weaker
        // than the selected highlight so it never looks as if the audio already switched.
        if (hover == i && enabled && activeAmount < 0.5f)
        {
            const float k = 1.0f - activeAmount;
            const auto hoverArea = seg.reduced (1.0f);
            g.setColour (Colours::panelHover.withAlpha (0.6f * k));
            g.fillRoundedRectangle (hoverArea, segRadius);
            g.setColour (roleColour.withAlpha (0.3f * k));
            g.drawRoundedRectangle (hoverArea.reduced (0.5f), segRadius, 1.0f);
        }

        const juce::String label (isRef ? "REFERENCE" : "MIX");
        const auto labelFont = font (juce::jmin (13.5f, seg.getHeight() * 0.4f), 700).withExtraKerningFactor (0.08f);
        const auto sub = isRef ? state.referenceSubLabel : state.mixSubLabel;

        // Role dot before the label.
        const float dotD = 6.0f;
        const float labelW = juce::jmin (textWidth (labelFont, label), seg.getWidth() - 24.0f);
        const float startX = std::round (seg.getCentreX() - (labelW + dotD + 6.0f) * 0.5f);
        const float labelY = twoLines && sub.isNotEmpty() ? seg.getY() + seg.getHeight() * 0.13f : seg.getY();
        const float labelH = twoLines && sub.isNotEmpty() ? seg.getHeight() * 0.46f : seg.getHeight();

        g.setColour (enabled ? roleColour.withAlpha (0.45f + 0.55f * activeAmount) : Colours::textFaint);
        g.fillEllipse (juce::Rectangle<float> (dotD, dotD).withCentre ({ startX + dotD * 0.5f, labelY + labelH * 0.5f }));

        g.setFont (labelFont);
        g.setColour (labelColour);
        g.drawFittedText (label, juce::Rectangle<float> (startX + dotD + 6.0f, labelY, labelW + 2.0f, labelH).toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.8f);

        if (twoLines && sub.isNotEmpty())
        {
            g.setFont (font (juce::jmin (11.0f, seg.getHeight() * 0.32f), 600));
            g.setColour (subColour);
            g.drawFittedText (sub, juce::Rectangle<float> (seg.getX() + 6.0f, seg.getY() + seg.getHeight() * 0.55f,
                                                           seg.getWidth() - 12.0f, seg.getHeight() * 0.36f).toNearestInt(),
                              juce::Justification::centred, 1, 0.85f);
        }
    }
}

void ABSwitch::mouseDown (const juce::MouseEvent& e)
{
    if (! isLeftClick (e))   // right / middle clicks never switch (Theme.h rule)
        return;

    choose (e.position.x >= (float) getWidth() * 0.5f);
}

void ABSwitch::choose (bool reference)
{
    if (reference && ! state.referenceAvailable)
        return;

    if (onChoose != nullptr)
        onChoose (reference);
}

std::unique_ptr<juce::AccessibilityHandler> ABSwitch::createAccessibilityHandler()
{
    struct Value final : public juce::AccessibilityTextValueInterface
    {
        explicit Value (ABSwitch& s) : owner (s) {}
        bool isReadOnly() const override { return false; }
        juce::String getCurrentValueAsString() const override { return owner.state.listenReference ? "Reference" : "Mix"; }
        void setValueAsString (const juce::String& v) override { owner.choose (v.trim().equalsIgnoreCase ("Reference")); }
        ABSwitch& owner;
    };

    return std::make_unique<juce::AccessibilityHandler> (
        *this, juce::AccessibilityRole::comboBox,
        juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press,
                                                [this] { choose (! state.listenReference); }),
        juce::AccessibilityHandler::Interfaces { std::make_unique<Value> (*this) });
}

//==============================================================================
MenuChip::MenuChip()
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    setRepaintsOnMouseActivity (true);
}

void MenuChip::setText (const juce::String& prefix, const juce::String& value)
{
    if (prefix == prefixText && value == valueText)
        return;

    prefixText = prefix;
    valueText = value;
    repaint();
}

int MenuChip::getIdealWidth() const
{
    const float prefixW = prefixText.isEmpty() ? 0.0f : textWidth (font (11.5f, 600), prefixText) + 6.0f;
    return (int) std::ceil (12.0f + prefixW + textWidth (font (12.5f, 600), valueText) + 26.0f);
}

void MenuChip::setMenuOpen (bool open)
{
    menuOpen = open;
    repaint();
}

void MenuChip::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    const float radius = juce::jmin (8.0f, b.getHeight() * 0.5f);
    const bool hover = isMouseOver (true) && isEnabled();

    auto fill = menuOpen ? Colours::panelHover : (hover ? Colours::panelHover.withAlpha (0.85f) : Colours::panelRaised);
    g.setGradientFill (juce::ColourGradient::vertical (fill.brighter (0.04f), b.getY(), fill.darker (0.05f), b.getBottom()));
    g.fillRoundedRectangle (b, radius);
    g.setColour (menuOpen ? Colours::accent.withAlpha (0.7f) : Colours::outline.brighter (hover ? 0.18f : 0.0f));
    g.drawRoundedRectangle (b, radius, 1.0f);

    auto text = b.withTrimmedLeft (12.0f).withTrimmedRight (24.0f);

    if (prefixText.isNotEmpty())
    {
        const auto f = font (11.5f, 600);
        const float w = juce::jmin (text.getWidth() * 0.5f, textWidth (f, prefixText) + 1.0f);
        g.setFont (f);
        g.setColour (Colours::textDim);
        g.drawText (prefixText, text.removeFromLeft (w), juce::Justification::centredLeft, false);
        text.removeFromLeft (6.0f);
    }

    g.setFont (font (12.5f, 600));
    g.setColour (hover || menuOpen ? Colours::text : Colours::text.withAlpha (0.92f));
    g.drawFittedText (valueText, text.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);

    const float cx = b.getRight() - 13.0f, cy = b.getCentreY();
    juce::Path chevron;
    chevron.startNewSubPath (cx - 3.2f, cy - 1.5f);
    chevron.lineTo (cx, cy + 1.7f);
    chevron.lineTo (cx + 3.2f, cy - 1.5f);
    g.setColour (hover || menuOpen ? Colours::text : Colours::textDim);
    g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void MenuChip::mouseDown (const juce::MouseEvent&)
{
    if (isEnabled() && ! menuOpen && onOpenMenu != nullptr)
        onOpenMenu (*this);
}

} // namespace rm::ui
