#pragma once

#include "ui/ViewData.h"
#include "ui/Widgets.h"

// Top bar: logo + wordmark, reference file chip (Load / remove, loading and error states), the big A/B switch with the
// matching gain, Level match, Mono, options menu (free-run, analysis window), Reset analysis and Help.
namespace rm::ui
{
class HeaderBar final : public juce::Component
{
public:
    explicit HeaderBar (EditorActions&);
    ~HeaderBar() override;

    // listenPosition: the displayed (eased) A/B crossfade position.
    void setData (const ViewData&, float listenPosition);
    void setHelpOpen (bool);

    void paint (juce::Graphics&) override;
    void resized() override;

    // What the A/B switch shows for this data (tests / dev tool): REFERENCE selected only while there is a track to
    // hear; sub-labels "your track" / "not heard" and "turned down 3.9 dB" / "same level" / "own level" / ...
    static ABSwitch::State switchStateFor (const ViewData&, float listenPosition);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HeaderBar)
};
} // namespace rm::ui
