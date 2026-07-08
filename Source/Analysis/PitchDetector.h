/*
  ==============================================================================

    PitchDetector.h

    Monophonic fundamental-frequency estimation using the YIN algorithm
    (de Cheveigné & Kawahara, 2002) with parabolic interpolation.

    Chosen over FFT peak-picking because YIN is robust against strong
    harmonics dominating the fundamental (common in saw/square synth stems)
    and returns a per-frame periodicity confidence we propagate all the way
    into the patch's overall reconstruction confidence.

    Runs on the analysis thread only — never on the audio thread.

  ==============================================================================
*/

#pragma once

#include <vector>

namespace axiom
{

class PitchDetector
{
public:
    struct Result
    {
        float f0Hz       = 0.0f;
        float confidence = 0.0f;   // 1 - CMNDF minimum, 0..1
    };

    PitchDetector (double sampleRate, int windowSize = 2048,
                   float minHz = 40.0f, float maxHz = 2000.0f);

    /** Analyzes one window of mono audio. `input` must hold windowSize samples. */
    Result detect (const float* input) const;

    int getWindowSize() const noexcept   { return windowSize; }

private:
    double sampleRate;
    int    windowSize;
    int    minLag, maxLag;

    mutable std::vector<float> diff;   // scratch, analysis thread only
};

} // namespace axiom
