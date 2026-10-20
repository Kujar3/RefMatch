#include "core/Loudness.h"
#include "plugin/PluginProcessor.h"
#include "plugin/ReferenceTrack.h"
#include "ui/PluginEditor.h"

#include <cstdio>
#include <iostream>

// Real-music run of the whole processor (owner: processor agent).
//   RefMatchDev compare <mix> <reference> [outDir]
// Loads <reference> as the reference, plays <mix> through processBlock in 512-sample blocks with a playing fake
// playhead (like a host on the master channel), runs the analysis every ~100 ms and prints the readouts and hints.
// Then listens to the reference for 20 s to check the A/B level match. With outDir, also renders the editor at
// 1100x690, 960x560 and 1600x1000 (compare_<w>x<h>.png).

namespace juce::detail
{
// Defined in juce_events (juce_Messaging_windows.cpp); lets this console tool pump the message queue.
bool dispatchNextMessageOnSystemQueue (bool returnIfNoPendingMessages);
}

namespace
{
constexpr int kBlock = 512;
constexpr int kReadChunk = 1 << 16;

struct PlayingHead final : juce::AudioPlayHead
{
    long long time = 0;
    double sampleRate = 44100.0;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying (true);
        info.setBpm (124.0);
        info.setTimeInSamples ((juce::int64) time);
        info.setTimeInSeconds ((double) time / sampleRate);
        info.setPpqPosition ((double) time / sampleRate * 124.0 / 60.0);
        return info;
    }
};

void pumpMessages (int milliseconds)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) milliseconds;

    while (juce::Time::getMillisecondCounter() < end)
        if (! juce::detail::dispatchNextMessageOnSystemQueue (true))
            juce::Thread::sleep (2);
}

std::string number (float v, int decimals, const char* unit = "")
{
    if (! rm::hasValue (v))
        return "--";

    char buffer[64];
    std::snprintf (buffer, sizeof (buffer), "%.*f%s%s", decimals, (double) v, *unit != 0 ? " " : "", unit);
    return buffer;
}

std::string signedNumber (float v, int decimals, const char* unit = "")
{
    if (! rm::hasValue (v) || v < -150.0f)
        return "--";

    char buffer[64];
    std::snprintf (buffer, sizeof (buffer), "%+.*f%s%s", decimals, (double) v, *unit != 0 ? " " : "", unit);
    return buffer;
}

std::string difference (float mix, float ref, int decimals, const char* unit)
{
    return rm::hasValue (mix) && rm::hasValue (ref) ? signedNumber (mix - ref, decimals, unit) : std::string ("--");
}

void row (const char* label, const std::string& mix, const std::string& ref, const std::string& diff = {})
{
    std::printf ("  %-28s %16s %16s %14s\n", label, mix.c_str(), ref.c_str(), diff.c_str());
}

const char* severityName (rm::Severity s)
{
    switch (s)
    {
        case rm::Severity::Good:  return "good";
        case rm::Severity::Info:  return "info";
        case rm::Severity::Minor: return "MINOR";
        case rm::Severity::Major: return "MAJOR";
    }
    return "?";
}

const char* topicName (rm::Topic t)
{
    switch (t)
    {
        case rm::Topic::Setup:    return "setup";
        case rm::Topic::Loudness: return "loudness";
        case rm::Topic::TruePeak: return "true peak";
        case rm::Topic::Dynamics: return "dynamics";
        case rm::Topic::Tone:     return "tone";
        case rm::Topic::Stereo:   return "stereo";
        case rm::Topic::LowEnd:   return "low end";
    }
    return "?";
}

void setParam (RefMatchProcessor& p, const char* id, float plainValue)
{
    if (auto* param = p.getAPVTS().getParameter (id))
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

bool writeSnapshot (juce::Component& c, const juce::File& file)
{
    const auto image = c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
    file.deleteFile();
    juce::FileOutputStream out (file);
    juce::PNGImageFormat png;
    const bool ok = out.openedOk() && png.writeImageToStream (image, out);
    std::cout << (ok ? "  wrote " : "  FAILED ") << file.getFullPathName() << " (" << c.getWidth() << "x" << c.getHeight() << ")\n";
    return ok;
}

// Plays [start, start + numSamples) of the mix through the processor. Returns the output (stereo, interleaved per
// channel buffers) when `keepOutput`.
struct Player
{
    RefMatchProcessor& proc;
    PlayingHead& head;
    juce::AudioFormatReader& reader;
    double rate;

    juce::AudioBuffer<float> play (long long start, long long numSamples, bool keepOutput, bool analyse)
    {
        juce::AudioBuffer<float> output (2, keepOutput ? (int) numSamples : 0);
        juce::AudioBuffer<float> chunk (2, kReadChunk), block (2, kBlock);
        juce::MidiBuffer midi;
        const int analyseEvery = juce::jmax (1, (int) std::lround (rate * 0.1 / kBlock));
        int blocks = 0;

        for (long long pos = 0; pos < numSamples; pos += kReadChunk)
        {
            const int n = (int) std::min<long long> (kReadChunk, numSamples - pos);
            chunk.clear();
            reader.read (&chunk, 0, n, start + pos, true, true);

            for (int offset = 0; offset < n; offset += kBlock)
            {
                const int m = juce::jmin (kBlock, n - offset);
                block.setSize (2, m, false, false, true);
                block.copyFrom (0, 0, chunk, 0, offset, m);
                block.copyFrom (1, 0, chunk, 1, offset, m);

                head.time = start + pos + offset;
                proc.processBlock (block, midi);

                if (keepOutput)
                {
                    output.copyFrom (0, (int) (pos + offset), block, 0, 0, m);
                    output.copyFrom (1, (int) (pos + offset), block, 1, 0, m);
                }

                if (analyse && ++blocks % analyseEvery == 0)
                    proc.runAnalysisNow();
            }
        }

        if (analyse)
            proc.runAnalysisNow();

        return output;
    }
};
} // namespace

int runCompare (const juce::File& mixFile, const juce::File& referenceFile, const juce::File& outDir)
{
    if (! mixFile.existsAsFile() || ! referenceFile.existsAsFile())
    {
        std::cout << "compare: file not found: " << (mixFile.existsAsFile() ? referenceFile : mixFile).getFullPathName() << "\n";
        return 2;
    }

    juce::AudioFormatManager formats;
    rm::registerReferenceFormats (formats);
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (mixFile));

    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
    {
        std::cout << "compare: cannot decode " << mixFile.getFullPathName() << "\n";
        return 2;
    }

    const double rate = reader->sampleRate;
    const long long length = reader->lengthInSamples;
    const double t0 = juce::Time::getMillisecondCounterHiRes();

    std::cout << "RefMatch compare\n"
              << "  mix:       " << mixFile.getFileName() << "  (" << rate << " Hz, " << reader->numChannels << " ch, "
              << juce::String ((double) length / rate, 1) << " s)\n"
              << "  reference: " << referenceFile.getFileName() << "\n";

    RefMatchProcessor proc;
    PlayingHead head;
    head.sampleRate = rate;
    proc.setPlayHead (&head);
    setParam (proc, RefMatchProcessor::kParamAveraging, 1.0f);   // whole song vs whole reference
    setParam (proc, RefMatchProcessor::kParamMatch, 1.0f);       // Integrated
    setParam (proc, RefMatchProcessor::kParamListen, 0.0f);
    proc.prepareToPlay (rate, kBlock);

    proc.loadReference (referenceFile);
    if (! proc.waitUntilReferenceIdle (300000))
    {
        std::cout << "compare: reference did not finish loading\n";
        return 1;
    }

    const auto info = proc.getReferenceInfo();
    if (info.state != RefMatchProcessor::ReferenceState::Ready)
    {
        std::cout << "compare: reference error: " << info.error << "\n";
        return 1;
    }

    const double tLoaded = juce::Time::getMillisecondCounterHiRes();
    std::cout << "  reference loaded: " << juce::String (info.lengthSeconds, 1) << " s, " << info.fileSampleRate
              << " Hz, " << info.fileChannels << " ch (load + analysis " << juce::String ((tLoaded - t0) / 1000.0, 2) << " s)\n";

    Player player { proc, head, *reader, rate };
    player.play (0, length, false, true);
    const double tPlayed = juce::Time::getMillisecondCounterHiRes();
    std::cout << "  mix played through the processor in " << juce::String ((tPlayed - tLoaded) / 1000.0, 2) << " s ("
              << juce::String ((double) length / rate / ((tPlayed - tLoaded) / 1000.0), 0) << "x realtime)\n";

    const auto mix = proc.getMixProfile();
    const auto ref = proc.getReferenceProfile();
    const auto cmp = proc.getComparison();
    const float targetGain = rm::referenceMatchGainDb (mix.integratedLufs, ref.integratedLufs);

    std::printf ("\nLOUDNESS & DYNAMICS                              mix        reference     mix - ref\n");
    row ("Integrated loudness", number (mix.integratedLufs, 1, "LUFS"), number (ref.integratedLufs, 1, "LUFS"), difference (mix.integratedLufs, ref.integratedLufs, 1, "LU"));
    row ("Short-term (last 3 s)", number (mix.shortTermLufs, 1, "LUFS"), number (ref.shortTermLufs, 1, "LUFS"), difference (mix.shortTermLufs, ref.shortTermLufs, 1, "LU"));
    row ("Loudness range (LRA)", number (mix.loudnessRangeLu, 1, "LU"), number (ref.loudnessRangeLu, 1, "LU"), signedNumber (mix.loudnessRangeLu - ref.loudnessRangeLu, 1, "LU"));
    row ("True peak", number (mix.truePeakDb, 1, "dBTP"), number (ref.truePeakDb, 1, "dBTP"), difference (mix.truePeakDb, ref.truePeakDb, 1, "dB"));
    row ("Sample peak", number (mix.samplePeakDb, 1, "dBFS"), number (ref.samplePeakDb, 1, "dBFS"), difference (mix.samplePeakDb, ref.samplePeakDb, 1, "dB"));
    row ("PLR (peak to loudness)", number (mix.plrDb, 1, "dB"), number (ref.plrDb, 1, "dB"), difference (mix.plrDb, ref.plrDb, 1, "dB"));
    row ("Crest factor", number (mix.crestDb, 1, "dB"), number (ref.crestDb, 1, "dB"), difference (mix.crestDb, ref.crestDb, 1, "dB"));
    row ("RMS", number (mix.rmsDb, 1, "dBFS"), number (ref.rmsDb, 1, "dBFS"), difference (mix.rmsDb, ref.rmsDb, 1, "dB"));
    row ("Spectral loudness", number (mix.spectralLoudnessDb, 1, "dB"), number (ref.spectralLoudnessDb, 1, "dB"), difference (mix.spectralLoudnessDb, ref.spectralLoudnessDb, 1, "dB"));
    row ("Analysed audio", number ((float) mix.analysedSeconds, 1, "s"), number ((float) ref.analysedSeconds, 1, "s"));

    std::printf ("\nTONAL BALANCE (loudness-normalised)      mix band      ref band     difference   severity\n");
    for (int b = 0; b < rm::kNumBands; ++b)
    {
        const auto& band = rm::bandInfo (b);
        const auto& bc = cmp.bands[(size_t) b];
        char label[64];
        std::snprintf (label, sizeof (label), "%s (%g-%g Hz)", band.name, (double) band.lowHz, (double) band.highHz);
        const float mixNorm = rm::hasValue (mix.bands[(size_t) b].levelDb) && rm::hasValue (mix.spectralLoudnessDb)
                                  ? mix.bands[(size_t) b].levelDb - mix.spectralLoudnessDb : rm::kNoValue;
        const float refNorm = rm::hasValue (ref.bands[(size_t) b].levelDb) && rm::hasValue (ref.spectralLoudnessDb)
                                  ? ref.bands[(size_t) b].levelDb - ref.spectralLoudnessDb : rm::kNoValue;
        std::printf ("  %-30s %13s %13s %14s   %s\n", label, signedNumber (mixNorm, 1, "dB").c_str(),
                     signedNumber (refNorm, 1, "dB").c_str(),
                     bc.valid ? signedNumber (bc.levelDiffDb, 1, "dB").c_str() : "--", bc.valid ? severityName (bc.severity) : "");
    }

    std::printf ("\nSTEREO                                           mix        reference     mix - ref\n");
    row ("Width (broadband)", number (mix.widthPct, 0, "%"), number (ref.widthPct, 0, "%"), signedNumber (mix.widthPct - ref.widthPct, 0, "%"));
    for (int b = 0; b < rm::kNumBands; ++b)
    {
        char label[64];
        std::snprintf (label, sizeof (label), "Width %s", rm::bandInfo (b).name);
        row (label, number (mix.bands[(size_t) b].widthPct, 0, "%"), number (ref.bands[(size_t) b].widthPct, 0, "%"),
             signedNumber (mix.bands[(size_t) b].widthPct - ref.bands[(size_t) b].widthPct, 0, "%"));
    }
    row ("Correlation (broadband)", number (mix.correlation, 2), number (ref.correlation, 2));
    row ("Low-end correlation (<120 Hz)", number (mix.lowEndCorrelation, 2), number (ref.lowEndCorrelation, 2));
    row ("Mono", mix.isMono ? "yes" : "no", ref.isMono ? "yes" : "no");

    std::printf ("\nMATCHING\n");
    std::printf ("  Matching gain (Integrated): target %s, applied %s (reference turned %s)\n",
                 signedNumber (targetGain, 1, "dB").c_str(), signedNumber (proc.getMatchGainDb(), 1, "dB").c_str(),
                 targetGain < 0.0f ? "down" : "up");
    std::printf ("  Comparison: mix ready %s, reference ready %s\n", cmp.mixReady ? "yes" : "no", cmp.referenceReady ? "yes" : "no");

    std::printf ("\nWHAT TO TRY (%d hints)\n", (int) cmp.hints.size());
    for (const auto& h : cmp.hints)
    {
        std::printf ("  [%s/%s] %s\n", severityName (h.severity), topicName (h.topic), h.title.c_str());
        if (! h.detail.empty())
            std::printf ("      %s\n", h.detail.c_str());
    }

    // Editor snapshots, before the A/B check below adds more audio to the analysis: they show the readouts above.
    bool ok = true;

    if (outDir != juce::File())
    {
        std::printf ("\nSNAPSHOTS\n");
        outDir.createDirectory();
        std::unique_ptr<juce::AudioProcessorEditor> base (proc.createEditor());

        if (auto* editor = dynamic_cast<RefMatchEditor*> (base.get()))
        {
            const juce::Point<int> sizes[] = { { RefMatchEditor::kDefaultWidth, RefMatchEditor::kDefaultHeight },
                                               { RefMatchEditor::kMinWidth, RefMatchEditor::kMinHeight },
                                               { RefMatchEditor::kMaxWidth, RefMatchEditor::kMaxHeight } };
            for (auto s : sizes)
            {
                editor->setSize (s.x, s.y);
                editor->refreshNow();
                pumpMessages (250);
                editor->refreshNow();
                ok &= writeSnapshot (*editor, outDir.getChildFile ("compare_" + juce::String (s.x) + "x" + juce::String (s.y) + ".png"));
            }
        }
        else
        {
            std::cout << "  editor not available, no snapshots\n";
        }
    }

    // A/B check: listen to the reference for 20 s from 1/3 into the song (the reference follows the host position,
    // so the same 20 s of it play) and measure what actually comes out, with Integrated and with Short-term matching.
    {
        const long long abStart = juce::jmin (length / 3, juce::jmax (0LL, length - (long long) (20.0 * rate)));
        const long long abLength = juce::jmin ((long long) (20.0 * rate), length - abStart);
        const long long settle = juce::jmin (abLength / 2, (long long) (3.0 * rate));   // fades, gain ramp, 3 s window
        const int measured = (int) (abLength - settle);

        juce::AudioBuffer<float> mixSection (2, (int) abLength);
        reader->read (&mixSection, 0, (int) abLength, abStart, true, true);
        const float mixSectionLufs = rm::measureIntegratedLufs (mixSection.getReadPointer (0, (int) settle),
                                                                mixSection.getReadPointer (1, (int) settle), measured, rate);

        float refSectionLufs = rm::kNoValue;
        if (const auto track = proc.getReferenceTrack(); track != nullptr && abStart + abLength <= track->numSamples())
            refSectionLufs = rm::measureIntegratedLufs (track->audio.getReadPointer (0, (int) (abStart + settle)),
                                                        track->audio.getReadPointer (1, (int) (abStart + settle)), measured, rate);

        std::printf ("\nA/B CHECK (Listen = Reference, %.0f s from %.0f s; measured after %.0f s)\n", (double) abLength / rate,
                     (double) abStart / rate, (double) settle / rate);
        std::printf ("  mix section %s, reference section unmatched %s\n", number (mixSectionLufs, 1, "LUFS").c_str(),
                     number (refSectionLufs, 1, "LUFS").c_str());

        setParam (proc, RefMatchProcessor::kParamListen, 1.0f);

        for (const int mode : { 1, 2 })
        {
            setParam (proc, RefMatchProcessor::kParamMatch, (float) mode);
            const auto output = player.play (abStart, abLength, true, true);
            const float heard = rm::measureIntegratedLufs (output.getReadPointer (0, (int) settle),
                                                           output.getReadPointer (1, (int) settle), measured, rate);
            std::printf ("  %-11s matching: reference heard at %s (gain now %s)\n", mode == 1 ? "Integrated" : "Short-term",
                         number (heard, 1, "LUFS").c_str(), signedNumber (proc.getMatchGainDb(), 1, "dB").c_str());
        }

        setParam (proc, RefMatchProcessor::kParamListen, 0.0f);
        setParam (proc, RefMatchProcessor::kParamMatch, 1.0f);
    }

    std::cout << "\n  total time " << juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 2) << " s\n";

    proc.releaseResources();
    return ok ? 0 : 1;
}
