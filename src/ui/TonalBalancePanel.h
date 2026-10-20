#pragma once

#include "ui/ViewData.h"
#include "ui/Widgets.h"

// Tonal balance: the reference's average spectrum (soft lavender area) and the mix's (crisp mint line, loudness-matched
// with Comparison::normalisationOffsetDb) on a 20 Hz - 20 kHz log axis with a +4.5 dB/octave display tilt, faint band
// separators, and below it six band difference bars (+-6 dB, coloured by severity) with a comparative word each
// ("muddier", "thinner", "harsher", "duller", "close"...). Hovering a band explains it. Curves and bars ease towards
// new data.
namespace rm::ui
{
class TonalBalancePanel final : public juce::Component, public juce::TooltipClient
{
public:
    TonalBalancePanel();
    ~TonalBalancePanel() override;

    // snap = jump to the new values (no easing).
    void setData (const ViewData&, bool snap);

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    juce::String getTooltip() override;

    // Short comparative word for a band difference ("muddier", "thinner", "close"...).
    static juce::String bandWord (int band, float levelDiffDb);

    // The band tooltip's sentence: "Your mix: 3.4 dB more than the reference (both at the same loudness)".
    static juce::String bandDifferenceText (float levelDiffDb);

    // +4.5 dB/octave display tilt (pivot 1 kHz).
    static float displayTiltDb (float frequencyHz) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TonalBalancePanel)
};
} // namespace rm::ui
