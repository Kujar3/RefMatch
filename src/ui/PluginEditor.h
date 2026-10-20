#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "plugin/PluginProcessor.h"
#include "ui/Theme.h"
#include "ui/ViewData.h"

#include <optional>

// RefMatch editor: header (reference file, A/B switch, matching, options), reference waveform with loop region,
// tonal balance (spectrum overlay + per-band difference), loudness & dynamics, stereo image, plain-language hints,
// and a beginner's help overlay. Default 1100 x 690, resizable 960 x 560 .. 1600 x 1000.
// Never takes keyboard focus (FL Studio keeps its shortcuts). Accepts audio files dropped anywhere on it.
class RefMatchEditor final : public juce::AudioProcessorEditor,
                             public juce::FileDragAndDropTarget,
                             private juce::Timer
{
public:
    static constexpr int kDefaultWidth = 1100, kDefaultHeight = 690;
    static constexpr int kMinWidth = 960, kMinHeight = 560;
    static constexpr int kMaxWidth = 1600, kMaxHeight = 1000;

    // Above the default size the whole UI is scaled uniformly (text and controls grow with the window) by
    // zoomForSize(): min (width / kDefaultWidth, height / kDefaultHeight), limited to 1 .. kMaxZoom. At and below the
    // default size the zoom is 1 and the layout reflows.
    static constexpr float kMaxZoom = 1.33f;
    static float zoomForSize (int width, int height) noexcept;

    explicit RefMatchEditor (RefMatchProcessor&);
    ~RefMatchEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;   // frame around the editor while the reference is heard
    void resized() override;

    // FileDragAndDropTarget
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

    //==============================================================================
    // Dev tool hooks (snapshots)
    void refreshNow();                                       // pull from the processor immediately
    void setViewDataOverride (std::optional<rm::ui::ViewData>);   // draw this instead of the processor's data
    void setHelpVisible (bool);
    bool isHelpVisible() const;
    void setDragHighlightForTesting (bool);
    // Shows a tooltip as if the mouse rested at `positionInEditor` (editor coordinates); hideTooltipForTesting
    // removes it.
    void showTooltipForTesting (juce::Point<int> positionInEditor, const juce::String& text);
    void hideTooltipForTesting();

private:
    void timerCallback() override;

    struct Impl;   // owned by the UI agent: child components, layout, look and feel
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RefMatchEditor)
};
