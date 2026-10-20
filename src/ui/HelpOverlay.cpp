#include "ui/HelpOverlay.h"

namespace rm::ui
{
namespace
{
struct Step
{
    juce::String title, body;
    juce::Colour colour;
};

const juce::String kTitle ("Getting started with RefMatch");
const juce::String kSubtitle ("Compare your mix with a song you love - fairly, at the same loudness - and see what to change.");
} // namespace

//==============================================================================
struct HelpOverlay::Impl
{
    Impl()
    {
        steps = {
            { "Put it on the master",
              "Add RefMatch as the **last plugin on FL Studio's Master** track, after your limiter. With **MIX** selected "
              "and **Mono** off it does not change your sound at all. Leave FL's **Master fader at 0 dB** - RefMatch "
              "hears your song before that fader, so its loudness and peak numbers match your export only at 0 dB.",
              Colours::mix },
            { "Pick a reference in your genre",
              "Drop a released song **in the same genre** onto the strip at the top (" + juce::String (kSupportedFormatsText)
                  + ") - one whose sound you would love your song to have.", Colours::reference },
            { "Loop the best part",
              "**Drag across the waveform** to loop the drop or chorus. Play the same part of your song, then press "
              "**reset** so both are measured over the same music. Click the waveform to jump, double-click to clear "
              "the loop.", Colours::info },
            { "Switch A/B while FL plays",
              "Click **MIX** and **REFERENCE**. **Level match** sets the reference to your mix's loudness, so you judge "
              "the sound - not which one is louder. **Mono** sums left and right into one, as many club and phone "
              "speakers do - parts that get weaker or vanish are too wide. Switch it off again afterwards.",
              Colours::minor },
        };

        panels = {
            "**Tonal balance** - mint line = your mix, purple area = the reference. The bars below show which ranges "
            "you have more of (up) or less of (down).",
            "**Loudness & dynamics** - how loud, how peaky and how dynamic your mix is next to the reference.",
            "**Stereo image** - how wide each range is. Bass and kick should stay in the middle (mono).",
            "**What to try** - the biggest differences first, with a concrete fix. Make small moves, then listen again. "
            "Press the **reset** button after changing your mix or moving to another part of the song.",
        };

        glossary = {
            "**LUFS** - loudness as your ears hear it. Streaming plays songs at about -14 LUFS.",
            "**True peak (dBTP)** - the highest point of the waveform. Keep a master below -1 dBTP.",
            "**PLR** - how far the peaks rise above the average loudness. Lower = more squashed.",
            "**LRA** - loudness range: how much the loudness moves between quiet and loud parts.",
            "**Width** - how much of a range is spread left / right instead of in the middle.",
            "**Correlation** - +1 is mono, 0 is very wide, below 0 parts cancel out in mono.",
        };

        close.roundBackground = true;
        close.setTooltip ("Close");
    }

    struct Block
    {
        juce::TextLayout layout;
        juce::Rectangle<float> area;
    };

    // Lays out the card content at a width with a body text size; returns the content height.
    float layoutContent (float cardW, float size)
    {
        stepBlocks.clear();
        panelBlocks.clear();
        glossaryBlocks.clear();

        const float colGap = 30.0f;
        const float leftW = (cardW - 2.0f * kPad - colGap) * 0.5f;
        const float rightX = kPad + leftW + colGap;
        const float textIndent = 34.0f;
        const auto body = Colours::textDim.brighter (0.15f), strong = Colours::text;

        float y = kHeaderH;
        stepTitleYs.clear();

        for (const auto& s : steps)
        {
            stepTitleYs.push_back (y);
            Block b;
            b.layout.createLayout (richText (s.body, size, body, strong), leftW - textIndent);
            b.area = { kPad + textIndent, y + 20.0f, leftW - textIndent, b.layout.getHeight() };
            y = b.area.getBottom() + 12.0f;
            stepBlocks.push_back (std::move (b));
        }

        const float leftBottom = y;

        y = kHeaderH;
        panelsTitleY = y;
        y += 20.0f;

        for (const auto& p : panels)
        {
            Block b;
            b.layout.createLayout (richText (p, size - 0.5f, body, strong), leftW - 14.0f);
            b.area = { rightX + 14.0f, y, leftW - 14.0f, b.layout.getHeight() };
            y = b.area.getBottom() + 5.0f;
            panelBlocks.push_back (std::move (b));
        }

        y += 8.0f;
        glossaryTitleY = y;
        y += 20.0f;

        for (const auto& t : glossary)
        {
            Block b;
            b.layout.createLayout (richText (t, size - 0.5f, body, strong), leftW - 14.0f);
            b.area = { rightX + 14.0f, y, leftW - 14.0f, b.layout.getHeight() };
            y = b.area.getBottom() + 4.0f;
            glossaryBlocks.push_back (std::move (b));
        }

        rightColumnX = rightX;
        columnW = leftW;
        return juce::jmax (leftBottom, y) + kFooterH;
    }

    static constexpr float kPad = 24.0f;
    static constexpr float kHeaderH = 74.0f;
    static constexpr float kFooterH = 30.0f;

    std::vector<Step> steps;
    juce::StringArray panels, glossary;
    std::vector<Block> stepBlocks, panelBlocks, glossaryBlocks;
    std::vector<float> stepTitleYs;
    float panelsTitleY = 0.0f, glossaryTitleY = 0.0f, rightColumnX = 0.0f, columnW = 0.0f;

    juce::Rectangle<float> card;
    IconButton close { "Close help", IconButton::Icon::close };
};

//==============================================================================
HelpOverlay::HelpOverlay() : impl (std::make_unique<Impl>())
{
    setTitle ("Help");
    impl->close.onClick = [this] { if (onClose != nullptr) onClose(); };
    addAndMakeVisible (impl->close);
    neverTakeKeyboardFocus (*this);
}

HelpOverlay::~HelpOverlay() = default;

void HelpOverlay::resized()
{
    auto& d = *impl;
    const float margin = 20.0f;
    const float cardW = juce::jmin (900.0f, (float) getWidth() - 2.0f * margin);
    const float maxH = (float) getHeight() - 2.0f * margin;
    float h = 0.0f;

    for (const float size : { 13.0f, 12.5f, 12.0f, 11.5f, 11.0f })
    {
        h = d.layoutContent (cardW, size);

        if (h <= maxH)
            break;
    }

    d.card = juce::Rectangle<float> (cardW, juce::jmin (h, maxH)).withCentre (getLocalBounds().toFloat().getCentre());
    d.card.setPosition (std::round (d.card.getX()), std::round (d.card.getY()));
    d.close.setBounds (juce::roundToInt (d.card.getRight()) - 40, juce::roundToInt (d.card.getY()) + 14, 28, 28);
}

void HelpOverlay::paint (juce::Graphics& g)
{
    auto& d = *impl;
    g.fillAll (juce::Colours::black.withAlpha (0.5f));

    const auto card = d.card;
    juce::Path shape;
    shape.addRoundedRectangle (card, 12.0f);
    juce::DropShadow (juce::Colours::black.withAlpha (0.6f), 32, { 0, 12 }).drawForPath (g, shape);
    g.setGradientFill (juce::ColourGradient::vertical (Colours::panel.brighter (0.07f), card.getY(),
                                                       Colours::panel.darker (0.02f), card.getBottom()));
    g.fillPath (shape);
    g.setColour (Colours::outline.brighter (0.15f));
    g.drawRoundedRectangle (card.reduced (0.5f), 12.0f, 1.0f);

    juce::Graphics::ScopedSaveState state (g);
    g.setOrigin (card.getPosition().toInt());
    const float kPad = Impl::kPad;

    // Header: logo, title, subtitle
    drawLogo (g, { kPad, 20.0f, 28.0f, 28.0f });
    g.setFont (font (18.0f, 700));
    g.setColour (Colours::text);
    g.drawText (kTitle, juce::Rectangle<float> (kPad + 40.0f, 16.0f, card.getWidth() - 2.0f * kPad - 80.0f, 22.0f),
                juce::Justification::centredLeft, true);
    g.setFont (font (12.5f));
    g.setColour (Colours::textDim);
    drawFittedLine (g, kSubtitle, { kPad + 40.0f, 38.0f, card.getWidth() - 2.0f * kPad - 80.0f, 18.0f },
                    juce::Justification::centredLeft, 0.85f);
    g.setColour (Colours::outline.withAlpha (0.7f));
    g.fillRect (juce::Rectangle<float> (kPad, 62.0f, card.getWidth() - 2.0f * kPad, 1.0f));

    // Left column: numbered steps
    for (size_t i = 0; i < d.steps.size() && i < d.stepBlocks.size(); ++i)
    {
        const auto& s = d.steps[i];
        const float y = d.stepTitleYs[i];
        const auto badge = juce::Rectangle<float> (kPad, y - 1.0f, 22.0f, 22.0f);
        g.setColour (s.colour.withAlpha (0.16f));
        g.fillEllipse (badge);
        g.setColour (s.colour.withAlpha (0.7f));
        g.drawEllipse (badge.reduced (0.5f), 1.0f);
        g.setFont (font (11.5f, 700));
        g.setColour (s.colour.brighter (0.2f));
        g.drawText (juce::String ((int) i + 1), badge, juce::Justification::centred, false);

        g.setFont (font (13.5f, 600));
        g.setColour (Colours::text);
        g.drawText (s.title, juce::Rectangle<float> (kPad + 34.0f, y, d.columnW - 34.0f, 20.0f), juce::Justification::centredLeft, true);
        d.stepBlocks[i].layout.draw (g, d.stepBlocks[i].area);
    }

    // Right column: inset with the panels and the glossary
    {
        const float x = d.rightColumnX;
        const float bottom = juce::jmax (d.glossaryBlocks.empty() ? 0.0f : d.glossaryBlocks.back().area.getBottom(), 0.0f);
        const auto inset = juce::Rectangle<float> (x - 12.0f, Impl::kHeaderH - 10.0f, d.columnW + 24.0f, bottom - Impl::kHeaderH + 20.0f);
        g.setColour (Colours::background.withAlpha (0.5f));
        g.fillRoundedRectangle (inset, 8.0f);
        g.setColour (Colours::outline.withAlpha (0.55f));
        g.drawRoundedRectangle (inset.reduced (0.5f), 8.0f, 1.0f);

        drawSectionTitle (g, "Reading the panels", juce::Rectangle<float> (x, d.panelsTitleY, d.columnW, 16.0f).toNearestInt());

        for (const auto& b : d.panelBlocks)
        {
            g.setColour (Colours::accent.withAlpha (0.8f));
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ b.area.getX() - 9.0f, b.area.getY() + 8.0f }));
            b.layout.draw (g, b.area);
        }

        drawSectionTitle (g, "Words you'll see", juce::Rectangle<float> (x, d.glossaryTitleY, d.columnW, 16.0f).toNearestInt());

        for (const auto& b : d.glossaryBlocks)
        {
            g.setColour (Colours::reference.withAlpha (0.8f));
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre ({ b.area.getX() - 9.0f, b.area.getY() + 8.0f }));
            b.layout.draw (g, b.area);
        }
    }

    g.setFont (font (11.0f));
    g.setColour (Colours::textSubtle);
    g.drawText ("Click anywhere to close", juce::Rectangle<float> (0.0f, card.getHeight() - 26.0f, card.getWidth(), 16.0f),
                juce::Justification::centred, false);
}

void HelpOverlay::mouseDown (const juce::MouseEvent&)
{
    // Any click closes it.
    if (onClose != nullptr)
        onClose();
}

} // namespace rm::ui
