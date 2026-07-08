/*
  ==============================================================================

    Reconstructor.h

    Instrument reconstruction: AnalysisFeatures -> InstrumentPatch.

    This is the "Patch Discovery" stage — Axiom does not resynthesize audio
    directly; it infers the SYNTHESIS RECIPE that would produce the analyzed
    sound (oscillator stack, filter, envelopes, modulation, effects) and hands
    that recipe to the synth engine. The recipe stays fully editable by the
    user afterwards.

    Two tiers behind one interface:

      AnalyticalReconstructor  — deterministic DSP heuristics. Explainable,
                                 fast, always available. Ships in v1.
      HybridReconstructor      — routes through an ONNX "patch_refiner" model
                                 when one is installed, otherwise falls back
                                 to the analytical tier. The model consumes
                                 the exact same AnalysisFeatures (packed by
                                 packFeatureVector) so it can be trained
                                 offline against patches rendered by Axiom's
                                 own synth engine (self-supervised inverse
                                 synthesis) and dropped in later without an
                                 app update.

  ==============================================================================
*/

#pragma once

#include "../Core/AnalysisFeatures.h"
#include "../Core/InstrumentPatch.h"
#include "InferenceEngine.h"

namespace axiom
{

//==============================================================================
class IReconstructor
{
public:
    virtual ~IReconstructor() = default;

    /** Infers a playable patch from extracted features. Call from a background
        thread. Always returns a usable patch; `patch.confidence` conveys how
        trustworthy the reconstruction is. */
    virtual InstrumentPatch reconstruct (const AnalysisFeatures& features) = 0;

    /** Name of the tier that produced the last result (shown in the UI). */
    virtual juce::String getTierName() const = 0;
};

//==============================================================================
class AnalyticalReconstructor final : public IReconstructor
{
public:
    InstrumentPatch reconstruct (const AnalysisFeatures& features) override;
    juce::String getTierName() const override    { return "Analytical DSP v1"; }

private:
    struct OscClassification
    {
        OscType type       = OscType::Saw;
        float   pulseWidth = 0.5f;
        float   confidence = 0.0f;
    };

    static OscClassification classifyOscillator (const AnalysisFeatures& f);
    static void estimateFilter (const AnalysisFeatures& f, OscType oscType,
                                float pulseWidth, FilterParams& filter);
};

//==============================================================================
class HybridReconstructor final : public IReconstructor
{
public:
    explicit HybridReconstructor (IInferenceEngine& engineToUse)
        : engine (engineToUse) {}

    InstrumentPatch reconstruct (const AnalysisFeatures& features) override;
    juce::String getTierName() const override    { return lastTier; }

    /** Fixed-order feature packing shared with model training. Layout:
        [0..23]  harmonicDb (dB, -80 floor, zero-padded)
        [24..]   scalar features in declaration order of AnalysisFeatures. */
    static std::vector<float> packFeatureVector (const AnalysisFeatures& f);

private:
    IInferenceEngine& engine;
    AnalyticalReconstructor analytical;
    juce::String lastTier { "Analytical DSP v1" };
};

} // namespace axiom
