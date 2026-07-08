/*
  ==============================================================================

    StemSeparator.h

    Model 1 of the AI pipeline: full-mix -> isolated stems, so users can feed
    Axiom a whole song and reconstruct just the synth line.

    The separation network (Demucs/BS-RoFormer class, exported to ONNX) is a
    large downloadable model ("stem_separator" in the ModelRegistry). The
    interface is in place and the UI reports availability; the DSP-side STFT
    chunking/overlap-add harness lands with the model integration milestone
    (see docs/ARCHITECTURE.md, M4). Until then users import pre-isolated
    stems, which the rest of the pipeline handles fully.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "InferenceEngine.h"

namespace axiom
{

enum class StemType { Vocals, Drums, Bass, Synth, Guitar, Piano, Other };

class IStemSeparator
{
public:
    virtual ~IStemSeparator() = default;

    virtual bool isAvailable() const = 0;

    /** Splits a mix into stems on a background thread. `progress` is polled
        by the UI (0..1). Returns one buffer per requested stem or empty on
        failure/unavailability. */
    virtual std::vector<juce::AudioBuffer<float>>
        separate (const juce::AudioBuffer<float>& mix, double sampleRate,
                  const std::vector<StemType>& wanted,
                  std::atomic<float>& progress) = 0;
};

/** Placeholder implementation reporting model availability from the registry;
    returns empty results until the ONNX separation harness ships. */
class StemSeparator final : public IStemSeparator
{
public:
    explicit StemSeparator (IInferenceEngine& engineToUse) : engine (engineToUse) {}

    bool isAvailable() const override
    {
        return engine.isModelAvailable ("stem_separator");
    }

    std::vector<juce::AudioBuffer<float>>
        separate (const juce::AudioBuffer<float>&, double,
                  const std::vector<StemType>&, std::atomic<float>& progress) override
    {
        progress = 1.0f;
        return {};
    }

private:
    IInferenceEngine& engine;
};

} // namespace axiom
