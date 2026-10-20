#include "ui/HeaderBar.h"
#include "ui/HintsPanel.h"
#include "ui/LoudnessPanel.h"
#include "ui/PluginEditor.h"
#include "ui/TonalBalancePanel.h"
#include "ui/Widgets.h"

#include <iostream>

// Renders the editor in several states to PNG files (owner: ui agent).
//   RefMatchDev snapshot <outDir>
//
// Most states use hand-built ("synthetic") ViewData pushed through RefMatchEditor::setViewDataOverride, so every
// panel can be checked in states that are hard to reach with real audio (their comparisons and hints come from the real
// rm::compareProfiles, so the cards show the real wording). One snapshot uses the real processor. Before the images,
// a few UI logic checks run (hint cards, loudness one-liners, band words, A/B labels, zoom); any failure makes the
// command fail.

namespace juce::detail
{
// Defined in juce_events (juce_Messaging_windows.cpp / _linux.cpp); lets this console tool pump the message queue so
// async callbacks run as they would inside a host.
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);
}

namespace
{
using RefState = RefMatchProcessor::ReferenceState;

void pumpMessages (int milliseconds)
{
   #if JUCE_WINDOWS || JUCE_LINUX
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) milliseconds;

    while (juce::Time::getMillisecondCounter() < end)
        if (! juce::detail::dispatchNextMessageOnSystemQueue (true))
            juce::Thread::sleep (2);
   #else
    juce::ignoreUnused (milliseconds);
   #endif
}

bool writeImage (const juce::Image& image, const juce::File& file)
{
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat png;
    const bool ok = out.openedOk() && png.writeImageToStream (image, out);
    std::cout << (ok ? "wrote " : "FAILED ") << file.getFullPathName() << " (" << image.getWidth() << "x"
              << image.getHeight() << ")\n";
    return ok;
}

bool writePng (juce::Component& c, const juce::File& file)
{
    return writeImage (c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f), file);
}

//==============================================================================
// Synthetic data. Deterministic: a fixed LCG, no time, no randomness.
struct Lcg
{
    juce::uint32 state = 0x2545f491u;

    float next() noexcept   // 0..1
    {
        state = state * 1664525u + 1013904223u;
        return (float) (state >> 8) / (float) (1u << 24);
    }
};

// Typical music PSD in dB/Hz: about -4.5 dB/octave (so the editor's +4.5 dB/octave display tilt shows it roughly
// flat), a kick / bass bump, sub and top-end roll-offs and a little ripple.
float musicPsdDb (float hz, float rippleSeed)
{
    float db = -52.0f - 4.5f * std::log2 (hz / 1000.0f);
    const float oct60 = std::log2 (hz / 60.0f);
    db += 3.0f * std::exp (-oct60 * oct60 / 0.9f);

    if (hz < 35.0f)
        db -= 12.0f * std::log2 (35.0f / hz);

    if (hz > 15000.0f)
        db -= 18.0f * std::log2 (hz / 15000.0f);

    db += 0.8f * std::sin (std::log2 (hz) * 2.7f + rippleSeed) + 0.4f * std::sin (std::log2 (hz) * 6.1f + rippleSeed * 2.0f);
    return db;
}

// Mix-minus-reference tonal difference, interpolated (in log frequency) between the band centres.
float bandDifferenceAt (float hz, const std::array<float, rm::kNumBands>& diffs)
{
    std::array<float, rm::kNumBands> centres {};

    for (int b = 0; b < rm::kNumBands; ++b)
        centres[(size_t) b] = std::sqrt (rm::bandInfo (b).lowHz * rm::bandInfo (b).highHz);

    if (hz <= centres.front())
        return diffs.front();

    if (hz >= centres.back())
        return diffs.back();

    for (int b = 0; b + 1 < rm::kNumBands; ++b)
    {
        const float lo = centres[(size_t) b], hi = centres[(size_t) b + 1];

        if (hz <= hi)
        {
            const float t = std::log (hz / lo) / std::log (hi / lo);
            return diffs[(size_t) b] + (diffs[(size_t) b + 1] - diffs[(size_t) b]) * t;
        }
    }

    return diffs.back();
}

constexpr double kTrackSeconds = 204.0;   // 3:24
constexpr std::array<float, rm::kNumBands> kBandDiffs { -1.2f, 1.8f, 3.4f, 0.6f, -1.9f, -3.6f };

// Envelope of a typical club track: intro, verse, build, drop, breakdown, build, drop 2, outro.
float songEnvelope (double t)
{
    struct Section { double start, end; float from, to; };
    const Section sections[] = { { 0, 16, 0.32f, 0.38f },   { 16, 48, 0.55f, 0.58f },  { 48, 64, 0.55f, 0.82f },
                                 { 64, 96, 0.95f, 0.95f },  { 96, 128, 0.42f, 0.46f }, { 128, 144, 0.5f, 0.86f },
                                 { 144, 176, 0.97f, 0.97f }, { 176, 204, 0.62f, 0.08f } };

    for (const auto& s : sections)
        if (t < s.end)
            return s.from + (s.to - s.from) * (float) ((t - s.start) / (s.end - s.start));

    return 0.05f;
}

std::shared_ptr<const rm::ReferenceTrack> makeSyntheticTrack()
{
    auto track = std::make_shared<rm::ReferenceTrack>();
    track->file = juce::File ("C:\\Users\\Producer\\Music\\References\\Neon Skyline - Club Mix.wav");
    track->displayName = "Neon Skyline - Club Mix";
    track->fileSampleRate = 44100.0;
    track->fileChannels = 2;
    track->sampleRate = 48000.0;   // (no audio: the editor only draws the overview and the curve)

    Lcg rng;
    const int columns = rm::ReferenceTrack::kOverviewColumns;
    track->overview.minimum.resize ((size_t) columns);
    track->overview.maximum.resize ((size_t) columns);

    for (int c = 0; c < columns; ++c)
    {
        // 128 BPM: a kick transient at every beat decaying over the beat, plus some per-column variation.
        const double t = (c + 0.5) / columns * kTrackSeconds;
        const double beatPhase = std::fmod (t, 60.0 / 128.0) / (60.0 / 128.0);
        const float transient = (float) std::exp (-beatPhase * 4.0);
        const float env = songEnvelope (t) * (0.62f + 0.38f * transient);
        track->overview.maximum[(size_t) c] = juce::jlimit (0.0f, 1.0f, env * (0.7f + 0.3f * rng.next()));
        track->overview.minimum[(size_t) c] = -juce::jlimit (0.0f, 1.0f, env * (0.7f + 0.3f * rng.next()));
    }

    const int steps = (int) (kTrackSeconds * 10.0);
    track->shortTermCurve.resize ((size_t) steps, rm::kNoValue);

    for (int i = 29; i < steps; ++i)
    {
        double power = 0.0;

        for (int k = 0; k < 30; ++k)
        {
            const float e = songEnvelope ((i - k + 0.5) * 0.1);
            power += (double) e * e;
        }

        track->shortTermCurve[(size_t) i] = -7.4f + (float) (10.0 * std::log10 (power / 30.0 + 1.0e-9));
    }

    return track;
}

rm::TrackProfile makeProfile (bool isMix)
{
    rm::TrackProfile p;
    p.valid = true;
    p.analysedSeconds = isMix ? 94.2 : 32.0;

    if (isMix)
    {
        p.integratedLufs = -11.8f;  p.shortTermLufs = -10.6f;  p.momentaryLufs = -10.1f;  p.loudnessRangeLu = 5.1f;
        p.truePeakDb = 0.4f;        p.samplePeakDb = -0.1f;    p.rmsDb = -14.6f;          p.crestDb = 14.5f;
        p.plrDb = 11.7f;            p.spectralLoudnessDb = -12.6f;   // plrDb = samplePeakDb - integratedLufs
        p.correlation = 0.71f;      p.widthPct = 38.0f;        p.lowEndCorrelation = 0.62f;
    }
    else
    {
        p.integratedLufs = -7.9f;   p.shortTermLufs = -7.4f;   p.momentaryLufs = -7.1f;   p.loudnessRangeLu = 4.2f;
        p.truePeakDb = -0.3f;       p.samplePeakDb = -0.4f;    p.rmsDb = -10.4f;          p.crestDb = 10.0f;
        p.plrDb = 7.5f;             p.spectralLoudnessDb = -8.7f;
        p.correlation = 0.62f;      p.widthPct = 47.0f;        p.lowEndCorrelation = 0.97f;
    }

    const float mixWidths[] = { 4.0f, 18.0f, 31.0f, 44.0f, 58.0f, 63.0f };
    const float refWidths[] = { 1.0f, 6.0f, 24.0f, 46.0f, 66.0f, 78.0f };
    const float offset = isMix ? -3.9f : 0.0f;   // the mix is quieter: its raw curve sits lower

    for (int b = 0; b < rm::kNumBands; ++b)
    {
        auto& band = p.bands[(size_t) b];
        band.widthPct = isMix ? mixWidths[b] : refWidths[b];
        band.correlation = 1.0f - band.widthPct / 100.0f;
        band.levelDb = -20.0f + offset + (isMix ? kBandDiffs[(size_t) b] : 0.0f) - 2.0f * (float) b;
    }

    for (int i = 0; i < rm::kCurvePoints; ++i)
    {
        const float hz = rm::curveFrequency (i);
        p.curveDb[(size_t) i] = isMix ? musicPsdDb (hz, 0.9f) + bandDifferenceAt (hz, kBandDiffs) + offset
                                      : musicPsdDb (hz, 0.0f);
    }

    return p;
}

RefMatchProcessor::ReferenceInfo readyInfo (const rm::ReferenceTrack& track, bool loop)
{
    RefMatchProcessor::ReferenceInfo info;
    info.state = RefState::Ready;
    info.file = track.file;
    info.displayName = track.displayName;
    info.lengthSeconds = kTrackSeconds;
    info.fileSampleRate = track.fileSampleRate;
    info.fileChannels = track.fileChannels;
    info.hasLoop = loop;
    info.loopStart = loop ? 64.0 : 0.0;
    info.loopEnd = loop ? 96.0 : kTrackSeconds;
    info.version = loop ? 8 : 7;
    return info;
}

// The fully analysed state: mix and reference measured, the core's comparison (hints of several severities: band
// differences of kBandDiffs, true peak over -1 dBTP, bass a bit wide, quieter and more dynamic), listening to Mix.
rm::ui::ViewData makeFullViewData (const std::shared_ptr<const rm::ReferenceTrack>& track, bool loop)
{
    rm::ui::ViewData d;
    d.mix = makeProfile (true);
    d.reference = makeProfile (false);
    d.comparison = rm::compareProfiles (d.mix, d.reference);
    d.track = track;
    d.referenceInfo = readyInfo (*track, loop);
    d.playheadSeconds = 78.4;
    d.referenceShortTermLufs = rm::shortTermAt (track->shortTermCurve, d.playheadSeconds);
    d.matchGainDb = -3.9f;
    d.listenPosition = 0.0f;
    d.listenReference = false;
    d.hostPlaying = true;
    d.referenceSounding = false;
    d.matchMode = rm::MatchMode::Integrated;
    d.averaging = rm::Averaging::Recent;
    d.analysisVersion = 42;
    return d;
}

// The mix has been played, no reference: the core's hints for the mix alone ("Load a reference track" + checks).
rm::ui::ViewData makeMixOnlyViewData()
{
    rm::ui::ViewData d;
    d.mix = makeProfile (true);
    d.comparison = rm::compareProfiles (d.mix, rm::TrackProfile());
    d.hostPlaying = true;
    return d;
}

// The first reference is loading (no track yet).
rm::ui::ViewData makeLoadingViewData()
{
    auto d = makeMixOnlyViewData();
    d.referenceInfo.state = RefState::Loading;
    d.referenceInfo.displayName = "Neon Skyline - Club Mix";
    d.referenceInfo.version = 1;
    return d;
}

// The first reference is installed (Ready) but its region profile is still being analysed.
rm::ui::ViewData makeAnalysingViewData (const std::shared_ptr<const rm::ReferenceTrack>& track)
{
    auto d = makeMixOnlyViewData();
    d.track = track;
    d.referenceInfo = readyInfo (*track, false);
    d.referenceInfo.analysing = true;
    d.playheadSeconds = 12.0;
    return d;
}

//==============================================================================
// UI logic checks (hint cards, loudness one-liners, band words, A/B labels, zoom). Prints one line per check.
bool runUiChecks (const std::shared_ptr<const rm::ReferenceTrack>& track)
{
    int failed = 0, passed = 0;

    auto check = [&] (bool condition, const juce::String& name, const juce::String& detail = {})
    {
        (condition ? passed : failed)++;

        if (! condition)
            std::cout << "ui check FAILED: " << name << (detail.isNotEmpty() ? " (" + detail + ")" : juce::String()) << "\n";
    };

    auto findLine = [] (const juce::StringArray& lines, const juce::String& start)
    {
        for (const auto& l : lines)
            if (l.startsWith (start))
                return l;

        return juce::String();
    };

    // ---- Hint cards ------------------------------------------------------------------------------------------------
    {
        const auto mixOnly = rm::ui::HintsPanel::describeHints (makeMixOnlyViewData());
        check (mixOnly.size() > 0 && mixOnly[0] == "TIP | Load a reference track", "no reference: load hint first", mixOnly.joinIntoString (" / "));
        check (findLine (mixOnly, "CHECK THIS | True peak").isNotEmpty(), "true peak Major is CHECK THIS", mixOnly.joinIntoString (" / "));

        for (const auto& [name, data] : { std::make_pair (juce::String ("loading"), makeLoadingViewData()),
                                          std::make_pair (juce::String ("analysing"), makeAnalysingViewData (track)) })
        {
            const auto lines = rm::ui::HintsPanel::describeHints (data);
            check (lines.size() > 0 && lines[0] == "TIP | Analysing the reference", name + ": analysing card first", lines.joinIntoString (" / "));
            check (findLine (lines, "TIP | Load a reference").isEmpty(), name + ": no load-a-reference card", lines.joinIntoString (" / "));
            check (findLine (lines, "CHECK THIS | True peak").isNotEmpty(), name + ": mix checks kept", lines.joinIntoString (" / "));
        }

        {
            auto d = makeLoadingViewData();
            d.mix = {};   // nothing played yet either
            d.comparison = rm::compareProfiles (d.mix, d.reference);
            const auto lines = rm::ui::HintsPanel::describeHints (d);
            check (lines.size() == 2 && lines[0] == "TIP | Analysing the reference" && lines[1].startsWith ("TIP | Play your track"),
                   "loading, mix not played: analysing + play your track", lines.joinIntoString (" / "));
        }

        {
            auto d = makeMixOnlyViewData();
            d.referenceInfo.state = RefState::Error;
            const auto lines = rm::ui::HintsPanel::describeHints (d);
            check (lines.size() > 0 && lines[0] == "TIP | Load a reference track", "error: load hint stays", lines.joinIntoString (" / "));
        }

        {
            const auto full = rm::ui::HintsPanel::describeHints (makeFullViewData (track, false));
            check (full.size() > 0 && full[0].startsWith ("CHECK THIS | True peak"), "full: true peak first, CHECK THIS", full.joinIntoString (" / "));
            check (findLine (full, "BIG DIFFERENCE | Low mids +3.4").isNotEmpty(), "full: tone Major is BIG DIFFERENCE", full.joinIntoString (" / "));
            check (findLine (full, "WORTH A LOOK | Bass is a bit wide").isNotEmpty(), "full: low end Minor is WORTH A LOOK", full.joinIntoString (" / "));
        }

        {
            auto d = makeFullViewData (track, false);
            d.mix.lowEndCorrelation = 0.31f;
            d.mix.correlation = -0.2f;
            d.comparison = rm::compareProfiles (d.mix, d.reference);
            const auto lines = rm::ui::HintsPanel::describeHints (d);
            check (findLine (lines, "CHECK THIS | Bass is not mono").isNotEmpty(), "low end Major is CHECK THIS", lines.joinIntoString (" / "));
            check (findLine (lines, "CHECK THIS | Left and right partly cancel").isNotEmpty(), "phase Major is CHECK THIS", lines.joinIntoString (" / "));
        }

        {
            auto d = makeFullViewData (track, false);
            d.mix.isMono = true;
            d.mix.correlation = 1.0f;
            d.comparison = rm::compareProfiles (d.mix, d.reference);
            const auto lines = rm::ui::HintsPanel::describeHints (d);
            check (findLine (lines, "BIG DIFFERENCE | Mix is mono").isNotEmpty(), "mono mix vs stereo reference is BIG DIFFERENCE", lines.joinIntoString (" / "));
        }

        {
            rm::ui::ViewData empty;   // nothing published yet: the core's setup hints
            const auto lines = rm::ui::HintsPanel::describeHints (empty);
            check (lines.size() == 2 && lines[0] == "TIP | Load a reference track" && lines[1].startsWith ("TIP | Play your track"),
                   "empty data: core setup hints", lines.joinIntoString (" / "));
        }
    }

    // ---- Loudness one-liners agree with the hint rules ----------------------------------------------------------
    {
        auto rows = [] (float mixI, float refI, float mixPlr, float refPlr, float mixTp, float refTp)
        {
            auto d = makeFullViewData (makeSyntheticTrack(), false);
            d.mix.integratedLufs = mixI;  d.reference.integratedLufs = refI;
            d.mix.plrDb = mixPlr;         d.reference.plrDb = refPlr;
            d.mix.truePeakDb = mixTp;     d.reference.truePeakDb = refTp;
            return rm::ui::LoudnessPanel::describeRows (d);
        };

        auto severityOf = [] (const juce::String& line) { return juce::StringArray::fromTokens (line, "|", "")[3].trim(); };

        auto r = rows (-4.0f, -8.0f, 6.0f, 11.0f, -2.0f, -2.0f);   // 4 LU louder, PLR -5 dB
        check (severityOf (r[0]) == "major", "integrated 4 LU louder is major", r[0]);
        check (severityOf (r[3]) == "major", "PLR 5 dB more squashed is major", r[3]);

        r = rows (-6.0f, -8.0f, 9.0f, 11.5f, -2.0f, -2.0f);        // 2 LU louder, PLR -2.5 dB
        check (severityOf (r[0]) == "minor", "integrated 2 LU louder is minor", r[0]);
        check (severityOf (r[3]) == "minor", "PLR 2.5 dB more squashed is minor", r[3]);

        r = rows (-9.0f, -8.0f, 12.0f, 8.0f, -2.0f, -2.0f);        // as loud, PLR +4 (mastered)
        check (severityOf (r[3]) == "minor" && r[3].contains ("less dense"), "more dynamic, not quieter: minor", r[3]);

        r = rows (-13.0f, -8.0f, 12.0f, 8.0f, -2.0f, -2.0f);       // 5 LU quieter, PLR +4 (unmastered)
        check (severityOf (r[3]) == "info" && r[3].contains ("mastering"), "more dynamic and quieter: info", r[3]);

        r = rows (-8.0f, -8.0f, 8.0f, 8.0f, 0.7f, 2.4f);           // both over -1 dBTP, reference higher
        check (severityOf (r[2]) == "minor" && r[2].contains ("| +0.7 | +2.4 |"), "true peak, reference higher: minor", r[2]);

        r = rows (-8.0f, -8.0f, 8.0f, 8.0f, 0.7f, -0.3f);          // mix higher than a hot reference
        check (severityOf (r[2]) == "major", "true peak above the reference: major", r[2]);

        auto d = makeMixOnlyViewData();
        const auto noRef = rm::ui::LoudnessPanel::describeRows (d);
        check (noRef[1].endsWith ("Loudness of the last 3 seconds"), "short-term without a reference", noRef[1]);
    }

    // ---- Band words / tooltip --------------------------------------------------------------------------------------
    {
        using TB = rm::ui::TonalBalancePanel;
        check (TB::bandWord (2, 3.4f) == "muddier" && TB::bandWord (4, -2.0f) == "more distant"
                   && TB::bandWord (0, 0.5f) == "close" && TB::bandWord (5, -3.6f) == "duller",
               "comparative band words");
        check (TB::bandDifferenceText (-2.4f) == "Your mix: 2.4 dB less than the reference (both at the same loudness)",
               "band tooltip has no double sign", TB::bandDifferenceText (-2.4f));
    }

    // ---- A/B switch labels -----------------------------------------------------------------------------------------
    {
        auto d = makeFullViewData (track, false);
        d.listenReference = true;
        auto s = rm::ui::HeaderBar::switchStateFor (d, 1.0f);
        check (s.listenReference && s.mixSubLabel == "not heard" && s.referenceSubLabel == "turned down 3.9 dB",
               "A/B labels while hearing the reference", s.mixSubLabel + " / " + s.referenceSubLabel);

        d.listenReference = false;
        d.matchGainDb = 2.04f;
        s = rm::ui::HeaderBar::switchStateFor (d, 0.0f);
        check (! s.listenReference && s.mixSubLabel == "your track" && s.referenceSubLabel == "turned up 2.0 dB",
               "A/B labels on the mix", s.mixSubLabel + " / " + s.referenceSubLabel);

        d.matchGainDb = 0.02f;
        check (rm::ui::HeaderBar::switchStateFor (d, 0.0f).referenceSubLabel == "same level", "A/B label at 0 dB");

        auto noTrack = makeMixOnlyViewData();
        noTrack.listenReference = true;   // Listen = Reference but nothing to play: the mix is heard
        s = rm::ui::HeaderBar::switchStateFor (noTrack, 0.0f);
        check (! s.listenReference && ! rm::ui::hearingReference (noTrack) && s.mixSubLabel == "your track",
               "Listen = Reference without a track shows MIX", s.mixSubLabel);
    }

    // ---- Zoom ------------------------------------------------------------------------------------------------------
    {
        const float maxZoom = RefMatchEditor::kMaxZoom;
        check (RefMatchEditor::zoomForSize (1100, 690) == 1.0f && RefMatchEditor::zoomForSize (960, 560) == 1.0f
                   && RefMatchEditor::zoomForSize (1100, 1000) == 1.0f, "no zoom at or below the default size");
        check (std::abs (RefMatchEditor::zoomForSize (1600, 1000) - maxZoom) < 1.0e-6f, "maximum zoom at 1600 x 1000");
        check (std::abs (RefMatchEditor::zoomForSize (1300, 800) - 800.0f / 690.0f) < 1.0e-4f, "zoom follows the smaller ratio");
    }

    std::cout << "ui checks: " << passed << " passed, " << failed << " failed\n";
    return failed == 0;
}

//==============================================================================
// Pieces that only exist in popup windows (menus, tooltips) drawn onto one sheet with the editor's look-and-feel.
juce::Image renderWidgetSheet (juce::LookAndFeel& lf)
{
    juce::Image image (juce::Image::ARGB, 980, 420, true);
    juce::Graphics g (image);
    g.fillAll (rm::ui::Colours::background);

    struct Item
    {
        juce::String text;
        enum Kind { normal, heading, separator } kind = normal;
        bool ticked = false, highlighted = false, enabled = true;
        juce::String shortcut {};
    };

    auto drawMenu = [&] (juce::Point<int> origin, const std::vector<Item>& items)
    {
        const int border = lf.getPopupMenuBorderSize();
        int width = 160, height = 2 * border;

        for (const auto& item : items)
        {
            int w = 0, h = 0;

            if (item.kind == Item::heading)
                lf.getIdealPopupMenuSectionHeaderSizeWithOptions (item.text, 24, w, h, juce::PopupMenu::Options());
            else
                lf.getIdealPopupMenuItemSize (item.shortcut.isEmpty() ? item.text : item.text + "   " + item.shortcut,
                                              item.kind == Item::separator, 24, w, h);

            width = juce::jmax (width, w + 2 * border);
            height += h;
        }

        juce::Graphics::ScopedSaveState state (g);
        g.setOrigin (origin);
        lf.drawPopupMenuBackground (g, width, height);
        int y = border;

        for (const auto& item : items)
        {
            int w = 0, h = 0;

            if (item.kind == Item::heading)
            {
                lf.getIdealPopupMenuSectionHeaderSizeWithOptions (item.text, 24, w, h, juce::PopupMenu::Options());
                lf.drawPopupMenuSectionHeader (g, { border, y, width - 2 * border, h }, item.text);
            }
            else
            {
                lf.getIdealPopupMenuItemSize (item.text, item.kind == Item::separator, 24, w, h);
                lf.drawPopupMenuItem (g, { border, y, width - 2 * border, h }, item.kind == Item::separator, item.enabled,
                                      item.highlighted, item.ticked, false, item.text, item.shortcut, nullptr, nullptr);
            }

            y += h;
        }
    };

    // Level match menu (header chip)
    {
        Item off { "Off" };              off.shortcut = "own level";
        Item integrated { "Integrated" }; integrated.shortcut = "since reset"; integrated.ticked = true;
        Item shortTerm { "Short-term" };  shortTerm.shortcut = "last 3 s";   shortTerm.highlighted = true;
        drawMenu ({ 20, 20 }, { { "Level match", Item::heading }, off, integrated, shortTerm });
    }

    // Options menu
    {
        Item freeRun { "Play reference when FL is stopped" }; freeRun.ticked = true;
        Item recent { "Recent" };       recent.shortcut = "last " + juce::String (juce::roundToInt (rm::kRecentSeconds)) + " s";
        recent.ticked = true;
        Item all { "Since reset" };     all.shortcut = "everything played";   all.highlighted = true;
        drawMenu ({ 250, 20 }, { { "Reference playback", Item::heading }, freeRun, { "Analysis window", Item::heading },
                                 recent, all, { {}, Item::separator }, { "Reset analysis" }, { "Remove reference" } });
    }

    auto drawTip = [&] (const juce::String& tip, juce::Point<int> origin)
    {
        const auto bounds = lf.getTooltipBounds (tip, { 0, 0 }, { 0, 0, 1000, 1000 });
        juce::Graphics::ScopedSaveState state (g);
        g.setOrigin (origin);
        lf.drawTooltip (g, tip, bounds.getWidth(), bounds.getHeight());
    };

    drawTip ("Low mids (200 - 500 Hz): warmth, but also mud and boxiness. Too much makes the mix cloudy, too little "
             "sounds thin or cold.\n\n" + rm::ui::TonalBalancePanel::bandDifferenceText (3.4f) + " - a big difference.",
             { 620, 20 });
    drawTip ("True peak (dBTP): the highest point of the waveform since the last reset, including peaks that appear "
             "between samples when the audio is converted. Keep the finished master below -1 dBTP so it does not "
             "distort on streaming services or as MP3. RefMatch measures before FL's Master fader, so keep that fader "
             "at 0 dB.\n\nMix +0.4, reference -0.3: Above -1 dBTP - can distort on streaming and as MP3.", { 620, 190 });

    return image;
}

juce::Component* findByTitle (juce::Component& root, const juce::String& title)
{
    if (root.getTitle() == title)
        return &root;

    for (auto* child : root.getChildren())
        if (auto* found = findByTitle (*child, title))
            return found;

    return nullptr;
}

juce::MouseEvent syntheticMouseEvent (juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, bool dragged)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                             juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, now, downPos, now, 1, dragged);
}

struct Size { int w, h; const char* name; };

const Size kSizes[] = { { RefMatchEditor::kDefaultWidth, RefMatchEditor::kDefaultHeight, "1100x690" },
                        { RefMatchEditor::kMinWidth, RefMatchEditor::kMinHeight, "960x560" },
                        { RefMatchEditor::kMaxWidth, RefMatchEditor::kMaxHeight, "1600x1000" } };
} // namespace

int runSnapshot (const juce::File& outputDir)
{
    outputDir.createDirectory();
    bool ok = true;
    rm::ui::freezeAnimationsForSnapshots (true);   // spinners at a fixed phase: identical PNGs on every run

    RefMatchProcessor processor;
    std::unique_ptr<juce::AudioProcessorEditor> holder (processor.createEditor());
    auto* editor = dynamic_cast<RefMatchEditor*> (holder.get());

    if (editor == nullptr)
    {
        std::cout << "FAILED: createEditor() did not return a RefMatchEditor\n";
        return 1;
    }

    const auto track = makeSyntheticTrack();
    ok &= runUiChecks (track);

    // Renders one state at every size.
    auto shoot = [&] (const juce::String& name, const std::optional<rm::ui::ViewData>& data,
                      const std::function<void()>& prepare = {})
    {
        for (const auto& size : kSizes)
        {
            editor->setSize (size.w, size.h);
            editor->setViewDataOverride (data);

            if (prepare != nullptr)
                prepare();

            pumpMessages (20);
            editor->refreshNow();
            ok &= writePng (*editor, outputDir.getChildFile (name + "_" + size.name + ".png"));
        }
    };

    // 01: nothing yet: no reference, the mix has not been played
    shoot ("01_empty", rm::ui::ViewData());

    // 02: the mix has been played, no reference loaded
    shoot ("02_mix_only", makeMixOnlyViewData());

    // 03: loading the first reference (the core still says "Load a reference track": the panel shows "Analysing")
    shoot ("03_loading", makeLoadingViewData());

    // 03b: the first reference is decoded and installed, its region is still being analysed (also on every reload)
    shoot ("03b_analysing", makeAnalysingViewData (track));

    // 04: the file could not be loaded
    {
        auto d = makeMixOnlyViewData();
        d.hostPlaying = false;
        d.referenceInfo.state = RefState::Error;
        d.referenceInfo.displayName = "voice memo 03";
        d.referenceInfo.error = "Unsupported or damaged audio file";
        d.referenceInfo.file = juce::File ("C:\\Users\\Producer\\Desktop\\voice memo 03.m4a");
        d.referenceInfo.version = 2;
        shoot ("04_error", d);
    }

    // 05: full analysis, listening to the mix
    shoot ("05_full_mix", makeFullViewData (track, false));

    // 05b: a tall window (no zoom: the layout reflows) and an in-between size (zoom 1.16)
    for (const auto& size : { Size { 1100, 1000, "1100x1000" }, Size { 1300, 800, "1300x800" } })
    {
        editor->setSize (size.w, size.h);
        editor->setViewDataOverride (makeFullViewData (track, false));
        pumpMessages (20);
        editor->refreshNow();
        ok &= writePng (*editor, outputDir.getChildFile (juce::String ("05b_full_mix_") + size.name + ".png"));
    }

    // 06: listening to the reference (loudness matched)
    {
        auto d = makeFullViewData (track, false);
        d.listenReference = true;
        d.listenPosition = 1.0f;
        d.referenceSounding = true;
        shoot ("06_full_reference", d);
    }

    // 07: loop region set on the first drop
    shoot ("07_loop", makeFullViewData (track, true));

    // 07b: a very short loop near the end (its two end labels merge into one)
    {
        auto d = makeFullViewData (track, true);
        d.referenceInfo.loopStart = 200.4;
        d.referenceInfo.loopEnd = 201.0;
        editor->setSize (RefMatchEditor::kMinWidth, RefMatchEditor::kMinHeight);
        editor->setViewDataOverride (d);
        ok &= writePng (*editor, outputDir.getChildFile ("07b_short_loop_960x560.png"));
    }

    // 07d: a 4-second reference at a constant level (the loudness shape must reach both ends, not look silent there)
    {
        auto shortTrack = std::make_shared<rm::ReferenceTrack> (*track);
        const double seconds = 4.0;
        const int columns = rm::ReferenceTrack::kOverviewColumns;
        shortTrack->overview.minimum.assign ((size_t) columns, -0.7f);
        shortTrack->overview.maximum.assign ((size_t) columns, 0.7f);
        shortTrack->shortTermCurve.assign ((size_t) (seconds * 10.0), rm::kNoValue);

        for (size_t i = 29; i < shortTrack->shortTermCurve.size(); ++i)
            shortTrack->shortTermCurve[i] = -9.0f;

        auto d = makeFullViewData (shortTrack, false);
        d.referenceInfo.lengthSeconds = seconds;
        d.playheadSeconds = 1.0;
        editor->setSize (RefMatchEditor::kMinWidth, RefMatchEditor::kMinHeight);
        editor->setViewDataOverride (d);
        ok &= writePng (*editor, outputDir.getChildFile ("07d_short_track_960x560.png"));
    }

    // 07c: a collapsed hint opened with a left click (and closed again)
    {
        editor->setSize (RefMatchEditor::kDefaultWidth, RefMatchEditor::kDefaultHeight);
        editor->setViewDataOverride (makeFullViewData (track, false));
        auto* panel = dynamic_cast<rm::ui::HintsPanel*> (findByTitle (*editor, "What to try"));
        auto* cards = panel != nullptr ? findByTitle (*panel, "Hint cards") : nullptr;

        if (panel != nullptr && cards != nullptr)
        {
            auto clickCard = [&] (int index)
            {
                const auto centre = cards->getLocalPoint (panel, panel->getCardBoundsForTesting (index).getCentre()).toFloat();
                cards->mouseUp (syntheticMouseEvent (*cards, centre, centre, false));
            };

            const int closedH = panel->getCardBoundsForTesting (1).getHeight();
            clickCard (1);
            const int openH = panel->getCardBoundsForTesting (1).getHeight();
            editor->refreshNow();
            ok &= writePng (*editor, outputDir.getChildFile ("07c_hint_opened_1100x690.png"));
            clickCard (1);
            const int reclosedH = panel->getCardBoundsForTesting (1).getHeight();

            // Back to the top for the next snapshots (opening scrolled the card into view).
            for (auto* c = cards->getParentComponent(); c != nullptr; c = c->getParentComponent())
                if (auto* viewport = dynamic_cast<juce::Viewport*> (c))
                {
                    viewport->setViewPosition (0, 0);
                    break;
                }

            const bool works = closedH > 0 && closedH < 40 && openH > closedH + 20 && reclosedH == closedH;
            std::cout << (works ? "hint row opens and closes on a click: ok" : "FAILED: hint row open / close")
                      << " (" << closedH << " -> " << openH << " -> " << reclosedH << " px)\n";
            ok &= works;
        }
        else
        {
            std::cout << "FAILED: no hint cards component\n";
            ok = false;
        }
    }

    // 08: variants: listening to the reference with short-term matching, mono on, bass too wide, loop being analysed
    {
        auto d = makeFullViewData (track, true);
        d.matchMode = rm::MatchMode::ShortTerm;
        d.mono = true;
        d.matchGainDb = -2.7f;
        d.listenReference = true;
        d.listenPosition = 1.0f;
        d.referenceSounding = true;
        d.mix.lowEndCorrelation = 0.31f;
        d.comparison = rm::compareProfiles (d.mix, d.reference);
        d.referenceInfo.analysing = true;
        shoot ("08_variants", d);
    }

    // 09: help overlay
    shoot ("09_help", makeFullViewData (track, true), [editor] { editor->setHelpVisible (true); });
    editor->setHelpVisible (false);

    // 10: file dragged over the editor
    shoot ("10_drag", makeFullViewData (track, true), [editor] { editor->setDragHighlightForTesting (true); });
    editor->setDragHighlightForTesting (false);

    // 11: popup menus and tooltips
    ok &= writeImage (renderWidgetSheet (editor->getLookAndFeel()), outputDir.getChildFile ("11_widgets.png"));

    // 12: dragging a new loop region across the waveform (live preview before the mouse is released)
    {
        editor->setSize (RefMatchEditor::kDefaultWidth, RefMatchEditor::kDefaultHeight);
        editor->setViewDataOverride (makeFullViewData (track, false));

        if (auto* strip = findByTitle (*editor, "Reference waveform"))
        {
            const float y = (float) strip->getHeight() * 0.45f;
            const juce::Point<float> down ((float) strip->getWidth() * 0.62f, y), to ((float) strip->getWidth() * 0.80f, y);
            strip->mouseDown (syntheticMouseEvent (*strip, down, down, false));
            strip->mouseDrag (syntheticMouseEvent (*strip, to, down, true));
            editor->refreshNow();
            ok &= writePng (*editor, outputDir.getChildFile ("12_loop_drag_preview_1100x690.png"));
            strip->mouseUp (syntheticMouseEvent (*strip, to, down, true));

            // The processor ignores the request (no real reference): the preview must disappear again.
            pumpMessages (1100);
            editor->refreshNow();
            ok &= writeImage (editor->createComponentSnapshot (editor->getLocalArea (strip, strip->getLocalBounds()), true, 1.0f),
                              outputDir.getChildFile ("12b_after_release_strip.png"));
        }
        else
        {
            std::cout << "FAILED: no reference waveform component" << std::endl;
            ok = false;
        }
    }

    // 15: zoom: hit-testing and a tooltip through the content's scale (1600 x 1000) and at the default size (1:1)
    for (const auto& size : { kSizes[2], kSizes[0] })
    {
        editor->setSize (size.w, size.h);
        editor->setViewDataOverride (makeFullViewData (track, false));
        editor->refreshNow();
        auto* panel = findByTitle (*editor, "Loudness and dynamics");

        if (panel == nullptr)
        {
            std::cout << "FAILED: no loudness panel\n";
            ok = false;
            break;
        }

        const float zoom = RefMatchEditor::zoomForSize (size.w, size.h);
        const auto area = editor->getLocalArea (panel, panel->getLocalBounds());   // editor (physical) pixels
        const bool wasVisible = editor->isVisible();
        editor->setVisible (true);   // (hit-testing skips invisible components; a host shows the editor)
        auto* hit = editor->getComponentAt (area.getCentre());
        editor->setVisible (wasVisible);
        const bool hitOk = hit == panel || (hit != nullptr && panel->isParentOf (hit));
        const bool scaleOk = std::abs ((float) area.getWidth() - zoom * (float) panel->getWidth()) <= 2.0f
                             && std::abs ((float) area.getHeight() - zoom * (float) panel->getHeight()) <= 2.0f;
        std::cout << (hitOk && scaleOk ? "zoom " : "FAILED: zoom ") << size.name << ": x" << zoom << ", loudness panel "
                  << panel->getWidth() << " -> " << area.getWidth() << " px, hit-test " << (hitOk ? "ok" : "wrong") << "\n";
        ok &= hitOk && scaleOk;

        editor->showTooltipForTesting ({ area.getX() + area.getWidth() / 4, area.getY() + area.getHeight() * 2 / 5 },
                                       "True peak (dBTP): the highest point of the waveform since the last reset, including "
                                       "peaks that appear between samples when the audio is converted. Keep the finished "
                                       "master below -1 dBTP so it does not distort on streaming services or as MP3.");
        ok &= writePng (*editor, outputDir.getChildFile (juce::String ("15_tooltip_") + size.name + ".png"));
        editor->hideTooltipForTesting();
    }

    // 13: the real processor, whatever state it is in (proves the non-override path)
    editor->setSize (RefMatchEditor::kDefaultWidth, RefMatchEditor::kDefaultHeight);
    editor->setViewDataOverride (std::nullopt);
    pumpMessages (50);
    editor->refreshNow();
    ok &= writePng (*editor, outputDir.getChildFile ("13_real_processor_1100x690.png"));

    // 14: an unsupported file dragged over the editor, then dropped. Last, because the notice it leaves at the bottom
    //     stays for a few seconds.
    {
        const juce::StringArray files { "C:/Users/Producer/Desktop/session notes.txt" };
        editor->setViewDataOverride (makeFullViewData (track, false));
        editor->fileDragEnter (files, 300, 300);
        ok &= writePng (*editor, outputDir.getChildFile ("14_drag_unsupported_1100x690.png"));
        editor->filesDropped (files, 300, 300);
        ok &= writePng (*editor, outputDir.getChildFile ("14b_unsupported_notice_1100x690.png"));
    }

    holder.reset();
    std::cout << (ok ? "snapshot: OK\n" : "snapshot: FAILED\n");
    return ok ? 0 : 1;
}
