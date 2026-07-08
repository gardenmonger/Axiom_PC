/*
  ==============================================================================

    StemSeparation.h

    Model 1 of the AI pipeline: full mix -> isolated stems.

    Two tiers behind one call (same philosophy as reconstruction):

      Neural  — an ONNX "stem_separator" model (Demucs / BS-RoFormer class)
                run in overlapping chunks with linear crossfade reassembly.
                Tensor contract v1: input  float32 [1, 2, T]
                                    output float32 [1, S, 2, T]
                Stem names / model sample rate / chunking come from an
                optional sidecar manifest `stem_separator.json` in the model
                folder; defaults match htdemucs (drums/bass/other/vocals,
                44.1 kHz). Models are drop-in — no recompile.

      HPSS    — median-filtering harmonic/percussive separation (Fitzgerald
                2010) as the always-available DSP fallback: harmonic content
                is smooth along time, percussive along frequency; soft Wiener
                masks split the STFT. Two stems: Harmonic / Percussive.

    Runs on the separation worker thread only. Progress is written to an
    atomic polled by the UI; `shouldAbort` is checked per chunk/frame.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "InferenceEngine.h"

namespace axiom
{

struct Stem
{
    juce::String name;
    juce::AudioBuffer<float> audio;
};

class StemSeparationEngine
{
public:
    /** Human-readable tier that separate() would use right now. */
    static juce::String backendName (const IInferenceEngine& engine);

    /** Splits `mix` into stems at `sampleRate`. Blocking; call from a worker
        thread. Returns an empty vector only on abort or hard failure. */
    static std::vector<Stem> separate (const juce::AudioBuffer<float>& mix,
                                       double sampleRate,
                                       IInferenceEngine& engine,
                                       std::atomic<float>& progress,
                                       const std::function<bool()>& shouldAbort);

private:
    static std::vector<Stem> separateNeural (const juce::AudioBuffer<float>& mix,
                                             double sampleRate,
                                             IInferenceEngine& engine,
                                             std::atomic<float>& progress,
                                             const std::function<bool()>& shouldAbort);

    static std::vector<Stem> separateHpss (const juce::AudioBuffer<float>& mix,
                                           double sampleRate,
                                           std::atomic<float>& progress,
                                           const std::function<bool()>& shouldAbort);
};

} // namespace axiom
