#pragma once

#include "ui/ViewData.h"
#include "ui/Widgets.h"

// Stereo image: width per band (mix vs reference mini bars), the overall phase correlation (-1..+1) of both, and a
// clear low-end mono badge ("Bass mono: OK" / "Bass too wide").
namespace rm::ui
{
class StereoPanel final : public juce::Component, public juce::TooltipClient
{
public:
    StereoPanel();
    ~StereoPanel() override;

    void setData (const ViewData&, bool snap);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    juce::String getTooltip() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoPanel)
};
} // namespace rm::ui
