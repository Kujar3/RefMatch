#pragma once

#include "plugin/PluginProcessor.h"

// Everything the editor's panels draw, gathered from the processor once per UI tick (30 Hz) by the editor and pushed
// into each panel with setData(). Snapshots can also build one by hand (synthetic data) to render panels in states
// that are hard to reach with real audio.
namespace rm::ui
{
struct ViewData
{
    rm::TrackProfile mix;
    rm::TrackProfile reference;
    rm::Comparison comparison;

    RefMatchProcessor::ReferenceInfo referenceInfo;
    std::shared_ptr<const rm::ReferenceTrack> track;   // overview + short-term curve; nullptr when none

    double playheadSeconds = 0.0;
    float referenceShortTermLufs = rm::kNoValue;   // reference as it plays (RefMatchProcessor::getReferenceShortTermLufs)
    float matchGainDb = 0.0f;
    float listenPosition = 0.0f;     // 0 = mix .. 1 = reference
    bool listenReference = false;    // parameter value (target of the crossfade)
    bool hostPlaying = false;
    bool referenceSounding = false;
    rm::MatchMode matchMode = rm::MatchMode::Integrated;
    rm::Averaging averaging = rm::Averaging::Recent;
    bool mono = false;
    bool freeRun = true;

    int analysisVersion = 0;
};

// Fills a ViewData from the processor (message thread).
ViewData gatherViewData (RefMatchProcessor&);

} // namespace rm::ui
