/*
  ==============================================================================

    DdspEncoder.h

    DDSP control-frame extraction: (zone-cropped source audio, features)
    -> DdspTimbre (per-frame 64-harmonic distribution + filtered-noise
    gain/cutoff + loudness). Runs on the analysis thread right after
    reconstruction; the audio thread only ever sees the finished block.

    Two tiers behind one call, mirroring the rest of the AI stack:

      Measured (STFT)   — short-time harmonic tracking of the source itself:
                          per frame, refine f0 around the YIN estimate, read
                          the magnitude of each harmonic peak, and treat the
                          remaining spectrum as the filtered-noise layer.
                          Deterministic, always available.
      Neural (ONNX)     — optional "ddsp_decoder" model slot in the shared
                          inference engine (same runtime instance and model
                          folder as stem_separator / patch_refiner). Tensor
                          contract, chosen so the model can be trained by
                          inverse synthesis against Axiom's own resynth:

                              input  [1, T, 2]  per frame: (midiPitch/127,
                                                            loudness 0..1)
                              output [1, T, 66] per frame: 64 linear harmonic
                                     amplitudes, noise gain, noise cutoff
                                     (0..1 -> 20 Hz * 2^(10x) log map)

                          Drop ddsp_decoder.onnx into the model folder to
                          activate — no recompile, like every other slot.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "../Core/AnalysisFeatures.h"
#include "../Core/DdspTimbre.h"
#include "InferenceEngine.h"

namespace axiom
{

class DdspEncoder
{
public:
    static constexpr const char* modelSlotName = "ddsp_decoder";

    /** Blocking; call from the analysis thread. Returns an invalid
        (default) timbre when the source has no stable pitch, is too short,
        or `shouldAbort` fires. */
    static DdspTimbre extract (const juce::AudioBuffer<float>& audio,
                               double sampleRate,
                               const AnalysisFeatures& features,
                               IInferenceEngine& inference,
                               const std::function<bool()>& shouldAbort = {});
};

} // namespace axiom
