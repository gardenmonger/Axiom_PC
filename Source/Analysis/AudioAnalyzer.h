/*
  ==============================================================================

    AudioAnalyzer.h

    Offline feature-extraction pipeline. Given an imported sample it produces
    an AnalysisFeatures vector:

        pitch track (YIN)  ->  root note, stability, vibrato
        STFT (Hann 4096)   ->  harmonic profile, centroid, flatness, rolloff,
                               noisiness, detune estimate, filter-LFO estimate
        RMS contour        ->  ADSR estimate, transient sharpness
        L/R correlation    ->  stereo width

    Heavy work: runs exclusively on the analysis thread (see AnalysisManager
    in PluginProcessor). Allocation is unrestricted here — this never touches
    the audio callback.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "../Core/AnalysisFeatures.h"

namespace axiom
{

class AudioAnalyzer
{
public:
    /** Full analysis of a (possibly stereo) sample. Blocking; call from a
        background thread. `shouldAbort` is polled between stages so a new
        import can cancel a running analysis. */
    static AnalysisFeatures analyze (const juce::AudioBuffer<float>& audio,
                                     double sampleRate,
                                     const std::function<bool()>& shouldAbort = {});

private:
    static void analyzePitch    (const float* mono, int numSamples, double sr, AnalysisFeatures& out,
                                 std::vector<float>& pitchTrackCentsOut, int& hopOut);
    static void analyzeSpectrum (const float* mono, int numSamples, double sr, AnalysisFeatures& out);
    static void analyzeEnvelope (const float* mono, int numSamples, double sr, AnalysisFeatures& out);
    static void analyzeVibrato  (const std::vector<float>& pitchTrackCents, double framesPerSecond,
                                 AnalysisFeatures& out);
    static void analyzeStereo   (const juce::AudioBuffer<float>& audio, AnalysisFeatures& out);
};

} // namespace axiom
