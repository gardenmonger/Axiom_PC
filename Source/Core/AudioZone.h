/*
  ==============================================================================

    AudioZone.h

    A user-selected slice of the active audio: start/end markers isolate the
    region the AI learns from, raised-cosine fade-in/out lengths remove edge
    pops and clicks. Shared by the waveform editor (GUI), the analysis
    pipeline (processor crops + fades before analysis) and session state.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace axiom
{

struct AudioZone
{
    double startSec   = 0.0;
    double endSec     = 0.0;    // <= 0 means "to the end"
    double fadeInSec  = 0.01;
    double fadeOutSec = 0.02;

    double effectiveEnd (double totalLen) const noexcept
    {
        return endSec > 0.0 ? juce::jmin (endSec, totalLen) : totalLen;
    }

    bool isFullLength (double totalLen) const noexcept
    {
        return startSec <= 0.001 && effectiveEnd (totalLen) >= totalLen - 0.001;
    }

    /** Copies the zone out of `source` with raised-cosine edge fades applied. */
    static juce::AudioBuffer<float> extract (const juce::AudioBuffer<float>& source,
                                             double sampleRate, const AudioZone& zone)
    {
        const double totalLen = source.getNumSamples() / sampleRate;
        const int start = juce::jlimit (0, source.getNumSamples() - 1,
                                        (int) (zone.startSec * sampleRate));
        const int end   = juce::jlimit (start + 1, source.getNumSamples(),
                                        (int) (zone.effectiveEnd (totalLen) * sampleRate));
        const int len   = end - start;

        juce::AudioBuffer<float> out (source.getNumChannels(), len);
        for (int ch = 0; ch < source.getNumChannels(); ++ch)
            out.copyFrom (ch, 0, source, ch, start, len);

        const int fadeIn  = juce::jlimit (0, len / 2, (int) (zone.fadeInSec  * sampleRate));
        const int fadeOut = juce::jlimit (0, len / 2, (int) (zone.fadeOutSec * sampleRate));

        for (int ch = 0; ch < out.getNumChannels(); ++ch)
        {
            float* d = out.getWritePointer (ch);
            for (int i = 0; i < fadeIn; ++i)
            {
                const float g = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi
                                                        * (float) i / (float) fadeIn);
                d[i] *= g;
            }
            for (int i = 0; i < fadeOut; ++i)
            {
                const float g = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi
                                                        * (float) i / (float) fadeOut);
                d[len - 1 - i] *= g;
            }
        }
        return out;
    }
};

} // namespace axiom
