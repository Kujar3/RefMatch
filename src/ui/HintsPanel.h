#pragma once

#include "ui/ViewData.h"
#include "ui/Widgets.h"

// "What to try": the hints from Comparison::hints, most important first. The first one is a full-width card with
// larger text; the others are cards (two columns when there is room) when they all fit, else one-line rows (title +
// severity tag) that show their advice as a tooltip and open on a left click. The title row counts the tips and how
// many are still below ("8 tips - 6 more below"). Severity tags: "BIG DIFFERENCE" for big differences from the
// reference, "CHECK THIS" for big problems found in the mix alone (true peak, bass mono, phase), "WORTH A LOOK", "TIP".
namespace rm::ui
{
class HintsPanel final : public juce::Component
{
public:
    HintsPanel();
    ~HintsPanel() override;

    void setData (const ViewData&);

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;   // fade where a card is cut at the bottom
    void resized() override;

    // The cards this data gives, in order, as "TAG | title" lines (tests / dev tool).
    static juce::StringArray describeHints (const ViewData&);

    // Bounds of card `index` (0 = the first hint) in this panel's coordinates; empty when there is none (dev tool).
    // The cards themselves are a child component titled "Hint cards".
    juce::Rectangle<int> getCardBoundsForTesting (int index) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HintsPanel)
};
} // namespace rm::ui
