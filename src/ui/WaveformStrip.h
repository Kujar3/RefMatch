#pragma once

#include "ui/ViewData.h"
#include "ui/Widgets.h"

// Reference waveform strip: overview of the whole reference (mono min / max), its short-term loudness, the loop region
// (drag to create, drag the edges to adjust, double-click to clear), the playhead (click to seek) and a time ruler.
// Without a reference it is a big, friendly drop target ("Drop a reference track here ... or click to browse"); while
// loading / after an error it says so.
namespace rm::ui
{
class WaveformStrip final : public juce::Component, public juce::TooltipClient
{
public:
    explicit WaveformStrip (EditorActions&);
    ~WaveformStrip() override;

    void setData (const ViewData&);
    void setDropHighlight (bool);   // a file is being dragged over the editor

    void paint (juce::Graphics&) override;
    void resized() override;

    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    juce::String getTooltip() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformStrip)
};
} // namespace rm::ui
