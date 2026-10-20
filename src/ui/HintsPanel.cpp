#include "ui/HintsPanel.h"

namespace rm::ui
{
namespace
{
using RefState = RefMatchProcessor::ReferenceState;

constexpr float kGap = 8.0f;            // between cards
constexpr float kCompactRowH = 30.0f;   // collapsed hint: one line (stripe, title, tag, chevron)
constexpr float kChevronW = 18.0f;
constexpr int kTwoColumnMinWidth = 520, kTwoColumnMinViewport = 140;

struct HintText
{
    rm::Severity severity = rm::Severity::Info;
    rm::Topic topic = rm::Topic::Setup;
    int band = -1;
    float amount = 0.0f;
    juce::String title, detail;

    bool operator== (const HintText& o) const
    {
        return severity == o.severity && topic == o.topic && band == o.band && title == o.title && detail == o.detail;
    }

    // Identifies "the same hint" while its numbers change (titles carry the measured values): the expanded state of a
    // collapsed row follows it.
    juce::String key() const
    {
        juce::String k;
        k << (int) topic << ":" << band << ":" << (amount < 0.0f ? "-" : "+");

        if (topic == rm::Topic::Setup)
            k << ":" << title;

        return k;
    }
};

HintText fromHint (const rm::Hint& h)
{
    return { h.severity, h.topic, h.band, h.amount, juce::String (h.title), juce::String (h.detail) };
}

// Checks of the mix alone (not differences from the reference): true peak, low-end mono, and the broadband phase
// check (its amount is the correlation, -1..0; the "mix is mono" hint's amount is -reference width, <= -20).
bool isMixOnlyCheck (const HintText& h)
{
    return h.topic == rm::Topic::TruePeak || h.topic == rm::Topic::LowEnd
           || (h.topic == rm::Topic::Stereo && h.band < 0 && h.amount > -1.01f);
}

juce::String severityTag (const HintText& h)
{
    switch (h.severity)
    {
        case rm::Severity::Good:  return "GOOD";
        case rm::Severity::Info:  return "TIP";
        case rm::Severity::Minor: return "WORTH A LOOK";
        case rm::Severity::Major: return isMixOnlyCheck (h) ? "CHECK THIS" : "BIG DIFFERENCE";
    }

    return {};
}

// Title of the core's "play your track" setup hint (the other setup hints are about the reference). Taken from the
// core itself, so the two can never drift apart.
const std::string& mixSetupHintTitle()
{
    static const std::string title = []
    {
        rm::TrackProfile validReference;
        validReference.valid = true;

        for (const auto& h : rm::compareProfiles (rm::TrackProfile(), validReference).hints)
            if (h.topic == rm::Topic::Setup)
                return h.title;

        return std::string();
    }();

    return title;
}

// The hints to show: the comparison's own (or, before the first comparison is published, the core's hints for the
// profiles at hand). While a reference is being loaded or analysed for the first time, the core's "Load a reference
// track" (it only sees an empty reference profile) becomes "Analysing the reference".
std::vector<HintText> hintsFor (const ViewData& data)
{
    const auto fallback = data.comparison.hints.empty() ? rm::compareProfiles (data.mix, data.reference) : rm::Comparison();
    const auto& source = data.comparison.hints.empty() ? fallback.hints : data.comparison.hints;

    const bool referencePending = (data.referenceInfo.state == RefState::Loading || data.referenceInfo.analysing)
                                  && ! data.reference.valid && data.reference.analysedSeconds <= 0.0;

    std::vector<HintText> out;
    bool pendingShown = false;

    for (const auto& h : source)
    {
        if (referencePending && h.topic == rm::Topic::Setup && h.title != mixSetupHintTitle())
        {
            if (! pendingShown)
                out.push_back ({ rm::Severity::Info, rm::Topic::Setup, -1, 0.0f, "Analysing the reference",
                                 "Loading and measuring the reference takes a few seconds." });

            pendingShown = true;
            continue;
        }

        out.push_back (fromHint (h));
    }

    return out;
}

//==============================================================================
// The hint cards. The first (most important) hint is a full-width card with larger text. The others are cards in one
// or two columns when they all fit in the visible area; otherwise they are one-line rows (title + tag) that show their
// advice as a tooltip and open into a card on a left click.
class HintList final : public juce::Component, public juce::TooltipClient
{
public:
    HintList()
    {
        setTitle ("Hint cards");
        setInterceptsMouseClicks (true, false);
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    // A row was opened (its index) or closed (-1): the owner re-lays out (scroll bar) and scrolls it into view.
    std::function<void (int openedIndex)> onLayoutChange;

    bool setHints (std::vector<HintText> newHints)
    {
        if (newHints == hints)
            return false;

        hints = std::move (newHints);
        hoverCard = -1;   // (the next mouse move finds it again)
        layout (getWidth(), viewportHeight, fitWidth);
        repaint();
        return true;
    }

    // Lays the cards out for a width and resizes to the content height. Whether the cards fit in the visible height
    // (and which rows open by themselves) is decided at `decisionWidth` (the width left with a scroll bar), so the
    // result is the same whether or not the owner then needs a scroll bar.
    void layout (int width, int visibleHeight, int decisionWidth)
    {
        viewportHeight = visibleHeight;
        fitWidth = decisionWidth;
        const auto open = chooseOpenRows (visibleHeight, (float) juce::jmax (100, juce::jmin (width, decisionWidth)));
        cards = buildCards ((float) juce::jmax (100, width), visibleHeight, open);
        const float bottom = cards.empty() ? 0.0f : cards.back().bounds.getBottom();
        setSize (width, juce::jmax (1, (int) std::ceil (bottom)));
    }

    void paint (juce::Graphics& g) override
    {
        for (size_t i = 0; i < cards.size(); ++i)
            paintCard (g, cards[i], (int) i == hoverCard);
    }

    void mouseMove (const juce::MouseEvent& e) override   { setHover (cardAt (e.position)); }
    void mouseExit (const juce::MouseEvent&) override      { setHover (-1); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! isLeftClick (e) || ! e.mouseWasClicked())
            return;

        const int index = cardAt (e.position);

        if (index < 0 || cards[(size_t) index].style == Style::lead || cards[(size_t) index].style == Style::card)
            return;

        const auto key = cards[(size_t) index].key;
        const bool opening = cards[(size_t) index].style == Style::row;

        // The user's choice wins over opening rows by themselves.
        openKeys.removeString (key);
        closedKeys.removeString (key);
        (opening ? openKeys : closedKeys).add (key);

        layout (getWidth(), viewportHeight, fitWidth);
        repaint();

        if (onLayoutChange != nullptr)
            onLayoutChange (opening ? index : -1);
    }

    juce::String getTooltip() override
    {
        const int index = cardAt (getMouseXYRelative().toFloat());

        if (index < 0)
            return {};

        const auto& card = cards[(size_t) index];

        if (card.style == Style::row)
            return card.detailText + "\n\n(Click to open it here.)";

        return card.style == Style::opened ? juce::String ("Click to close.") : juce::String();
    }

    int numHints() const noexcept { return (int) hints.size(); }

    juce::Rectangle<float> cardBounds (int index) const
    {
        return index >= 0 && index < (int) cards.size() ? cards[(size_t) index].bounds : juce::Rectangle<float>();
    }

    // Cards not completely visible above `viewBottom` (list coordinates).
    int numCardsBelow (int viewBottom) const
    {
        int n = 0;

        for (const auto& c : cards)
            if (c.bounds.getBottom() > (float) viewBottom + 1.0f)
                ++n;

        return n;
    }

    // True when a card is cut by the line y = viewBottom (it shows partly).
    bool cardCutAt (int viewBottom) const
    {
        for (const auto& c : cards)
            if (c.bounds.getY() < (float) viewBottom - 1.0f && c.bounds.getBottom() > (float) viewBottom + 1.0f)
                return true;

        return false;
    }

    // How many pixels of the card cut by y = viewBottom are visible (the smallest, if several), or -1 if none is cut.
    float visiblePartOfCutCard (int viewBottom) const
    {
        float shown = -1.0f;

        for (const auto& c : cards)
            if (c.bounds.getY() < (float) viewBottom - 1.0f && c.bounds.getBottom() > (float) viewBottom + 1.0f)
                shown = shown < 0.0f ? (float) viewBottom - c.bounds.getY()
                                     : juce::jmin (shown, (float) viewBottom - c.bounds.getY());

        return shown;
    }

    juce::StringArray describe() const
    {
        juce::StringArray lines;

        for (const auto& h : hints)
            lines.add (severityTag (h) + " | " + h.title);

        return lines;
    }

    static juce::Font tagFont() { return font (9.5f, 700).withExtraKerningFactor (0.06f); }

private:
    enum class Style
    {
        lead,     // the most important hint: full width, larger text
        card,     // title + advice
        row,      // collapsed: one line, advice in the tooltip, opens on click
        opened    // an open row (opened by the user, or by itself while there is room): like a card, closes on click
    };

    struct Card
    {
        Style style = Style::card;
        rm::Severity severity = rm::Severity::Info;
        juce::String tag, key, title, detailText;
        juce::TextLayout titleLayout, detail;
        juce::Rectangle<float> bounds, titleArea, detailArea;
    };

    // Which of hints[1..] are shown as open cards when they are one-line rows: the ones the user opened, then (top
    // down, while they fit in the visible height) the ones the user has not closed. Empty = they all fit as cards.
    std::vector<bool> chooseOpenRows (int visibleHeight, float w) const
    {
        if (visibleHeight <= 0 || hints.size() <= 1)
            return {};

        const auto asCards = buildCards (w, visibleHeight, {});

        if ((int) std::ceil (asCards.back().bounds.getBottom()) <= visibleHeight)
            return {};

        std::vector<bool> open (hints.size(), false);
        float height = makeCard (hints.front(), Style::lead, 0.0f, 0.0f, w).bounds.getHeight();
        std::vector<float> openedH (hints.size(), 0.0f);

        for (size_t i = 1; i < hints.size(); ++i)
        {
            open[i] = openKeys.contains (hints[i].key());
            openedH[i] = makeCard (hints[i], Style::opened, 0.0f, 0.0f, w).bounds.getHeight();
            height += kGap + (open[i] ? openedH[i] : kCompactRowH);
        }

        for (size_t i = 1; i < hints.size(); ++i)
        {
            if (open[i] || closedKeys.contains (hints[i].key()))
                continue;

            const float extra = openedH[i] - kCompactRowH;

            if ((int) std::ceil (height + extra) > visibleHeight)
                break;

            open[i] = true;
            height += extra;
        }

        return open;
    }

    // The cards: the lead card, then either cards in columns (openRows empty) or rows / opened cards in one column.
    std::vector<Card> buildCards (float w, int visibleHeight, const std::vector<bool>& openRows) const
    {
        std::vector<Card> out;

        if (hints.empty())
            return out;

        out.push_back (makeCard (hints.front(), Style::lead, 0.0f, 0.0f, w));
        float y = out.back().bounds.getBottom() + kGap;

        if (! openRows.empty())
        {
            for (size_t i = 1; i < hints.size(); ++i)
            {
                out.push_back (makeCard (hints[i], openRows[i] ? Style::opened : Style::row, 0.0f, y, w));
                y = out.back().bounds.getBottom() + kGap;
            }

            return out;
        }

        const int columns = (w >= (float) kTwoColumnMinWidth && visibleHeight >= kTwoColumnMinViewport) ? 2 : 1;
        const float cardW = (w - kGap * (float) (columns - 1)) / (float) columns;

        for (size_t i = 1; i < hints.size(); i += (size_t) columns)
        {
            float rowH = 0.0f;
            const size_t rowStart = out.size();
            const bool alone = i + 1 >= hints.size();   // a last row with a single card uses the full width

            for (int c = 0; c < columns && i + (size_t) c < hints.size(); ++c)
            {
                out.push_back (makeCard (hints[i + (size_t) c], Style::card, (cardW + kGap) * (float) c, y, alone ? w : cardW));
                rowH = juce::jmax (rowH, out.back().bounds.getHeight());
            }

            for (size_t k = rowStart; k < out.size(); ++k)
                out[k].bounds.setHeight (rowH);

            y += rowH + kGap;
        }

        return out;
    }

    static Card makeCard (const HintText& h, Style style, float x, float y, float cardW)
    {
        Card card;
        card.style = style;
        card.severity = h.severity;
        card.tag = severityTag (h);
        card.key = h.key();
        card.title = h.title;
        card.detailText = h.detail;

        const bool lead = style == Style::lead;
        const bool collapsible = style == Style::row || style == Style::opened;
        const float tagW = textWidth (tagFont(), card.tag) + 8.0f + (collapsible ? kChevronW : 0.0f);
        const float textX = lead ? 17.0f : 16.0f, textW = cardW - textX - 10.0f;

        if (style == Style::row)
        {
            card.titleArea = { textX, 0.0f, juce::jmax (20.0f, textW - tagW), kCompactRowH };
            card.bounds = { x, y, cardW, kCompactRowH };
            return card;
        }

        juce::AttributedString title;
        title.setWordWrap (juce::AttributedString::byWord);
        title.append (h.title, font (lead ? 14.5f : 13.0f, 600), Colours::text);
        card.titleLayout.createLayout (title, textW - tagW);

        juce::AttributedString detail;
        detail.setWordWrap (juce::AttributedString::byWord);
        detail.setLineSpacing (1.5f);
        detail.append (h.detail, font (lead ? 12.0f : 11.5f), Colours::textDim.brighter (lead ? 0.2f : 0.12f));
        card.detail.createLayout (detail, textW);

        const float top = lead ? 10.0f : 8.0f;
        card.titleArea = { textX, top, textW - tagW, card.titleLayout.getHeight() };
        card.detailArea = { textX, card.titleArea.getBottom() + (lead ? 4.0f : 3.0f), textW, card.detail.getHeight() };
        card.bounds = { x, y, cardW, card.detailArea.getBottom() + (lead ? 11.0f : 9.0f) };
        return card;
    }

    void paintCard (juce::Graphics& g, const Card& card, bool hovered) const
    {
        const auto b = card.bounds;
        const auto colour = severityColour (card.severity);
        const bool lead = card.style == Style::lead;
        const bool collapsible = card.style == Style::row || card.style == Style::opened;

        g.setColour ((hovered && collapsible ? Colours::panelHover : Colours::panelRaised).withAlpha (lead ? 0.8f : 0.55f));
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour ((lead ? colour.withAlpha (0.35f) : Colours::outline.withAlpha (hovered && collapsible ? 0.9f : 0.5f)));
        g.drawRoundedRectangle (b.reduced (0.5f), 6.0f, 1.0f);

        // Severity stripe
        const float stripeInset = card.style == Style::row ? 7.0f : 6.0f;
        g.setColour (colour);
        g.fillRoundedRectangle (juce::Rectangle<float> (b.getX() + 1.0f, b.getY() + stripeInset, lead ? 4.0f : 3.0f,
                                                        b.getHeight() - 2.0f * stripeInset), 1.5f);

        // Tag (and the open / close chevron of a collapsible hint), top right
        const float tagRight = b.getRight() - 10.0f - (collapsible ? kChevronW : 0.0f);
        const float tagY = card.style == Style::row ? b.getY() : b.getY() + 8.0f;
        const float tagH = card.style == Style::row ? kCompactRowH : 16.0f;
        g.setFont (tagFont());
        g.setColour (colour.withAlpha (0.85f));
        g.drawText (card.tag, juce::Rectangle<float> (tagRight - 120.0f, tagY, 120.0f, tagH), juce::Justification::centredRight, false);

        if (collapsible)
        {
            const float cx = b.getRight() - 10.0f - kChevronW * 0.5f + 2.0f, cy = tagY + tagH * 0.5f;
            const float dir = card.style == Style::opened ? -1.0f : 1.0f;
            juce::Path chevron;
            chevron.startNewSubPath (cx - 3.5f, cy - 1.6f * dir);
            chevron.lineTo (cx, cy + 1.8f * dir);
            chevron.lineTo (cx + 3.5f, cy - 1.6f * dir);
            g.setColour (hovered ? Colours::text : Colours::textDim);
            g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        if (card.style == Style::row)
        {
            g.setFont (font (12.5f, 600));
            g.setColour (Colours::text.withAlpha (hovered ? 1.0f : 0.92f));
            drawFittedLine (g, card.title, card.titleArea.translated (b.getX(), b.getY()), juce::Justification::centredLeft, 0.9f);
            return;
        }

        card.titleLayout.draw (g, card.titleArea.translated (b.getX(), b.getY()));
        card.detail.draw (g, card.detailArea.translated (b.getX(), b.getY()));
    }

    int cardAt (juce::Point<float> p) const
    {
        for (size_t i = 0; i < cards.size(); ++i)
            if (cards[i].bounds.contains (p))
                return (int) i;

        return -1;
    }

    void setHover (int index)
    {
        if (index == hoverCard)
            return;

        auto area = [this] (int i) { return i >= 0 && i < (int) cards.size() ? cards[(size_t) i].bounds.expanded (1.0f).getSmallestIntegerContainer()
                                                                               : juce::Rectangle<int>(); };
        repaint (area (hoverCard));
        hoverCard = index;
        repaint (area (hoverCard));
    }

    std::vector<HintText> hints;
    std::vector<Card> cards;
    juce::StringArray openKeys, closedKeys;   // rows the user opened / closed (by HintText::key)
    int viewportHeight = 0, fitWidth = 0;
    int hoverCard = -1;
};

// Viewport that reports scrolling (the panel redraws its "more below" count and fade).
class HintsViewport final : public juce::Viewport
{
public:
    std::function<void()> onScroll;

    void visibleAreaChanged (const juce::Rectangle<int>&) override
    {
        if (onScroll != nullptr)
            onScroll();
    }
};
} // namespace

//==============================================================================
struct HintsPanel::Impl
{
    HintList list;
    HintsViewport viewport;
    juce::Rectangle<int> titleRow;
};

HintsPanel::HintsPanel() : impl (std::make_unique<Impl>())
{
    setTitle ("What to try");
    impl->viewport.setViewedComponent (&impl->list, false);
    impl->viewport.setScrollBarsShown (true, false);
    impl->viewport.setScrollBarThickness (8);
    impl->viewport.setWantsKeyboardFocus (false);
    impl->viewport.onScroll = [this]
    {
        repaint (impl->titleRow);
        repaint (impl->viewport.getBounds());
    };
    impl->list.onLayoutChange = [this] (int opened)
    {
        resized();
        repaint();

        // Show all of a card that was just opened (as much as fits, from its top).
        auto& vp = impl->viewport;
        const auto card = impl->list.cardBounds (opened).getSmallestIntegerContainer();

        if (! card.isEmpty() && card.getBottom() > vp.getViewPositionY() + vp.getViewHeight())
            vp.setViewPosition (0, juce::jmin (card.getY(), card.getBottom() - vp.getViewHeight()));
    };
    addAndMakeVisible (impl->viewport);
}

HintsPanel::~HintsPanel() = default;

void HintsPanel::setData (const ViewData& data)
{
    if (impl->list.setHints (hintsFor (data)))
    {
        resized();
        repaint();
    }
}

juce::StringArray HintsPanel::describeHints (const ViewData& data)
{
    HintList list;
    list.setHints (hintsFor (data));
    return list.describe();
}

juce::Rectangle<int> HintsPanel::getCardBoundsForTesting (int index) const
{
    const auto b = impl->list.cardBounds (index);
    return b.isEmpty() ? juce::Rectangle<int>() : getLocalArea (&impl->list, b.getSmallestIntegerContainer());
}

void HintsPanel::resized()
{
    auto inner = getLocalBounds().reduced (14, 10);
    impl->titleRow = inner.removeFromTop (20);
    inner.removeFromTop (6);
    impl->viewport.setBounds (inner);

    // Leave room for the scroll bar only when it is needed (what fits is decided for the narrower width either way).
    const int narrow = inner.getWidth() - impl->viewport.getScrollBarThickness() - 4;
    impl->list.layout (inner.getWidth(), inner.getHeight(), narrow);

    if (impl->list.getHeight() > inner.getHeight())
        impl->list.layout (narrow, inner.getHeight(), narrow);
}

void HintsPanel::paintOverChildren (juce::Graphics& g)
{
    // A short fade where a card is cut at the bottom of the visible area (never over a card that is fully visible).
    const auto& vp = impl->viewport;
    const int viewBottom = vp.getViewPositionY() + vp.getViewHeight();

    if (! impl->list.cardCutAt (viewBottom))
        return;

    const auto area = vp.getBounds().toFloat().withTrimmedRight ((float) vp.getScrollBarThickness() + 2.0f);

    // Only a few pixels of the next card would show: hide them completely, a thin sliver reads like a stray line.
    const float shown = impl->list.visiblePartOfCutCard (viewBottom);

    if (shown >= 0.0f && shown < 14.0f)
    {
        g.setColour (Colours::panel);
        g.fillRect (area.withTop (area.getBottom() - shown - 1.0f));
        return;
    }

    const auto fade = area.withTop (area.getBottom() - 16.0f);
    g.setGradientFill (juce::ColourGradient::vertical (Colours::panel.withAlpha (0.0f), fade.getY(), Colours::panel, fade.getBottom()));
    g.fillRect (fade);
}

void HintsPanel::paint (juce::Graphics& g)
{
    drawPanel (g, getLocalBounds().toFloat());
    drawSectionTitle (g, "What to try", impl->titleRow);

    // How many tips there are, and how many are still below (scroll for them).
    const int total = impl->list.numHints();

    if (total < 2)
        return;

    const auto& vp = impl->viewport;
    const int below = impl->list.numCardsBelow (vp.getViewPositionY() + vp.getViewHeight());
    juce::String text (juce::String (total) + " tips");

    if (below > 0)
        text << " - " << below << " more below";

    g.setFont (font (11.0f, below > 0 ? 600 : 400));
    g.setColour (below > 0 ? Colours::textDim : Colours::textSubtle);
    g.drawText (text, impl->titleRow, juce::Justification::centredRight, false);
}

} // namespace rm::ui
