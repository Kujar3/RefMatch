#pragma once

#include "ui/ViewData.h"
#include "ui/Widgets.h"

// Loudness & dynamics: mix vs reference for integrated loudness, short-term loudness, true peak, dynamics (PLR) and
// loudness range (LRA), each with small comparison bars and a plain-language one-liner ("4.2 LU quieter than the
// reference - normal before mastering"). Every row has a beginner's tooltip.
namespace rm::ui
{
class LoudnessPanel final : public juce::Component, public juce::TooltipClient
{
public:
    LoudnessPanel();
    ~LoudnessPanel() override;

    void setData (const ViewData&, bool snap);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    juce::String getTooltip() override;

    // Every row for this data as "Name | mix | ref | severity | one-liner" (tests / dev tool). The one-liners use the
    // same thresholds as the hint cards (rm::kLoudnessDiffLu, kLouderMajorLu, kPlrDiffDb, kPlrMajorDb, true peak).
    static juce::StringArray describeRows (const ViewData&);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoudnessPanel)
};
} // namespace rm::ui
