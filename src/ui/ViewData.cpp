#include "ui/ViewData.h"

namespace rm::ui
{
ViewData gatherViewData (RefMatchProcessor& p)
{
    ViewData d;
    d.mix = p.getMixProfile();
    d.reference = p.getReferenceProfile();
    d.comparison = p.getComparison();
    d.referenceInfo = p.getReferenceInfo();
    d.track = p.getReferenceTrack();
    d.playheadSeconds = p.getReferencePlayheadSeconds();
    d.referenceShortTermLufs = p.getReferenceShortTermLufs();
    d.matchGainDb = p.getMatchGainDb();
    d.listenPosition = p.getListenPosition();
    d.hostPlaying = p.isHostPlaying();
    d.referenceSounding = p.isReferenceSounding();
    d.analysisVersion = p.getAnalysisVersion();

    auto& apvts = p.getAPVTS();
    auto value = [&apvts] (const char* id)
    {
        auto* v = apvts.getRawParameterValue (id);
        return v != nullptr ? v->load() : 0.0f;
    };

    d.listenReference = value (RefMatchProcessor::kParamListen) > 0.5f;
    d.matchMode = (rm::MatchMode) juce::jlimit (0, 2, (int) std::lround (value (RefMatchProcessor::kParamMatch)));
    d.averaging = (rm::Averaging) juce::jlimit (0, 1, (int) std::lround (value (RefMatchProcessor::kParamAveraging)));
    d.mono = value (RefMatchProcessor::kParamMono) > 0.5f;
    d.freeRun = value (RefMatchProcessor::kParamFreeRun) > 0.5f;
    return d;
}
} // namespace rm::ui
