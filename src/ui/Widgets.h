#pragma once

#include "ui/Theme.h"

#include <functional>

// Small building blocks shared by the editor's panels: number formatting, the actions panels can ask the editor
// for, icon buttons, the big A/B switch, a drop-down chip and rich text with **bold** runs.
namespace rm::ui
{
struct ViewData;

//==============================================================================
// Text formatting (ASCII only so every font renders it).
namespace fmt
{
    juce::String number (float value, int decimals);         // "-11.8"; "--" for kNoValue
    juce::String signedNumber (float value, int decimals);   // "+3.1", "-0.4", "0.0" (no "-0.0"); "--" for kNoValue
    juce::String time (double seconds);                      // "1:04", "12:30", "1:02:03"
    juce::String duration (double seconds);                  // "32 s", "1:04"
    juce::String sampleRate (double hz);                     // "44.1 kHz", "48 kHz"
}

// The file types RefMatch can load, as one phrase for every text that lists them.
inline constexpr const char* kSupportedFormatsText = "WAV, MP3, FLAC, AIFF or OGG";

//==============================================================================
// True when a reference is loaded or on its way (loading, or an earlier track still playing after a failed load):
// panels then say "analysing" instead of "load a reference".
bool referenceExpected (const ViewData&);

// True when there is a reference track the A/B switch can play (the REFERENCE side is enabled).
bool referencePlayable (const ViewData&);

// True when REFERENCE is selected and there is a track to hear: the user hears the reference, not the mix (the
// processor plays the mix whatever Listen says while no track is installed).
bool hearingReference (const ViewData&);

//==============================================================================
// What the panels can ask the editor to do. Implemented by the editor (which talks to the processor), so panels only
// ever draw ViewData and report user gestures.
struct EditorActions
{
    virtual ~EditorActions() = default;

    virtual void browseForReference() = 0;                          // async FileChooser
    virtual void removeReference() = 0;
    virtual void seekReference (double seconds) = 0;
    virtual void setLoopRegion (double startSeconds, double endSeconds) = 0;
    virtual void clearLoopRegion() = 0;
    virtual void setParameter (const char* paramId, float plainValue) = 0;   // one complete host gesture
    virtual void resetAnalysis() = 0;
    virtual void toggleHelp() = 0;
};

//==============================================================================
// Rich text: "**bold**" runs in `strong`, the rest in `normal`. Word-wrapped, top-left.
juce::AttributedString richText (const juce::String& markup, float size, juce::Colour normal, juce::Colour strong,
                                 float lineSpacing = 2.0f);

// Draws `text` in one line, shrinking it horizontally down to `minScale` and eliding beyond that.
void drawFittedLine (juce::Graphics&, const juce::String& text, juce::Rectangle<float> area,
                     juce::Justification, float minScale = 0.9f);

// Phase 0..1 of a looping animation (spinners, progress shimmer) with the given period. The dev tool freezes the
// clock so snapshots are deterministic.
float animationPhase (juce::uint32 periodMs);
void freezeAnimationsForSnapshots (bool shouldFreeze);

// Mixes two colours for gradients / glows (alpha kept from `a`).
juce::Colour blend (juce::Colour a, juce::Colour b, float amountOfB);

// RefMatch's logo: the embedded logo.png (resource "logo_png") when present, else a vector-drawn mint / lavender
// mark of the same shape. `area` should be square.
void drawLogo (juce::Graphics&, juce::Rectangle<float> area);

//==============================================================================
// Small icon-only button (header tools, close buttons). Only left clicks act (right / middle clicks are ignored).
class IconButton final : public juce::Button
{
public:
    enum class Icon { help, reset, options, close, folder };

    IconButton (const juce::String& name, Icon iconType);

    void mouseDown (const juce::MouseEvent& e) override { if (isLeftClick (e)) juce::Button::mouseDown (e); }
    void mouseDrag (const juce::MouseEvent& e) override { if (isLeftClick (e)) juce::Button::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override   { if (isLeftClick (e)) juce::Button::mouseUp (e); }

    void paintButton (juce::Graphics&, bool highlighted, bool down) override;

    // Draws an icon into a 16 x 16 box (also used by other components).
    static void paintIcon (juce::Graphics&, Icon, juce::Rectangle<float> box);

    bool roundBackground = false;   // circular hover fill (close buttons)

private:
    Icon icon;
};

//==============================================================================
// The big "MIX | REFERENCE" switch. The highlight follows the processor's crossfade position, so it slides while the
// audio fades. Each half has a sub-label (e.g. "your track", "turned down 3.9 dB"). Left clicks pick the clicked side.
// Hovering the inactive (enabled) half gives it a faint fill, weaker than the selected highlight.
class ABSwitch final : public juce::Component, public juce::SettableTooltipClient
{
public:
    ABSwitch();

    struct State
    {
        bool listenReference = false;      // target (parameter)
        float position = 0.0f;             // displayed crossfade position 0 = mix .. 1 = reference
        bool referenceAvailable = false;
        juce::String mixSubLabel, referenceSubLabel;

        bool operator== (const State& o) const
        {
            return listenReference == o.listenReference && std::abs (position - o.position) < 0.002f
                   && referenceAvailable == o.referenceAvailable && mixSubLabel == o.mixSubLabel
                   && referenceSubLabel == o.referenceSubLabel;
        }
        bool operator!= (const State& o) const { return ! operator== (o); }
    };

    void setState (const State&);
    const State& getState() const noexcept { return state; }

    std::function<void (bool reference)> onChoose;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override   { repaint(); }
    void mouseExit (const juce::MouseEvent&) override   { repaint(); }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void choose (bool reference);

    State state;
};

//==============================================================================
// Pill-shaped drop-down: dim prefix ("Match") + value ("Integrated") + chevron. Opens its menu on any click (it only
// ever opens a menu); the owner builds and shows the menu in onOpenMenu.
class MenuChip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    MenuChip();

    void setText (const juce::String& prefix, const juce::String& value);
    int getIdealWidth() const;

    void setMenuOpen (bool);
    std::function<void (MenuChip&)> onOpenMenu;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    juce::String prefixText, valueText;
    bool menuOpen = false;
};

} // namespace rm::ui
