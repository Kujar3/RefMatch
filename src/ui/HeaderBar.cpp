#include "ui/HeaderBar.h"

namespace rm::ui
{
namespace
{
using RefState = RefMatchProcessor::ReferenceState;

const juce::String kDot = juce::String::fromUTF8 (" \xc2\xb7 ");   // " · "

juce::String matchModeName (rm::MatchMode m)
{
    switch (m)
    {
        case rm::MatchMode::Off:        return "Off";
        case rm::MatchMode::Integrated: return "Integrated";
        case rm::MatchMode::ShortTerm:  return "Short-term";
    }

    return {};
}

// What Level match does to the reference right now: "turned down 3.9 dB", "turned up 1.2 dB", "same level".
juce::String matchGainText (float gainDb)
{
    if (! rm::hasValue (gainDb) || std::abs (gainDb) < 0.05f)
        return "same level";

    return (gainDb < 0.0f ? "turned down " : "turned up ") + fmt::number (std::abs (gainDb), 1) + " dB";
}

// The "Recent" analysis window as a menu hint ("last 30 s").
juce::String recentWindowText()
{
    return "last " + juce::String (juce::roundToInt (rm::kRecentSeconds)) + " s";
}

//==============================================================================
// The reference file chip: icon, name and details (length, sample rate) or the empty / loading / error state, with a
// small x to remove the reference. A click on the chip opens the file browser.
class ReferenceChip final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit ReferenceChip (EditorActions& a) : actions (a)
    {
        setTitle ("Reference file");
        setRepaintsOnMouseActivity (true);
        remove.roundBackground = true;
        remove.setTooltip ("Remove the reference");
        remove.onClick = [this] { actions.removeReference(); };
        addChildComponent (remove);
    }

    void setInfo (const RefMatchProcessor::ReferenceInfo& newInfo)
    {
        const bool changed = newInfo.version != info.version || newInfo.state != info.state
                             || newInfo.displayName != info.displayName || newInfo.error != info.error
                             || newInfo.analysing != info.analysing || newInfo.lengthSeconds != info.lengthSeconds;
        info = newInfo;

        if (changed)
        {
            remove.setVisible (info.state != RefState::Empty);
            updateTooltip();
            resized();
            repaint();
        }
        else if (info.state == RefState::Loading || info.analysing)
        {
            repaint (iconArea().toNearestInt().expanded (2));   // spinner
        }
    }

    void resized() override
    {
        remove.setBounds (getWidth() - 30, (getHeight() - 24) / 2, 24, 24);
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool hover = isMouseOver (true);
        const bool error = info.state == RefState::Error;
        const float radius = 8.0f;

        auto fill = hover ? Colours::panelHover.withAlpha (0.9f) : Colours::panelRaised;
        g.setGradientFill (juce::ColourGradient::vertical (fill.brighter (0.04f), b.getY(), fill.darker (0.05f), b.getBottom()));
        g.fillRoundedRectangle (b, radius);
        g.setColour (error ? Colours::major.withAlpha (0.55f)
                           : (info.state == RefState::Ready ? Colours::reference.withAlpha (hover ? 0.55f : 0.32f)
                                                            : Colours::outline.brighter (hover ? 0.18f : 0.0f)));
        g.drawRoundedRectangle (b, radius, 1.0f);

        paintIcon (g, iconArea());

        auto text = b.withTrimmedLeft (iconArea().getRight() + 9.0f).withTrimmedRight (remove.isVisible() ? 32.0f : 10.0f);
        const bool twoLines = text.getHeight() >= 28.0f;
        juce::String title, detail;
        auto titleColour = Colours::text, detailColour = Colours::textDim;

        switch (info.state)
        {
            case RefState::Empty:
                title = "No reference yet";
                detail = "Drop a song or click Load";
                titleColour = Colours::textDim.brighter (0.15f);
                detailColour = Colours::textDim;   // on the lighter chip fill: textDim keeps >= 4.5:1
                break;

            case RefState::Loading:
                title = info.displayName.isNotEmpty() ? info.displayName : juce::String ("Loading");
                detail = "Loading and analysing...";
                detailColour = Colours::reference.brighter (0.2f);
                break;

            case RefState::Ready:
                title = info.displayName;
                detail = fmt::time (info.lengthSeconds) + kDot + fmt::sampleRate (info.fileSampleRate)
                         + (info.fileChannels == 1 ? kDot + "mono" : juce::String());

                if (info.analysing)
                {
                    detail = info.hasLoop ? "Analysing loop..." : "Analysing...";
                    detailColour = Colours::reference.brighter (0.2f);
                }
                break;

            case RefState::Error:
                title = info.displayName.isNotEmpty() ? info.displayName : juce::String ("Reference");
                detail = "Couldn't load this file";   // the strip and the tooltip say why
                detailColour = Colours::major.brighter (0.1f);
                break;
        }

        if (twoLines)
        {
            const float mid = text.getCentreY();
            g.setFont (font (12.5f, 600));
            g.setColour (titleColour);
            g.drawText (title, text.withBottom (mid + 1.0f).withTrimmedTop (1.0f), juce::Justification::bottomLeft, true);
            g.setFont (font (11.0f));
            g.setColour (detailColour);
            g.drawText (detail, text.withTop (mid + 2.0f), juce::Justification::topLeft, true);
        }
        else
        {
            g.setFont (font (12.5f, 600));
            g.setColour (titleColour);
            g.drawText (title, text, juce::Justification::centredLeft, true);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (isLeftClick (e))
            actions.browseForReference();
    }

private:
    juce::Rectangle<float> iconArea() const
    {
        return juce::Rectangle<float> (12.0f, ((float) getHeight() - 18.0f) * 0.5f, 18.0f, 18.0f);
    }

    void paintIcon (juce::Graphics& g, juce::Rectangle<float> box) const
    {
        const auto c = box.getCentre();

        if (info.state == RefState::Loading || info.analysing)
        {
            // Spinner: a 270 degree arc rotating with time.
            const float angle = animationPhase (1000) * juce::MathConstants<float>::twoPi;
            juce::Path track, arc;
            track.addCentredArc (c.x, c.y, 7.0f, 7.0f, 0.0f, 0.0f, juce::MathConstants<float>::twoPi, true);
            arc.addCentredArc (c.x, c.y, 7.0f, 7.0f, angle, 0.0f, 4.2f, true);
            g.setColour (Colours::outline.brighter (0.1f));
            g.strokePath (track, juce::PathStrokeType (2.0f));
            g.setColour (Colours::reference);
            g.strokePath (arc, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            return;
        }

        if (info.state == RefState::Error)
        {
            g.setColour (Colours::major.withAlpha (0.18f));
            g.fillEllipse (box);
            g.setColour (Colours::major);
            g.drawEllipse (box.reduced (0.7f), 1.4f);
            g.setFont (font (12.0f, 700));
            g.drawText ("!", box.translated (0.0f, 0.5f), juce::Justification::centred, false);
            return;
        }

        // Waveform glyph: five rounded bars, lavender when a reference is loaded.
        const float heights[] = { 0.45f, 0.85f, 1.0f, 0.65f, 0.35f };
        g.setColour (info.state == RefState::Ready ? Colours::reference : Colours::textFaint.brighter (0.2f));

        for (int i = 0; i < 5; ++i)
        {
            const float h = box.getHeight() * heights[i];
            g.fillRoundedRectangle (juce::Rectangle<float> (2.2f, h).withCentre ({ box.getX() + 1.5f + 3.7f * (float) i, c.y }), 1.1f);
        }
    }

    void updateTooltip()
    {
        switch (info.state)
        {
            case RefState::Empty:
                setTooltip ("Load a reference: a finished, released song in the same genre whose sound you like. "
                            "Click to browse, or drop an audio file anywhere on RefMatch.");
                break;
            case RefState::Loading:
                setTooltip ("Loading the reference. The previous reference (if any) keeps playing until it is ready.");
                break;
            case RefState::Ready:
                setTooltip (info.file.getFullPathName() + "\n\nClick to load a different reference.");
                break;
            case RefState::Error:
                setTooltip ("This file could not be loaded: " + info.error
                            + (info.file != juce::File() ? "\n" + info.file.getFullPathName() : juce::String())
                            + "\n\nClick to choose another file.");
                break;
        }
    }

    EditorActions& actions;
    RefMatchProcessor::ReferenceInfo info;
    IconButton remove { "Remove reference", IconButton::Icon::close };
};
} // namespace

//==============================================================================
struct HeaderBar::Impl
{
    Impl (HeaderBar& o, EditorActions& a) : owner (o), actions (a), chip (a)
    {
        load.setButtonText ("Load");
        load.setTooltip (juce::String ("Choose a reference track (") + kSupportedFormatsText + ")");
        load.onClick = [this] { actions.browseForReference(); };

        abSwitch.onChoose = [this] (bool reference)
        {
            actions.setParameter (RefMatchProcessor::kParamListen, reference ? 1.0f : 0.0f);
        };

        matchChip.setTitle ("Level match");
        matchChip.setTooltip ("Level match: turns the reference up or down so it is as loud as your mix. Without it the "
                              "louder track always sounds 'better', which fools your ears.\n\n"
                              "Integrated: matches the average loudness of everything you played since the last "
                              "reset (recommended). Press reset when you move to another part of the song.\n"
                              "Short-term: follows the last 3 seconds, useful when you loop one section.\n"
                              "Off: the reference plays at its own level.");
        matchChip.onOpenMenu = [this] (MenuChip& c) { showMatchMenu (c); };

        mono.setTooltip ("Mono check: plays the output in mono (left + right summed), as many club systems and phone "
                         "speakers do. Parts that get weaker or disappear in mono are too wide or out of phase. Turn it "
                         "off again when you are done - it stays on until you do, even after reopening the project.");
        mono.onClick = [this]
        {
            actions.setParameter (RefMatchProcessor::kParamMono, mono.getToggleState() ? 1.0f : 0.0f);
        };

        reset.setTooltip ("Reset analysis: forget what RefMatch has measured of your mix so far and start again. "
                          "Do this after changing your mix or moving to another part of the song, then play it again.");
        reset.onClick = [this] { actions.resetAnalysis(); };

        options.setTooltip ("Options");
        options.onClick = [this] { showOptionsMenu(); };

        help.setTooltip ("How to use RefMatch");
        help.onClick = [this] { actions.toggleHelp(); };

        for (juce::Component* c : std::initializer_list<juce::Component*> { &chip, &load, &abSwitch, &matchChip, &mono,
                                                                            &reset, &options, &help })
            owner.addAndMakeVisible (c);
    }

    void showMatchMenu (MenuChip& target)
    {
        juce::PopupMenu menu;
        menu.addSectionHeader ("Level match");
        const char* hints[] = { "own level", "since reset", "last 3 s" };

        for (int i = 0; i < 3; ++i)
        {
            juce::PopupMenu::Item item (matchModeName ((rm::MatchMode) i));
            item.itemID = i + 1;
            item.isTicked = (int) menuState.matchMode == i;
            item.shortcutKeyDescription = hints[i];
            menu.addItem (std::move (item));
        }

        target.setMenuOpen (true);
        menu.setLookAndFeel (&owner.getLookAndFeel());
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target).withMinimumWidth (target.getWidth()),
                            [safe = juce::Component::SafePointer<HeaderBar> (&owner), this] (int result)
                            {
                                if (safe == nullptr)
                                    return;

                                matchChip.setMenuOpen (false);

                                if (result > 0)
                                    actions.setParameter (RefMatchProcessor::kParamMatch, (float) (result - 1));
                            });
    }

    void showOptionsMenu()
    {

        juce::PopupMenu menu;
        menu.addSectionHeader ("Reference playback");
        menu.addItem (juce::PopupMenu::Item ("Play reference when FL is stopped").setID (1).setTicked (menuState.freeRun));

        menu.addSectionHeader ("Analysis window");
        {
            juce::PopupMenu::Item recent ("Recent");
            recent.itemID = 2;
            recent.isTicked = menuState.averaging == rm::Averaging::Recent;
            recent.shortcutKeyDescription = recentWindowText();
            menu.addItem (std::move (recent));

            juce::PopupMenu::Item all ("Since reset");
            all.itemID = 3;
            all.isTicked = menuState.averaging == rm::Averaging::SinceReset;
            all.shortcutKeyDescription = "everything played";
            menu.addItem (std::move (all));
        }

        menu.addSeparator();
        menu.addItem (juce::PopupMenu::Item ("Reset analysis").setID (4));
        menu.addItem (juce::PopupMenu::Item ("Remove reference").setID (5).setEnabled (menuState.hasReference));

        options.setToggleState (true, juce::dontSendNotification);
        menu.setLookAndFeel (&owner.getLookAndFeel());
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&options),
                            [safe = juce::Component::SafePointer<HeaderBar> (&owner), this] (int result)
                            {
                                if (safe == nullptr)
                                    return;

                                options.setToggleState (false, juce::dontSendNotification);

                                switch (result)
                                {
                                    case 1: actions.setParameter (RefMatchProcessor::kParamFreeRun, menuState.freeRun ? 0.0f : 1.0f); break;
                                    case 2: actions.setParameter (RefMatchProcessor::kParamAveraging, (float) rm::Averaging::Recent); break;
                                    case 3: actions.setParameter (RefMatchProcessor::kParamAveraging, (float) rm::Averaging::SinceReset); break;
                                    case 4: actions.resetAnalysis(); break;
                                    case 5: actions.removeReference(); break;
                                    default: break;
                                }
                            });
    }

    static ABSwitch::State switchState (const ViewData& data, float listenPosition)
    {
        ABSwitch::State s;
        s.referenceAvailable = referencePlayable (data);
        s.listenReference = hearingReference (data);   // with no track to play the mix is heard, whatever Listen says
        s.position = listenPosition;
        s.mixSubLabel = s.listenReference ? "not heard" : "your track";   // not "muted": its meters keep running

        if (! s.referenceAvailable)
            s.referenceSubLabel = data.referenceInfo.state == RefState::Loading ? "loading..." : "load a track first";
        else if (data.listenReference && ! data.referenceSounding && ! data.hostPlaying && ! data.freeRun)
            s.referenceSubLabel = "silent - press play";
        else if (data.matchMode == rm::MatchMode::Off)
            s.referenceSubLabel = "own level";
        else
            s.referenceSubLabel = matchGainText (data.matchGainDb);

        return s;
    }

    void paintBrand (juce::Graphics& g) const
    {
        auto area = brandArea;
        drawLogo (g, area.removeFromLeft (30).withSizeKeepingCentre (30, 30).toFloat());

        if (! showWordmark)
            return;

        auto textArea = area.withTrimmedLeft (9).toFloat();
        const auto f = font (19.0f, 700);
        g.setFont (f);
        g.setColour (Colours::text);
        g.drawText ("Ref", textArea, juce::Justification::centredLeft, false);
        g.setColour (Colours::mix);
        g.drawText ("Match", textArea.withTrimmedLeft (textWidth (f, "Ref")), juce::Justification::centredLeft, false);
    }

    HeaderBar& owner;
    EditorActions& actions;

    // What the menus show (copied from the latest ViewData).
    struct MenuState
    {
        rm::MatchMode matchMode = rm::MatchMode::Integrated;
        rm::Averaging averaging = rm::Averaging::Recent;
        bool freeRun = true, hasReference = false;
    } menuState;

    ReferenceChip chip;
    LeftClickOnly<juce::TextButton> load;
    ABSwitch abSwitch;
    MenuChip matchChip;
    LeftClickOnly<juce::ToggleButton> mono { "Mono" };
    IconButton reset { "Reset analysis", IconButton::Icon::reset };
    IconButton options { "Options", IconButton::Icon::options };
    IconButton help { "Help", IconButton::Icon::help };

    juce::Rectangle<int> brandArea;
    bool showWordmark = true;
    std::vector<int> separators;
};

//==============================================================================
HeaderBar::HeaderBar (EditorActions& actions) : impl (std::make_unique<Impl> (*this, actions))
{
    setTitle ("Header");
    impl->matchChip.setText ("Match", matchModeName (rm::MatchMode::Integrated));
}

HeaderBar::~HeaderBar() = default;

ABSwitch::State HeaderBar::switchStateFor (const ViewData& data, float listenPosition)
{
    return Impl::switchState (data, listenPosition);
}

void HeaderBar::setData (const ViewData& data, float listenPosition)
{
    auto& d = *impl;
    const bool matchChanged = data.matchMode != d.menuState.matchMode;
    d.menuState = { data.matchMode, data.averaging, data.freeRun, data.referenceInfo.state != RefState::Empty };

    d.chip.setInfo (data.referenceInfo);
    d.abSwitch.setState (Impl::switchState (data, listenPosition));
    d.matchChip.setText ("Match", matchModeName (data.matchMode));
    d.mono.setToggleState (data.mono, juce::dontSendNotification);

    if (matchChanged)
        resized();
}

void HeaderBar::setHelpOpen (bool open)
{
    impl->help.setToggleState (open, juce::dontSendNotification);
}

void HeaderBar::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setGradientFill (juce::ColourGradient::vertical (juce::Colour (0xff222a33), b.getY(), juce::Colour (0xff1d232b), b.getBottom()));
    g.fillRect (b);

    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRect (b.withTop (b.getBottom() - 1.0f));
    g.setColour (juce::Colours::white.withAlpha (0.03f));
    g.fillRect (b.withHeight (1.0f));

    for (int x : impl->separators)
    {
        g.setColour (Colours::outline.withAlpha (0.8f));
        g.fillRect (juce::Rectangle<float> ((float) x, b.getY() + 16.0f, 1.0f, b.getHeight() - 32.0f));
    }

    impl->paintBrand (g);
}

void HeaderBar::resized()
{
    auto& d = *impl;
    auto r = getLocalBounds().reduced (16, 0);
    const int h = 30;
    const int iconW = 28;
    int groupGap = 12;   // each side of a separator

    const int wordmarkW = (int) std::ceil (textWidth (font (19.0f, 700), "RefMatch"));
    int brandW = 30 + 9 + wordmarkW;
    int abW = 244;
    const int abH = juce::jmin (getHeight() - 8, 50);
    const int loadW = 54;
    const int chipMinW = 190, chipMaxW = 360;
    const int matchW = d.matchChip.getIdealWidth();

    d.mono.setSize (10, 24);
    d.mono.changeWidthToFitText();
    const int monoW = d.mono.getWidth();

    auto required = [&]
    {
        return brandW + 2 * groupGap + chipMinW + 6 + loadW + 14 + abW + 14 + matchW + 12 + monoW + 2 * groupGap
               + 3 * iconW + 2 * 4;
    };

    if (required() > r.getWidth())
        abW = 228;

    d.showWordmark = required() <= r.getWidth();

    if (! d.showWordmark)
        brandW = 30;

    if (required() > r.getWidth())
        groupGap = 9;

    if (required() > r.getWidth())
        abW = juce::jmax (200, abW - (required() - r.getWidth()));

    auto centred = [] (juce::Rectangle<int> area, int height) { return area.withSizeKeepingCentre (area.getWidth(), height); };
    d.separators.clear();

    // Left: brand | chip + Load
    d.brandArea = r.removeFromLeft (brandW);
    r.removeFromLeft (groupGap);
    d.separators.push_back (r.getX());
    r.removeFromLeft (groupGap);

    // Right: help, options, reset | Mono, Match
    d.help.setBounds (centred (r.removeFromRight (iconW), 28));
    r.removeFromRight (4);
    d.options.setBounds (centred (r.removeFromRight (iconW), 28));
    r.removeFromRight (4);
    d.reset.setBounds (centred (r.removeFromRight (iconW), 28));
    r.removeFromRight (groupGap);
    d.separators.push_back (r.getRight());
    r.removeFromRight (groupGap);
    d.mono.setBounds (centred (r.removeFromRight (monoW), 24));
    r.removeFromRight (12);
    d.matchChip.setBounds (centred (r.removeFromRight (matchW), h));
    r.removeFromRight (14);

    // Middle: the chip takes the spare space (up to its maximum), then the A/B switch, centred in the editor when
    // there is room for that.
    const int chipW = juce::jlimit (chipMinW, chipMaxW, r.getWidth() - (6 + loadW + 14 + abW));
    d.chip.setBounds (centred (r.withWidth (chipW), 38));
    d.load.setBounds (centred (r.withX (r.getX() + chipW + 6).withWidth (loadW), h));

    const int minAbX = d.load.getRight() + 14;
    const int abX = juce::jlimit (minAbX, juce::jmax (minAbX, r.getRight() - abW), getWidth() / 2 - abW / 2);
    d.abSwitch.setBounds (abX, (getHeight() - abH) / 2, abW, abH);
}

} // namespace rm::ui
