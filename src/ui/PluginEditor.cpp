#include "ui/PluginEditor.h"

#include "ui/HeaderBar.h"
#include "ui/HelpOverlay.h"
#include "ui/HintsPanel.h"
#include "ui/LoudnessPanel.h"
#include "ui/StereoPanel.h"
#include "ui/TonalBalancePanel.h"
#include "ui/WaveformStrip.h"

namespace
{
namespace Col = rm::ui::Colours;

// Room left around the editor for the host's plug-in window frame when the editor size is fitted to a screen.
constexpr int kScreenMarginW = 40;
constexpr int kScreenMarginH = 90;

constexpr juce::uint32 kNoticeMs = 4000;
constexpr float kListenEase = 0.45f;   // A/B highlight slide per 30 Hz tick
constexpr int kListenFrameStrip = 4;   // width of the editor edge strips repainted when the listening frame changes

// Open RefMatch editors in this plug-in binary (message thread only), so the default look-and-feel can be handed to
// a remaining editor when one closes.
juce::Array<RefMatchEditor*>& liveEditors()
{
    static juce::Array<RefMatchEditor*> editors;
    return editors;
}

juce::Rectangle<int> primaryUserArea()
{
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        return display->userBounds.getLargestIntegerWithin();

    return {};
}

juce::Point<int> initialSize (juce::Point<int> saved)
{
    int w = saved.x > 0 ? saved.x : RefMatchEditor::kDefaultWidth;
    int h = saved.y > 0 ? saved.y : RefMatchEditor::kDefaultHeight;
    const auto screen = primaryUserArea();

    if (! screen.isEmpty())
    {
        w = juce::jmin (w, screen.getWidth() - kScreenMarginW);
        h = juce::jmin (h, screen.getHeight() - kScreenMarginH);
    }

    // The resizable minimum wins on very small screens (the host can still move the window).
    return { juce::jlimit (RefMatchEditor::kMinWidth, RefMatchEditor::kMaxWidth, w),
             juce::jlimit (RefMatchEditor::kMinHeight, RefMatchEditor::kMaxHeight, h) };
}

void drawDashedRoundedRect (juce::Graphics& g, juce::Rectangle<float> r, float radius, float thickness)
{
    juce::Path outline, dashed;
    outline.addRoundedRectangle (r, radius);
    const float dashes[] = { 8.0f, 6.0f };
    juce::PathStrokeType (thickness).createDashedStroke (dashed, outline, dashes, 2);
    g.fillPath (dashed);
}
} // namespace

//==============================================================================
struct RefMatchEditor::Impl final : public rm::ui::EditorActions
{
    // Every panel lives in this component, laid out at the editor's logical size. Above the default size the editor
    // scales it uniformly (RefMatchEditor::zoomForSize), so text, controls and panels grow together; at and below the
    // default size it fills the editor 1:1 and the layout reflows.
    struct Content final : public juce::Component
    {
        explicit Content (Impl& o) : impl (o) { setTitle ("RefMatch content"); }

        void paint (juce::Graphics& g) override             { impl.paintContent (g); }
        void paintOverChildren (juce::Graphics& g) override { impl.paintContentOverChildren (g); }
        void resized() override                             { impl.layoutContent(); }

        Impl& impl;
    };

    Impl (RefMatchEditor& o, RefMatchProcessor& p)
        : owner (o), processor (p), header (*this), strip (*this)
    {
        help.onClose = [this] { setHelpVisible (false); };
    }

    //==========================================================================
    // EditorActions
    void browseForReference() override
    {
        if (chooser != nullptr)
            return;

        // Start where the last reference came from, but only on a local disk: JUCE's native chooser checks the start
        // folder synchronously, and a disconnected network share could stall the message thread (and the host).
        using RefState = RefMatchProcessor::ReferenceState;
        const auto info = processor.getReferenceInfo();
        const bool fromReference = (info.state == RefState::Ready || info.state == RefState::Error) && info.file != juce::File();
        auto folder = fromReference ? info.file.getParentDirectory() : lastFolder;

        if (folder == juce::File() || folder.getFullPathName().startsWith ("\\\\") || ! folder.isOnHardDisk())
        {
            folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        }
        else if (info.state == RefState::Error)
        {
            // Relink after a project moved: the old folder may be gone, so start in its nearest existing parent (only
            // probed after the network-path checks above).
            while (! folder.isDirectory() && folder.getParentDirectory() != folder)
                folder = folder.getParentDirectory();

            if (! folder.isDirectory())
                folder = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
        }

        chooser = std::make_unique<juce::FileChooser> ("Choose a reference track", folder,
                                                       RefMatchProcessor::getSupportedWildcard());

        juce::Component::SafePointer<RefMatchEditor> safe (&owner);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe] (const juce::FileChooser& fc)
                              {
                                  if (safe == nullptr)
                                      return;

                                  const auto file = fc.getResult();
                                  auto& d = *safe->impl;

                                  if (file != juce::File())
                                      d.loadFile (file);

                                  // Released after the callback returns (the chooser is still on the stack here).
                                  juce::MessageManager::callAsync ([safe]
                                  {
                                      if (safe != nullptr)
                                          safe->impl->chooser.reset();
                                  });
                              });
    }

    void removeReference() override
    {
        processor.clearReference();
        setParameter (RefMatchProcessor::kParamListen, 0.0f);   // back to MIX (also updates the view)
    }

    void seekReference (double seconds) override
    {
        processor.seekReference (seconds);
    }

    void setLoopRegion (double startSeconds, double endSeconds) override
    {
        const auto before = processor.getReferenceInfo();
        processor.setLoopRegion (startSeconds, endSeconds);
        const auto after = processor.getReferenceInfo();

        // A new loop (not a small edge adjustment, nor a request the processor ignored): remind the user to measure
        // the same part of the mix.
        const double centre = 0.5 * (after.loopStart + after.loopEnd);

        if (after.hasLoop && after.version != before.version
            && (! before.hasLoop || centre < before.loopStart || centre > before.loopEnd))
            showNotice ("Loop set - play the same part of your song, then click Reset (round arrow, top right)", false);

        update (false);
    }

    void clearLoopRegion() override
    {
        processor.clearLoopRegion();
        update (false);
    }

    void setParameter (const char* paramId, float plainValue) override
    {
        if (auto* param = processor.getAPVTS().getParameter (paramId))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
            param->endChangeGesture();
        }

        update (false);
    }

    void resetAnalysis() override
    {
        processor.resetMixAnalysis();
        showNotice ("Analysis reset - play your mix again", false);
        update (false);
    }

    void toggleHelp() override
    {
        setHelpVisible (! help.isVisible());
    }

    //==========================================================================
    void loadFile (const juce::File& file)
    {
        if (! RefMatchProcessor::isSupportedAudioFile (file))
        {
            showNotice ("Can't open \"" + file.getFileName() + "\" - use " + rm::ui::kSupportedFormatsText, true);
            return;
        }

        lastFolder = file.getParentDirectory();
        processor.loadReference (file);
        update (false);
    }

    void setHelpVisible (bool shouldShow)
    {
        if (shouldShow)
        {
            help.setBounds (content.getLocalBounds());
            help.toFront (false);
        }

        help.setVisible (shouldShow);
        header.setHelpOpen (shouldShow);
    }

    void showNotice (const juce::String& text, bool isError)
    {
        const auto oldArea = noticeArea();
        notice = text;
        noticeIsError = isError;
        noticeShownMs = juce::Time::getMillisecondCounter();
        content.repaint (oldArea.getUnion (noticeArea()));
    }

    // Area (content coordinates) to repaint for the current notice (empty when none), including its shadow.
    juce::Rectangle<int> noticeArea() const
    {
        return notice.isEmpty() ? juce::Rectangle<int>() : noticeBounds().getSmallestIntegerContainer().expanded (24);
    }

    juce::Rectangle<float> noticeBounds() const
    {
        const auto f = rm::ui::font (12.5f, 600);
        const float w = juce::jmin ((float) content.getWidth() - 40.0f, rm::ui::textWidth (f, notice) + 48.0f);
        return { ((float) content.getWidth() - w) * 0.5f, (float) content.getHeight() - 58.0f, w, 34.0f };
    }

    // Pulls fresh data (or the override) and hands it to every panel. snap: no easing. The actions above call it
    // without snapping for immediate feedback that keeps the A/B slide and the easing.
    void update (bool snap)
    {
        const auto data = overrideData.has_value() ? *overrideData : rm::ui::gatherViewData (processor);

        // The A/B highlight follows the parameter (the user's choice) and slides towards it - towards MIX while there
        // is no reference to play (the processor plays the mix then, whatever Listen says).
        const float target = overrideData.has_value() ? data.listenPosition : (rm::ui::hearingReference (data) ? 1.0f : 0.0f);
        const float oldShown = shownListen;
        shownListen = (snap || std::abs (target - shownListen) < 0.01f) ? target : shownListen + (target - shownListen) * kListenEase;

        if (std::abs (shownListen - oldShown) > 0.001f)
            repaintListenFrame();

        header.setData (data, shownListen);
        strip.setData (data);
        tonal.setData (data, snap);
        loudness.setData (data, snap);
        stereo.setData (data, snap);
        hints.setData (data);
    }

    // While the reference is heard: a lavender frame around the whole editor, so going back to FL's mixer while still
    // on REFERENCE (and wondering why EQ moves change nothing) is hard to miss. Editor coordinates, unscaled.
    void paintListenFrame (juce::Graphics& g) const
    {
        if (shownListen <= 0.01f || dragActive)
            return;

        g.setColour (Col::reference.withAlpha (0.7f * juce::jmin (1.0f, shownListen)));
        g.drawRoundedRectangle (owner.getLocalBounds().toFloat().reduced (1.0f), 8.0f, 2.0f);
    }

    void repaintListenFrame()
    {
        const auto b = owner.getLocalBounds();
        const int s = kListenFrameStrip;
        owner.repaint (b.withHeight (s));
        owner.repaint (b.withTop (b.getBottom() - s));
        owner.repaint (b.withWidth (s));
        owner.repaint (b.withLeft (b.getRight() - s));
    }

    // ---- content painting / layout (logical coordinates) -----------------------------------------------------------
    void paintContent (juce::Graphics& g) const
    {
        // Soft contact shadow under every card (the panels paint their own surfaces; the editor paints the background).
        for (const auto& area : panelAreas)
        {
            const auto r = area.toFloat().reduced (0.5f);
            g.setColour (juce::Colours::black.withAlpha (0.16f));
            g.fillRoundedRectangle (r.translated (0.0f, 2.0f).expanded (0.5f), rm::ui::kCornerRadius + 0.5f);
            g.setColour (juce::Colours::black.withAlpha (0.22f));
            g.fillRoundedRectangle (r.translated (0.0f, 1.0f), rm::ui::kCornerRadius);
        }
    }

    void paintContentOverChildren (juce::Graphics& g) const
    {
        if (dragActive)
            paintDropOverlay (g);

        paintNotice (g);
    }

    void layoutContent()
    {
        const int height = content.getHeight();
        const bool compact = height < 620;
        const int margin = compact ? 10 : 12;
        const int gap = compact ? 8 : 10;

        auto r = content.getLocalBounds();
        header.setBounds (r.removeFromTop (compact ? 56 : 60));
        r.reduce (margin, margin);

        const int stripH = juce::jlimit (compact ? 76 : 82, 128, juce::roundToInt ((float) height * 0.125f));
        const auto stripArea = r.removeFromTop (stripH);
        r.removeFromTop (gap);

        const int rightW = juce::jlimit (330, 520, juce::roundToInt ((float) r.getWidth() * 0.37f));
        auto right = r.removeFromRight (rightW);
        r.removeFromRight (gap);
        auto left = r;

        const int row1H = juce::roundToInt ((float) (left.getHeight() - gap) * 0.6f);
        const auto tonalArea = left.removeFromTop (row1H);
        left.removeFromTop (gap);
        const auto loudnessArea = right.removeFromTop (row1H);
        right.removeFromTop (gap);

        strip.setBounds (stripArea);
        tonal.setBounds (tonalArea);
        hints.setBounds (left);
        loudness.setBounds (loudnessArea);
        stereo.setBounds (right);
        panelAreas = { stripArea, tonalArea, left, loudnessArea, right };

        help.setBounds (content.getLocalBounds());
    }

    void paintDropOverlay (juce::Graphics& g) const
    {
        const auto b = content.getLocalBounds().toFloat();
        const auto tint = dragSupported ? Col::reference : Col::major;

        g.setColour (juce::Colours::black.withAlpha (0.38f));
        g.fillRect (b);
        g.setColour (tint.withAlpha (0.08f));
        g.fillRoundedRectangle (b.reduced (6.0f), 12.0f);
        g.setColour (tint.withAlpha (0.85f));
        drawDashedRoundedRect (g, b.reduced (6.0f), 12.0f, 2.0f);

        const juce::String line1 = dragSupported ? "Drop to load the reference" : "This file type can't be opened";
        const juce::String line2 = dragSupported ? dragName : juce::String ("Use ") + rm::ui::kSupportedFormatsText;
        const auto f1 = rm::ui::font (18.0f, 700), f2 = rm::ui::font (13.0f);
        const float w = juce::jmin (b.getWidth() - 80.0f,
                                    juce::jmax (rm::ui::textWidth (f1, line1), rm::ui::textWidth (f2, line2)) + 110.0f);
        const auto card = juce::Rectangle<float> (w, 86.0f).withCentre (b.getCentre());

        juce::Path shape;
        shape.addRoundedRectangle (card, 12.0f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.55f), 26, { 0, 8 }).drawForPath (g, shape);
        g.setColour (Col::panel.brighter (0.05f));
        g.fillPath (shape);
        g.setColour (tint.withAlpha (0.7f));
        g.drawRoundedRectangle (card.reduced (0.5f), 12.0f, 1.2f);

        const auto icon = juce::Rectangle<float> (40.0f, 40.0f).withCentre ({ card.getX() + 44.0f, card.getCentreY() });
        g.setColour (tint.withAlpha (0.18f));
        g.fillEllipse (icon);
        g.setColour (tint.brighter (0.2f));
        g.drawEllipse (icon.reduced (0.6f), 1.2f);

        const auto c = icon.getCentre();
        juce::Path arrow;

        if (dragSupported)
        {
            arrow.startNewSubPath (c.x, c.y - 9.0f);
            arrow.lineTo (c.x, c.y + 4.0f);
            arrow.startNewSubPath (c.x - 6.0f, c.y - 2.0f);
            arrow.lineTo (c.x, c.y + 4.0f);
            arrow.lineTo (c.x + 6.0f, c.y - 2.0f);
            arrow.startNewSubPath (c.x - 10.0f, c.y + 5.0f);
            arrow.lineTo (c.x - 10.0f, c.y + 10.0f);
            arrow.lineTo (c.x + 10.0f, c.y + 10.0f);
            arrow.lineTo (c.x + 10.0f, c.y + 5.0f);
        }
        else
        {
            arrow.startNewSubPath (c.x - 6.0f, c.y - 6.0f);
            arrow.lineTo (c.x + 6.0f, c.y + 6.0f);
            arrow.startNewSubPath (c.x + 6.0f, c.y - 6.0f);
            arrow.lineTo (c.x - 6.0f, c.y + 6.0f);
        }

        g.strokePath (arrow, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        auto text = card.withTrimmedLeft (80.0f).withTrimmedRight (20.0f).reduced (0.0f, 18.0f);
        g.setFont (f1);
        g.setColour (Col::text);
        rm::ui::drawFittedLine (g, line1, text.removeFromTop (26.0f), juce::Justification::centredLeft, 0.8f);
        g.setFont (f2);
        g.setColour (dragSupported ? Col::reference.brighter (0.25f) : Col::textDim);
        rm::ui::drawFittedLine (g, line2, text, juce::Justification::centredLeft, 0.8f);
    }

    void paintNotice (juce::Graphics& g) const
    {
        if (notice.isEmpty())
            return;

        const auto pill = noticeBounds();
        const auto tint = noticeIsError ? Col::major : Col::accent;
        juce::Path shape;
        shape.addRoundedRectangle (pill, pill.getHeight() * 0.5f);
        juce::DropShadow (juce::Colours::black.withAlpha (0.5f), 16, { 0, 4 }).drawForPath (g, shape);
        g.setColour (Col::panel.brighter (0.08f));
        g.fillPath (shape);
        g.setColour (tint.withAlpha (0.7f));
        g.drawRoundedRectangle (pill.reduced (0.5f), pill.getHeight() * 0.5f, 1.0f);
        g.setColour (tint);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ pill.getX() + 20.0f, pill.getCentreY() }));
        g.setFont (rm::ui::font (12.5f, 600));
        g.setColour (Col::text);
        rm::ui::drawFittedLine (g, notice, pill.withTrimmedLeft (32.0f).withTrimmedRight (16.0f),
                                juce::Justification::centredLeft, 0.8f);
    }

    // Declared first: destroyed last, after every component that uses it.
    rm::ui::LookAndFeel lookAndFeel;

    RefMatchEditor& owner;
    RefMatchProcessor& processor;

    Content content { *this };   // parent of everything below (and of the tooltip window)
    rm::ui::HeaderBar header;
    rm::ui::WaveformStrip strip;
    rm::ui::TonalBalancePanel tonal;
    rm::ui::LoudnessPanel loudness;
    rm::ui::StereoPanel stereo;
    rm::ui::HintsPanel hints;
    rm::ui::HelpOverlay help;

    std::unique_ptr<juce::TooltipWindow> tooltipWindow;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File lastFolder;   // folder of the last file loaded from this editor
    std::optional<rm::ui::ViewData> overrideData;
    float shownListen = 0.0f;

    std::vector<juce::Rectangle<int>> panelAreas;   // for the contact shadows

    bool dragActive = false, dragSupported = false;
    juce::String dragName;

    juce::String notice;
    bool noticeIsError = false;
    juce::uint32 noticeShownMs = 0;

    bool persistSize = false;
    int focusSweepCountdown = 30;
};

//==============================================================================
RefMatchEditor::RefMatchEditor (RefMatchProcessor& p) : AudioProcessorEditor (p), impl (std::make_unique<Impl> (*this, p))
{
    auto& d = *impl;
    setLookAndFeel (&d.lookAndFeel);

    // Popup menus / alert windows created without an explicit look-and-feel use the default one: make that ours
    // while this editor is open (per plug-in binary, so other plug-ins are unaffected).
    juce::LookAndFeel::setDefaultLookAndFeel (&d.lookAndFeel);
    liveEditors().add (this);

    setOpaque (true);
    setTitle ("RefMatch");

    addAndMakeVisible (d.content);

    for (juce::Component* c : std::initializer_list<juce::Component*> { &d.header, &d.strip, &d.tonal, &d.loudness,
                                                                        &d.stereo, &d.hints })
        d.content.addAndMakeVisible (c);

    d.content.addChildComponent (d.help);

    // A child of the content, so tooltips scale with the rest of the editor.
    d.tooltipWindow = std::make_unique<juce::TooltipWindow> (&d.content, 650);
    d.tooltipWindow->setOpaque (false);   // rounded bubble

    // Size first, constrainer second: setResizeLimits() clamps the current bounds, which would otherwise overwrite
    // the remembered size (through resized()) before it is read.
    const auto size = initialSize (p.getEditorSize());
    setSize (size.x, size.y);
    setResizable (true, true);
    setResizeLimits (kMinWidth, kMinHeight, kMaxWidth, kMaxHeight);
    d.persistSize = true;

    // Never take keyboard focus away from the host (FL Studio's spacebar and shortcuts keep working).
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    rm::ui::neverTakeKeyboardFocus (*this);

    refreshNow();
    startTimerHz (30);
}

RefMatchEditor::~RefMatchEditor()
{
    stopTimer();
    juce::PopupMenu::dismissAllActiveMenus();
    impl->tooltipWindow.reset();
    impl->chooser.reset();

    liveEditors().removeAllInstancesOf (this);

    if (&juce::LookAndFeel::getDefaultLookAndFeel() == &impl->lookAndFeel)
        juce::LookAndFeel::setDefaultLookAndFeel (liveEditors().isEmpty() ? nullptr
                                                                         : &liveEditors().getLast()->getLookAndFeel());

    setLookAndFeel (nullptr);
}

//==============================================================================
void RefMatchEditor::paint (juce::Graphics& g)
{
    // Background of the whole (physical) editor; the content component paints the cards' shadows on top.
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient::vertical (Col::background.brighter (0.03f), b.getY(),
                                                       Col::background.darker (0.08f), b.getBottom()));
    g.fillRect (b);
}

void RefMatchEditor::paintOverChildren (juce::Graphics& g)
{
    // (The drop overlay and the notice are painted by the content component, so they scale with it.)
    impl->paintListenFrame (g);
}

float RefMatchEditor::zoomForSize (int width, int height) noexcept
{
    return juce::jlimit (1.0f, kMaxZoom, juce::jmin ((float) width / (float) kDefaultWidth,
                                                      (float) height / (float) kDefaultHeight));
}

void RefMatchEditor::resized()
{
    auto& d = *impl;

    // Up to the default size the layout reflows; above it everything is scaled uniformly (the content keeps a
    // logical size near the default, and its resized() lays the panels out).
    const float zoom = zoomForSize (getWidth(), getHeight());
    d.content.setTransform (zoom > 1.0f ? juce::AffineTransform::scale (zoom) : juce::AffineTransform());
    d.content.setBounds (0, 0, (int) std::ceil ((float) getWidth() / zoom), (int) std::ceil ((float) getHeight() / zoom));

    if (d.persistSize)
        d.processor.setEditorSize ({ getWidth(), getHeight() });
}

//==============================================================================
bool RefMatchEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    // Every file is accepted so an unsupported one can say why it does not load.
    return ! files.isEmpty();
}

void RefMatchEditor::fileDragEnter (const juce::StringArray& files, int, int)
{
    auto& d = *impl;
    d.dragActive = true;
    d.dragSupported = false;
    d.dragName = files.isEmpty() ? juce::String() : juce::File (files[0]).getFileName();

    for (const auto& path : files)
    {
        if (RefMatchProcessor::isSupportedAudioFile (juce::File (path)))
        {
            d.dragSupported = true;
            d.dragName = juce::File (path).getFileName();
            break;
        }
    }

    d.strip.setDropHighlight (d.dragSupported);
    repaint();
}

void RefMatchEditor::fileDragExit (const juce::StringArray&)
{
    impl->dragActive = false;
    impl->strip.setDropHighlight (false);
    repaint();
}

void RefMatchEditor::filesDropped (const juce::StringArray& files, int, int)
{
    auto& d = *impl;
    d.dragActive = false;
    d.strip.setDropHighlight (false);
    repaint();

    for (const auto& path : files)
    {
        const juce::File file (path);

        if (RefMatchProcessor::isSupportedAudioFile (file))
        {
            d.loadFile (file);
            return;
        }
    }

    if (! files.isEmpty())
        d.loadFile (juce::File (files[0]));   // shows why it can't be opened
}

//==============================================================================
void RefMatchEditor::refreshNow()
{
    impl->update (true);
}

void RefMatchEditor::setViewDataOverride (std::optional<rm::ui::ViewData> data)
{
    impl->overrideData = std::move (data);
    refreshNow();
}

void RefMatchEditor::setHelpVisible (bool shouldShow)
{
    impl->setHelpVisible (shouldShow);
}

bool RefMatchEditor::isHelpVisible() const
{
    return impl->help.isVisible();
}

void RefMatchEditor::setDragHighlightForTesting (bool shouldHighlight)
{
    auto& d = *impl;
    d.dragActive = shouldHighlight;
    d.dragSupported = true;
    d.dragName = "Neon Skyline - Club Mix.wav";
    d.strip.setDropHighlight (shouldHighlight);
    repaint();
}

void RefMatchEditor::showTooltipForTesting (juce::Point<int> positionInEditor, const juce::String& text)
{
    if (impl->tooltipWindow != nullptr && text.isNotEmpty())
        impl->tooltipWindow->displayTip (localPointToGlobal (positionInEditor), text);
}

void RefMatchEditor::hideTooltipForTesting()
{
    if (impl->tooltipWindow != nullptr)
        impl->tooltipWindow->hideTip();
}

void RefMatchEditor::timerCallback()
{
    auto& d = *impl;
    d.update (false);

    if (d.notice.isNotEmpty() && juce::Time::getMillisecondCounter() - d.noticeShownMs > kNoticeMs)
    {
        const auto area = d.noticeArea();
        d.notice.clear();
        d.content.repaint (area);
    }

    // Once a second: make sure no component created since can take keyboard focus.
    if (--d.focusSweepCountdown <= 0)
    {
        d.focusSweepCountdown = 30;
        rm::ui::neverTakeKeyboardFocus (*this);
    }
}
