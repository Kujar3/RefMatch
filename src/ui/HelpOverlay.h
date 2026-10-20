#pragma once

#include "ui/Widgets.h"

// Beginner's guide shown over the whole editor ("?" button): how to set RefMatch up on the FL master, pick and loop a
// reference, A/B with level matching, read each panel, and what LUFS / true peak / PLR / LRA / width / correlation
// mean in one sentence each. A click anywhere (or the x) closes it.
namespace rm::ui
{
class HelpOverlay final : public juce::Component
{
public:
    HelpOverlay();
    ~HelpOverlay() override;

    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HelpOverlay)
};
} // namespace rm::ui
