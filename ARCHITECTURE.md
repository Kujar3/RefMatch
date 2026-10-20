# RefMatch: architecture & team contract

RefMatch is a reference-track matcher (VST3 + Standalone + CLAP, JUCE 8, C++17) for producers who mix by ear. It sits
on the master channel, plays a commercial reference track in place of the mix at the click of a button (loudness
matched, so the comparison is fair) and explains, in plain language, how the mix differs from the reference: tonal
balance per band, loudness, peaks and dynamics, stereo width and low-end mono compatibility.
Main target host: **FL Studio 2026 on Windows at 1920x1080**. Sibling project and template: Chordbloom
(`Desktop\Klimper-Like`). The user-facing guide (install, FL setup, how to read the panels) is `README.md`.

## Repository layout

```
CMakeLists.txt              targets: RefMatch (VST3, Standalone, + RefMatch_CLAP when clap-juce-extensions exists),
                            CoreTests, RefMatchDev. Sources are globbed per folder (src/core, src/plugin, src/ui,
                            tests, tools/dev): a new .cpp is picked up on the next configure (cmake re-runs
                            automatically)
README.md                   for the user (a producer): what it does, install, FL setup, beginner's guide, dev checks
docs/screenshot.png         README image = RefMatchDev snapshot 07_loop_1100x690.png (synthetic data only)
scripts/build.bat           scripts\build.bat <buildDir> <targets...>   (Ninja + MSVC 2022 x64 Release)
scripts/fetch_clap.bat      clap-juce-extensions checkout at a pinned commit (+ submodules) into third_party,
                            applies clap-juce-extensions-state-rescan.patch (param rescan after state load)
scripts/install-plugin.ps1  copies the built VST3 / CLAP to C:\Program Files\Common Files\{VST3,CLAP} (one UAC
                            prompt); -CurrentUser: %LOCALAPPDATA%\Programs\Common\{VST3,CLAP}; -BuildDir, -DestRoot.
                            Staged replace (.new / .old): a copy FL has loaded is left intact
src/core/                   pure C++17 analysis, no JUCE, unit-tested by CoreTests
  Common.h                  kNoValue sentinel, dB helpers, the 6 bands, spectrum curve grid, kLowEndMonoHz
  Loudness.h/.cpp           BS.1770-4 K-weighting, LoudnessMeter (M / S / I / LRA)
  TruePeak.h/.cpp           4x oversampled true-peak meter, crest factor
  Fft.h/.cpp                radix-2 complex FFT
  Analysis.h/.cpp           TrackAnalyser -> TrackProfile (loudness, peaks, spectrum, bands, stereo), offline helpers
  Compare.h/.cpp            matching gain, mix-vs-reference Comparison, band severity (+ hysteresis), Hints
src/plugin/                 JUCE audio side
  PluginProcessor.h/.cpp    RefMatchProcessor (public API = contract; private Impl is free)
  ReferenceTrack.h/.cpp     decode + resample + analyse a reference file (loader thread)
src/ui/                     JUCE editor (each panel owns a private Impl)
  Theme.h/.cpp              colours, Inter fonts, LookAndFeel (from Chordbloom) + RefMatch roles / severity colours
  Widgets.h/.cpp            fmt helpers, EditorActions, IconButton, ABSwitch, MenuChip, rich text, logo, animation clock
  ViewData.h/.cpp           snapshot of everything the panels draw, gathered once per UI tick
  PluginEditor.h/.cpp       layout + coordination (file chooser, drag and drop, notices, help, size persistence)
  HeaderBar.h/.cpp          logo, reference chip + Load, A/B switch, Match chip, Mono, Reset, options menu, Help
  WaveformStrip.h/.cpp      reference overview + loudness line, loop region, playhead, seek; empty-state drop target
  TonalBalancePanel.h/.cpp  spectrum overlay + six band difference bars
  LoudnessPanel.h/.cpp      integrated / short-term / true peak / PLR / LRA rows, mix vs reference
  StereoPanel.h/.cpp        width per band, phase correlation, bass mono badge
  HintsPanel.h/.cpp         "What to try" cards
  HelpOverlay.h/.cpp        beginner's guide overlay ("?")
tests/                      CoreTests (tiny framework in TestFramework.h, namespace rmtest; TestSignals.h generators)
tools/dev/                  RefMatchDev: DevMain.cpp (`selftest`, `snapshot <dir>`, `compare <mix> <ref> [dir]`),
                            SelfTest.cpp, Snapshot.cpp, Compare.cpp, AllocationTracking.h/.cpp (realtime checks)
resources/fonts             Inter 400/600/700 (RefMatchBinary::Inter400_ttf / Inter600_ttf / Inter700_ttf)
resources/images            logo.png (RefMatchBinary::logo_png), embedded the same way
resources/icon              logo_512.png (plugin / app icon, ICON_BIG)
third_party/                JUCE 8.0.16 (tag 8.0.16-security), clap-juce-extensions (gitignored)
tools/pluginval             pluginval.exe (gitignored); tools/clap-validator (gitignored)
out/                        snapshots, compare runs, ground-truth measurements (gitignored)
Example_Tracks/             the user's private test songs (gitignored, NEVER copy, bundle or commit them)
```

## Rules for everyone

1. **Headers are contracts.** The public parts of `src/core/*.h`, `src/plugin/*.h`, `src/ui/PluginEditor.h` and
   `src/ui/ViewData.h` were written up front so modules can be built in parallel. Implement them as specified. You may
   *add* things (private members, helpers, new public methods, new files) but do not change or remove existing public
   signatures. If a contract is genuinely wrong, make the smallest fix and say so in your final report.
2. **Only edit the files you own** (your task lists them). Other agents edit other files at the same time.
3. **Build in your own build directory** (`build-<yourname>`), e.g. `cmd //c "scripts\\build.bat build-core CoreTests"`
   from Git Bash in the repo root. The first JUCE build of a directory takes several minutes. If the build fails in a
   file you do not own, it is someone else's work in progress: re-run later.
4. **Audio thread**: no allocation, no locks (try-lock at most), no logging, no `juce::String` building, no file I/O.
   `rm::LoudnessMeter` / `rm::TruePeakMeter` are realtime-safe after prepare(); `rm::TrackAnalyser::push` is
   allocation-free but is meant for the analysis thread. The selftest enforces this (see Verification).
5. **Determinism**: core functions are pure and deterministic (no rand, no time). Tests generate noise with their own
   LCG.
6. Code style: JUCE-like (4-space indent, braces on new lines, `camelCase` methods, `Type& name`), C++17. **0 warnings
   in our sources** (MSVC /W4 via juce_recommended_warning_flags; CoreTests /W4 /permissive-).
7. User-facing text (hints, tooltips, help) is ASCII only, plain language, names the measured number and suggests one
   concrete FL Studio action. Keep `README.md` in step when behaviour the user sees changes.
8. Never commit; the coordinator commits.

## Measurements (src/core)

- **Loudness** per ITU-R BS.1770-4 / EBU R128: K-weighting (shelf + RLB high-pass designed for any sample rate),
  100 ms sub-blocks; momentary = 400 ms, short-term = 3 s, integrated = 400 ms blocks every 100 ms with the -70 LUFS
  absolute and -10 LU relative gates (histogram with 0.01 LU bins, fixed memory); LRA per EBU Tech 3342.
  Stereo channel weights 1.0; mono input = one channel.
- **True peak**: 4x polyphase windowed-sinc interpolation (64 taps), max |x| of the oversampled signal, since reset.
- **PLR** (`TrackProfile::plrDb`) = **sample peak** - integrated loudness. Deliberately not the true peak: clipped
  masters overshoot between samples by 1-3 dB, more the harder they are clipped, so a true-peak PLR would make a
  squashed track look more dynamic. True peak (streaming safety) has its own readout.
- **Spectrum**: Hann-windowed FFT (8192 at <= 50 kHz, 16384 up to 100 kHz, 32768 above), 50 % overlap; L and R in one
  complex FFT (L real, R imaginary). Per bin accumulate |L|^2, |R|^2, Re(L conj R), always **twice**: plain sums since
  reset and exponentially forgetting sums with a **kRecentSeconds = 30 s** time constant. "Since reset" reports the
  first, "Recent" the second, so switching the averaging mode is instant and exact either way. Frames whose mean power
  is below -70 dBFS are skipped (pausing does not erase the picture); RMS uses the same gate per hop-sized chunk. A
  profile is `valid` after kMinProfileSeconds = 1 s of non-silent audio.
- **Bands**: Sub 20-60, Bass 60-200, Low mids 200-500, Mids 500-2k, High mids 2k-6k, Air 6k-20k Hz (short names
  Sub / Bass / Low-mid / Mid / High-mid / Air). Level = mean-square power of the band (mean of channels), integrated
  from the PSD with partial edge bins; width = 100 * side / mid energy (0..300); correlation = Re(L conj R) normalised.
  Broadband correlation / width over 20 Hz - 20 kHz; low-end correlation below kLowEndMonoHz = 120 Hz.
- **Curve**: PSD (dB/Hz, mean of channels) at 160 log-spaced points (20 Hz - 20 kHz), smoothed over log2 f with a Hann
  (raised-cosine) kernel of half-width kCurveSmoothingOctaves = 1/3 (1/3-octave equivalent bandwidth, support +-1/3
  octave). Each FFT bin gets its exact share of the kernel; the weights are precomputed in prepare(), so profile() is a
  plain multiply-add loop. kNoValue at or above Nyquist. The UI adds a +4.5 dB/octave display tilt (pivot 1 kHz) so
  typical music looks roughly flat.
- **Loudness normalisation for tonal balance**: each profile has `spectralLoudnessDb` (K-weighted power of the same
  averaged spectrum). Band differences are `(mixBand - mixSpectral) - (refBand - refSpectral)`, i.e. "with both at the
  same loudness". Band and spectral power come from the same average (whichever averaging mode is selected), so the
  difference does not depend on how loud either track is, nor on the A/B matching gain.
- **Reference analysis**: the loop region (or whole file) with SinceReset averaging (`rm::analyseRegion`, or
  `rm::analyseReferenceRegion` with cancellation, which also measures the integrated loudness of the (L+R)/2 fold-down
  for a mono bus).

### Comparison and hints (Compare.cpp)

- **Matching gain** `referenceMatchGainDb (mix, ref)` = clamp (mix - ref, -40, +12) dB; 0 when the reference loudness is
  unknown, kFallbackMixLufs = -14 LUFS in place of an unknown mix loudness.
- **Band severity**: |diff| >= 1.5 dB Minor, >= 3 dB Major (kBandMinorDb / kBandMajorDb). For live updates
  `compareProfiles (mix, ref, previous)` adds hysteresis (kBandHysteresisDb = 0.4 dB): a band steps up only at
  threshold + 0.4 dB and down only below threshold - 0.4 dB, and the Tone hints follow the held severities. Pass the
  previous Comparison for the same reference / region / averaging mode, nullptr after a mix reset, a reference or
  region change or an averaging switch. The two-argument overload has no hold. The processor holds while publishing
  live mix updates and starts fresh after a mix reset, an averaging switch and a new reference / region profile.
- **Hints** (plain language, with the measured number and one FL Studio action; at most kMaxHints = 8):
  - Setup (Info, always first): "Load a reference track" / "Reference region is too short" / "Play your track for a
    few seconds" (put RefMatch last on the Master, after the limiter).
  - True peak (mix only): above -1 dBTP Major; Minor when the reference is valid, also above -1 dBTP and peaks at least
    as high as the mix. The text says to lower the limiter ceiling to about -2 dB (Maximus: master POST), then Reset
    analysis (the peak is a since-reset maximum); the FL master fader does not help.
  - Low end (mix only, not for a mono mix, only when Sub/Bass are within 30 dB of the spectral loudness): low-end
    correlation < 0.8 Minor, < 0.5 Major.
  - Loudness: |mix - ref integrated| > 1 LU. Quieter = Info ("normal before mastering"); louder = Minor, > 3 LU Major.
  - Dynamics: |PLR diff| > 2 dB. Lower PLR (more squashed) Minor, < -4 dB Major; higher PLR Minor, or Info when the mix
    is also > 3 LU quieter (unmastered).
  - Tone: every band that is not Good (band severity above), with per-band advice.
  - Stereo: broadband correlation < 0 Major (and no other stereo hint); mono mix vs a reference wider than 20 % Major;
    otherwise per-band width from Low mids up, one hint per direction about the band with the biggest difference,
    |diff| >= 20 points Minor, >= 40 Major. (No LRA hint: LRA is shown, not judged.)
  - "Close to your reference" (Good) when the comparison is valid and nothing is Minor / Major.
  Sorted: Setup first, then severity, then the difference relative to its topic's threshold.

## Processor (src/plugin)

### Parameters (APVTS, all automatable)

| id          | type   | values                                          | default     |
|-------------|--------|-------------------------------------------------|-------------|
| `listen`    | bool   | Mix (A) / Reference (B)                         | Mix         |
| `matchMode` | choice | Off, Integrated, Short-term                     | Integrated  |
| `monoCheck` | bool   |                                                 | off         |
| `freeRun`   | bool   | reference plays while host is stopped           | on          |
| `averaging` | choice | Recent (kRecentSeconds = 30 s), Since reset     | Recent      |

Buses: one input and one output, mono -> mono or stereo -> stereo. Times: kListenFadeMs 30, kJumpFadeMs 8, kMonoFadeMs
20, kGainSmoothMs 250 (multiplicative smoothing), kMinLoopSeconds 0.5, kHostSyncToleranceMs 5.

### Threads

Lock order (a thread may take a lock further down while holding one further up, never the reverse): `stateMutex`
(reference info, loader jobs, the UI's track pointer, serials, host rate) -> `analysisMutex` (mix analyser, FIFO read
side, matching-gain state) -> `resultsMutex` (published profiles and comparison) -> `slotLock` (`juce::SpinLock` around
the tracks and region the audio thread plays).

- **Audio thread** (`processBlock`): copies the input (the mix) into an SPSC FIFO (`juce::AbstractFifo`, ~2 s stereo,
  drops when full) for the analysis thread, except while a host transport that has reported playing since
  prepareToPlay is stopped (FL sends silence then; the mix meters hold, like the spectrum). Hosts whose playhead never
  plays (JUCE Standalone) and offline renders feed everything. It plays the reference, crossfades, folds to mono, and
  measures the rendered reference with an `rm::LoudnessMeter` (unity gain, the (L+R)/2 fold-down as one channel on a
  mono bus) for short-term matching and the UI (`getReferenceShortTermLufs()`).
- **Analysis thread** (`juce::Thread`, low priority, every ~15 ms, started in prepareToPlay, stopped in
  releaseResources / prepareToPlay / the destructor): applies the averaging parameter, drains the FIFO into an
  `rm::TrackAnalyser`, publishes `TrackProfile mix` and `Comparison` (against the current reference region profile,
  with band hysteresis anchored on the previous comparison; the loader publishes a fresh comparison when a new region
  profile arrives) under `resultsMutex` at most every 30 ms when something changed (never touched by the audio thread), bumps
  `analysisVersion`, frees replaced tracks the audio thread no longer reads, and computes the matching-gain *target*
  for the audio thread (atomic float):
  - Off -> 0 dB.
  - Integrated -> `rm::referenceMatchGainDb (mix.integratedLufs, refRegion.integratedLufs)` (on a mono bus the
    region's fold-down loudness).
  - Short-term -> `rm::referenceMatchGainDb (mix.shortTermLufs, short-term of the reference as rendered)`, used while
    both 3 s windows hold >= 3.2 s of continuous, non-silent audio (no gap, no 100 ms step below -70 dBFS, both values
    above -70 LUFS). Otherwise the last short-term target is held (the integrated match before the first one; reset on
    a mode, track or loop-region change and in prepareToPlay, not on resetMixAnalysis). Loop wraps, host loops, seeks
    and the mono fold are included because the reference is measured as heard.
  `runAnalysisNow()` performs exactly one such iteration synchronously (tests).
- **Loader thread** (`juce::Thread`, low priority): one pending load and one pending region job (a load wins); a newer
  request cancels an older one through serial numbers polled by the decode / resample / analysis loops.
  `rm::loadReferenceTrack` decodes, resamples to the host rate, builds the short-term curves and the overview; then
  `rm::analyseReferenceRegion` analyses the loop region (the whole-file result is cached for clearLoopRegion). When
  done the loader installs the new `shared_ptr<const ReferenceTrack>` itself (no message-thread round trip, so it also
  works while the host's message thread is busy) and publishes the region profile. Loads requested before
  prepareToPlay use 44.1 kHz. When prepareToPlay changes the sample rate, the current file is re-loaded at the new rate
  (the mix is heard until then). Out of memory -> state Error "Not enough memory to load this file: <name>".
- **Handing the track to the audio thread**: `Slot` = current track + generation + region (samples) + `regionGen`.
  Other threads take `slotLock` only for a hand-over, a region change, and while replaced tracks wait to be freed
  (an atomic `retiredPending` flag keeps the analysis thread off the lock in steady state). The audio thread
  try-locks once per rendered chunk (a host block, split when it is larger than the scratch buffer), retrying up to
  32 times with a CPU pause; if that fails the reference part of that chunk is silent and its voices do not advance
  (the mix part of an A/B fade is unaffected; counted in `getReferenceDropoutCount()`, expected 0). A replaced track
  is parked in `Slot::retired` (up to 8) so the audio thread can fade out of it, and released on another thread once
  the audio thread marks it releasable; tracks are always destroyed outside every lock and never on the audio thread.

### Reference track (ReferenceTrack.h/.cpp)

- `ReferenceTrack`: file, displayName, file rate and channels, `sampleRate` (= host rate at load), `audio` (always 2
  channels: mono duplicated, extra channels ignored), `shortTermCurve` (stereo, every 100 ms; drawn as the loudness
  line), `monoShortTermCurve` (the (L+R)/2 fold-down; not used for matching any more since the reference is measured as
  heard), `overview` (kOverviewColumns = 2048 min/max columns of the mono sum).
- Formats: JUCE's basic formats (WAV, AIFF, FLAC, Ogg Vorbis, MP3); the file filter is
  `*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg`. Length 0.5 s .. 20 min (kMaxReferenceSeconds, accepted with 1 s of
  tolerance) and at most INT_MAX samples.
- Non-finite samples of a damaged float file become silence at load, before resampling; more than 1 % ->
  "Unsupported or damaged audio file".
- Resampling: Kaiser-windowed sinc (48 zero crossings per side, beta 9.5, cut-off at 95 % of the lower Nyquist),
  exact polyphase table for small rational ratios (44.1 <-> 48 kHz: 160 / 147 phases), else 4096 quantised phases.
- Errors (the processor appends ": <file name>"): "File not found", "Unsupported or damaged audio file", "The file
  contains no audio", "File is shorter than half a second", "File is longer than 20 minutes", "Not enough memory to
  load this file".

### Reference playback

- The reference buffer is at the host sample rate (resampled at load), always 2 channels; for a mono bus it is folded
  to mono ((L+R)/2).
- **Region**: whole file, or the loop region (`setLoopRegion`, clamped to the file and to >= kMinLoopSeconds).
  Playback loops inside the region.
- **Voices**: a pool of kMaxVoices = 8 read heads. The main voice is what should play; every discontinuity (host jump,
  loop wrap, seek, region or track change, start / stop) starts a new main voice that fades in over kJumpFadeMs while
  the previous ones fade out from their current equal-power weight. Continuous playback is sample-exact (at equal
  sample rates and 0 dB gain the output equals the file's samples). Jumps keep every fading voice while a slot is free;
  only with 8 jumps inside one fade is the quietest voice (about 1 % weight by then) replaced. A region that ends at
  the end of the file wraps jumpLength samples early, crossfading
  into the lead-in before the region start; without a lead-in the new voice stays silent until the region start.
- **Host playing** (playhead reports isPlaying): reference position = `regionStart + mod (hostSamples + seekOffset,
  regionLength)` where hostSamples comes from timeInSamples (else timeInSeconds, else ppq * 60 / bpm). So pressing
  play at bar 1 plays the region from its start, and host jumps / loops jump the reference too. Host positions within
  kHostSyncToleranceMs of the reference's own continuation are not jumps (hosts wobble by a sample or two); larger
  differences and accumulated drift resync with one crossfade. A seek (`seekReference`, click in the waveform) while
  playing sets `seekOffset` so the clicked point plays now; it is reset by a track change.
- **Host stopped / no playhead (Standalone)**: with `freeRun` on, the reference keeps playing from where it is
  (looping the region); with it off, the reference is silent and the position holds.
- **A/B**: equal-power crossfade over kListenFadeMs (sin/cos tables). The mix input is never modified in level. When the
  fade has settled on Mix, mono is off and no fade is running, the output buffer is left untouched (bit-exact). With no
  track installed (none loaded, a load error, the reload after a rate change) the A/B target is Mix whatever `listen`
  says, so an empty reference never silences the mix; prepareToPlay starts on the reference only if a track is
  installed.
- **Matching gain** ramps towards the analysis thread's target over kGainSmoothMs (no zipper noise).
- **Mono check**: output L = R = (L+R)/2, ramped over kMonoFadeMs (a mono bus is mono already).
- **Offline render** (`isNonRealtime()`): output = untouched input, always (the input still feeds the analysis; the
  reference does not play).

### State format

```xml
<RefMatch version="1" editorW="1100" editorH="690">
  <PARAMS>...</PARAMS>                  (APVTS state)
  <Reference path="C:\...\track.wav" hasLoop="1" loopStart="64.0" loopEnd="96.0"/>   (absent = none)
</RefMatch>
```

- `getStateInformation` writes the parameters, the editor size, and the reference path + loop (seconds) unless the
  reference is Empty (an Error state keeps its path, so re-saving a project does not lose it). The audio is not stored.
- `setStateInformation` accepts this element or a bare `<PARAMS>` tree (anything else is ignored). Every parameter in
  the PARAMS tree is sanitised (missing / non-finite -> default, clamped); without a PARAMS element the current values
  are kept. Either way `listen` is set back to Mix (clap-validator's 3 state-reproducibility
  checks fail because of this, by design). The editor size is clamped to the editor limits. A `<Reference>` starts
  loading the file with its loop (re-using the loaded track when it is the same file at the same rate); a full state
  without one clears the reference. A relative path resolves against the working directory. A missing file gives state
  Error "File not found: <name>" while keeping the path.
- **Relink**: `loadReference` of a file with the same name (case-insensitive) while the reference is in the Error state
  keeps the saved loop region (clamped to the new file); any other load starts without a loop.

## UI (src/ui)

- Default editor size **1100 x 690**, resizable 960 x 560 .. 1600 x 1000 (corner resizer), remembered in the state and
  fitted to the primary screen when opened. Layout (PluginEditor::resized): header (60 px, 56 when compact = height
  < 620) on top; the waveform strip (~12.5 % of the height); below it a left column with Tonal balance (60 %) over
  What to try, and a right column (37 % of the width, 330..520 px) with Loudness & dynamics over Stereo image. Nothing
  may overlap or clip at 960 x 560.
- Dark slate Chordbloom look (Theme.h): soft rounded panels (radius 8), 1 px low-contrast outlines, subtle gradients,
  Inter. Mix = mint (`Colours::mix`), Reference = lavender (`Colours::reference`). Severity colours: good teal,
  info sky, minor amber, major coral. While an editor is open its LookAndFeel is the default for this plug-in binary
  (popup menus), handed back when the last editor closes.
- Areas:
  - **Header**: logo + name, reference file chip with Load / remove and loading / error states, big A/B
    "MIX | REFERENCE" switch with what level match does as its sub-label ("turned down 3.9 dB"), Match chip (Off /
    Integrated / Short-term), Mono, Reset analysis, options menu (Play reference when FL is stopped, Analysis window
    Recent / Since reset, Reset analysis, Remove reference), Help.
  - **Reference waveform strip**: overview, short-term loudness line, loop region (drag to create, drag the edges to
    adjust, double-click clears), playhead, click to seek, time ruler.
  - **Tonal balance**: spectrum overlay + six band difference bars (+-6 dB, coloured by severity) with a comparative
    word each: heavier / lighter, boomier / thinner, muddier / colder, honkier / more scooped, harsher / more distant,
    brighter / duller, or close.
  - **Loudness & dynamics**: mix vs reference for integrated, short-term (the reference's as heard,
    `ViewData::referenceShortTermLufs`), true peak, PLR and LRA, with a plain-language line each.
  - **Stereo image**: width per band mix vs reference, phase correlation of both, bass mono badge (OK >= 0.8, a bit
    wide >= 0.5, too wide below).
  - **What to try**: the hints from `Comparison::hints`, most important first. The first is a full-width card; the
    others are cards in one or two columns when they all fit, else one-line rows whose advice shows as a tooltip and
    opens on a click. Tags: BIG DIFFERENCE, CHECK THIS (Major checks of the mix alone: true peak, low-end mono,
    broadband phase), WORTH A LOOK, TIP, GOOD.
- Every readout has a tooltip explaining it for a beginner. A Help overlay ("?") is a short beginner's guide.
- Empty states: no reference -> a large friendly drop target ("Drop a reference track here", WAV, MP3, FLAC, AIFF, or
  click to browse); mix not played yet -> "Press play in FL Studio" guidance; loading -> progress text; error -> the
  reason, click to choose another file.
- **Keyboard focus**: never take it (`neverTakeKeyboardFocus` on the whole editor after building it, and again once a
  second for components created later). FileChooser is async (`launchAsync`) and starts in the last reference's folder
  only when it is on a local disk (never a UNC path: a dead network share would stall the message thread).
- Files can be dropped anywhere on the editor (overlay while dragging, coral for an unsupported type). Unsupported
  files, Reset analysis and a newly set loop ("play the same part of your song, then click Reset") show a short notice
  pill at the bottom for 4 s. Removing the reference also sets `listen` back to Mix.
- Right / middle clicks never act like left clicks on buttons and switches (`LeftClickOnly`, `isLeftClick`).
- The editor polls the processor at 30 Hz (`gatherViewData`), eases curves / bars / the A/B highlight towards the new
  data and repaints only what changed. The dev tool can override the ViewData (`setViewDataOverride`) for snapshots.

## Verification tools

- `build-x\CoreTests.exe [filter]`: core unit tests (substring filter on the test name).
- `build-x\RefMatchDev_artefacts\Release\RefMatchDev.exe selftest`: processor integration tests (load, switching,
  pass-through, gain matching, transport, mono, state, sample-rate change, analysis, short-term matching, listen
  without a reference, slot lock, thread safety, realtime allocation). Each run uses its own
  `%TEMP%\RefMatchSelfTest-<uuid>` folder (printed, deleted at the end), so runs can overlap. The realtime check counts
  allocations on the audio thread: global operator new / delete (all forms) plus, through the exe's import table, the
  CRT heap (malloc / calloc / realloc / free); a canary proves the hooks count before the real checks run.
- `build-x\RefMatchDev_artefacts\Release\RefMatchDev.exe snapshot out\snapshots`: editor PNGs of synthetic states at
  1100x690, 960x560 and 1600x1000 (look at them!). `07_loop_1100x690.png` is the README screenshot.
- `build-x\RefMatchDev_artefacts\Release\RefMatchDev.exe compare Example_Tracks\<mix>.wav Example_Tracks\<ref>.wav out\compare`:
  real-music run of the whole processor (readouts, hints, A/B level check, editor snapshots at 1100x690, 960x560 and
  1600x1000). Its PNGs show the user's private track names: keep them out of docs and commits.
- `tools\pluginval\pluginval.exe --strictness-level 10 --validate-in-process --validate build-x\RefMatch_artefacts\Release\VST3\RefMatch.vst3`
- `tools\clap-validator\clap-validator.exe validate build-x\RefMatch_artefacts\Release\CLAP\RefMatch.clap` (3
  state-reproducibility failures are expected: restoring a state sets Listen to Mix).
- Standalone: `build-x\RefMatch_artefacts\Release\Standalone\RefMatch.exe`
- `out\ground-truth\measurements.md`: independent measurements of the example songs (ffmpeg ebur128 + two Python
  implementations) to check RefMatch's numbers against.
