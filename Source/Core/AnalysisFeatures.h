/*
  ==============================================================================

    AnalysisFeatures.h

    The feature vector extracted from an imported sample by the analysis
    pipeline (Analysis/AudioAnalyzer). This struct is the *only* interface
    between DSP analysis and instrument reconstruction (AI/Reconstructor):
    both the analytical estimator and future ONNX parameter-prediction
    models consume exactly this.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <vector>

namespace axiom
{

struct AnalysisFeatures
{
    // --- Source info -------------------------------------------------------
    double sampleRate      = 0.0;
    double lengthSeconds   = 0.0;
    int    numChannels     = 0;

    // --- Pitch -------------------------------------------------------------
    float  f0Hz            = 0.0f;   // fundamental (median over sustain)
    int    rootMidiNote    = 60;
    float  pitchConfidence = 0.0f;   // 0..1 (YIN periodicity)
    float  pitchStability  = 0.0f;   // 0..1 (1 = rock solid)

    // --- Harmonic profile ----------------------------------------------------
    // Magnitudes of harmonics 1..N in dB relative to the strongest harmonic.
    std::vector<float> harmonicDb;
    float  oddEvenRatio    = 0.5f;   // odd energy / (odd + even), 0.5 = balanced
    float  slopeDbPerHarm  = -6.0f;  // fitted rolloff per harmonic doubling
    float  inharmonicity   = 0.0f;   // deviation of peaks from k*f0, 0..1

    // --- Spectral shape ------------------------------------------------------
    float  spectralCentroidHz = 0.0f;
    float  spectralFlatness   = 0.0f; // 0 tonal .. 1 noise
    float  spectralRolloffHz  = 0.0f; // 95% energy point
    float  noisiness          = 0.0f; // non-harmonic energy ratio 0..1

    // --- Envelope (measured from RMS contour) -------------------------------
    float  attackSec   = 0.005f;
    float  decaySec    = 0.2f;
    float  sustainLvl  = 0.8f;    // 0..1 relative to peak; ~0 for one-shots
    float  releaseSec  = 0.3f;
    bool   isSustained = true;    // false = one-shot / percussive

    // --- Modulation ----------------------------------------------------------
    float  vibratoRateHz     = 0.0f;
    float  vibratoDepthCents = 0.0f;
    float  centroidModRateHz = 0.0f; // periodic brightness movement (filter LFO)
    float  centroidModDepth  = 0.0f; // 0..1

    // --- Unison / stereo -----------------------------------------------------
    float  detuneCentsEst = 0.0f;  // unison spread estimate from peak widening
    float  stereoWidth    = 0.0f;  // 1 - |correlation(L,R)|, 0..1

    // --- Dynamics ------------------------------------------------------------
    float  dynamicRangeDb = 0.0f;
    float  transientSharpness = 0.0f; // 0..1, crest of the first 30 ms

    bool isValid() const noexcept   { return f0Hz > 0.0f || noisiness > 0.9f; }
};

} // namespace axiom
