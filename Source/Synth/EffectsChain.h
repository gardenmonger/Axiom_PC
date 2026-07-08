/*
  ==============================================================================

    EffectsChain.h

    Post-voice stereo effects rack:

        saturation -> chorus -> stereo delay -> reverb -> width (M/S) -> gain

    All memory is allocated in prepare(); process() is allocation- and
    lock-free. Delay time changes are slewed (one-pole toward target) to
    avoid zipper clicks; reverb/chorus parameters are set per block.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <vector>

#include "../Core/InstrumentPatch.h"

namespace axiom
{

class EffectsChain
{
public:
    void prepare (double sampleRate, int maxBlockSize, int numChannels = 2)
    {
        sr = sampleRate;

        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlockSize,
                                      (juce::uint32) numChannels };
        chorus.prepare (spec);
        chorus.setCentreDelay (7.0f);
        chorus.setFeedback (0.0f);

        reverb.setSampleRate (sampleRate);

        const int delayCapacity = (int) (2.0 * sampleRate) + 8;
        for (auto& d : delayBuf)
            d.assign ((size_t) delayCapacity, 0.0f);
        delayWrite = 0;
        delaySamplesSmoothed = (float) (0.35 * sampleRate);

        gainSmoothed.reset (sampleRate, 0.02);
        reset();
    }

    void reset()
    {
        chorus.reset();
        reverb.reset();
        for (auto& d : delayBuf)
            std::fill (d.begin(), d.end(), 0.0f);
        chorusWasOn = delayWasOn = reverbWasOn = false;
    }

    /** `numSamples` may be smaller than the buffer's allocated size — only
        that many samples are processed. Passing the allocated size for a
        short host block would cycle stale samples through the delay/reverb
        state and feed them back into the audible region. */
    void process (juce::AudioBuffer<float>& buffer, const FxParams& fx, int numSamples) noexcept
    {
        const int n = juce::jmin (numSamples, buffer.getNumSamples());
        if (n <= 0 || buffer.getNumChannels() < 2)
            return;

        float* l = buffer.getWritePointer (0);
        float* r = buffer.getWritePointer (1);

        // --- Saturation ------------------------------------------------------
        if (fx.satDrive > 0.001f)
        {
            const float pre  = 1.0f + 6.0f * fx.satDrive;
            const float post = 1.0f / std::tanh (pre * 0.8f);
            for (int i = 0; i < n; ++i)
            {
                l[i] = std::tanh (l[i] * pre * 0.8f) * post * 0.8f;
                r[i] = std::tanh (r[i] * pre * 0.8f) * post * 0.8f;
            }
        }

        // --- Chorus ----------------------------------------------------------
        const bool chorusOn = fx.chorusMix > 0.001f;
        if (chorusOn)
        {
            if (! chorusWasOn)
                chorus.reset();               // don't replay a stale mod buffer

            chorus.setRate (juce::jlimit (0.05f, 10.0f, fx.chorusRateHz));
            chorus.setDepth (juce::jlimit (0.0f, 1.0f, fx.chorusDepth));
            chorus.setMix (juce::jlimit (0.0f, 1.0f, fx.chorusMix));

            juce::dsp::AudioBlock<float> block (buffer);
            auto sub = block.getSubBlock (0, (size_t) n);
            juce::dsp::ProcessContextReplacing<float> ctx (sub);
            chorus.process (ctx);
        }
        chorusWasOn = chorusOn;

        // --- Stereo delay (cross-fed ping-pong flavour) -----------------------
        const bool delayOn = fx.delayMix > 0.001f;
        if (delayOn)
        {
            if (! delayWasOn)                 // don't replay echoes from last time
                for (auto& d : delayBuf)
                    std::fill (d.begin(), d.end(), 0.0f);

            const float targetSamples = juce::jlimit (1.0f, (float) delayBuf[0].size() - 4.0f,
                                                      fx.delayTimeMs * 0.001f * (float) sr);
            const float fb  = juce::jlimit (0.0f, 0.9f, fx.delayFeedback);
            const float mix = juce::jlimit (0.0f, 1.0f, fx.delayMix);
            const int   cap = (int) delayBuf[0].size();

            for (int i = 0; i < n; ++i)
            {
                delaySamplesSmoothed += 0.0005f * (targetSamples - delaySamplesSmoothed);

                const float readPos = (float) delayWrite - delaySamplesSmoothed;
                const int   idx0    = ((int) readPos % cap + cap) % cap;
                const int   idx1    = (idx0 + 1) % cap;
                const float frac    = readPos - std::floor (readPos);

                const float dl = delayBuf[0][(size_t) idx0] * (1.0f - frac)
                               + delayBuf[0][(size_t) idx1] * frac;
                const float dr = delayBuf[1][(size_t) idx0] * (1.0f - frac)
                               + delayBuf[1][(size_t) idx1] * frac;

                // Cross feedback L<->R for width.
                delayBuf[0][(size_t) delayWrite] = l[i] + dr * fb;
                delayBuf[1][(size_t) delayWrite] = r[i] + dl * fb;
                delayWrite = (delayWrite + 1) % cap;

                l[i] += dl * mix;
                r[i] += dr * mix;
            }
        }
        delayWasOn = delayOn;

        // --- Reverb ------------------------------------------------------------
        const bool reverbOn = fx.reverbMix > 0.001f;
        if (reverbOn)
        {
            if (! reverbWasOn)
                reverb.reset();               // don't replay a stale tail

            juce::Reverb::Parameters rp;
            rp.roomSize   = juce::jlimit (0.0f, 1.0f, fx.reverbSize);
            rp.damping    = 0.45f;
            rp.wetLevel   = juce::jlimit (0.0f, 1.0f, fx.reverbMix) * 0.8f;
            rp.dryLevel   = 1.0f;
            rp.width      = 1.0f;
            reverb.setParameters (rp);
            reverb.processStereo (l, r, n);
        }
        reverbWasOn = reverbOn;

        // --- Width (mid/side) ---------------------------------------------------
        {
            const float w = juce::jlimit (0.0f, 2.0f, fx.stereoWidth);
            if (std::abs (w - 1.0f) > 0.01f)
            {
                for (int i = 0; i < n; ++i)
                {
                    const float mid  = 0.5f * (l[i] + r[i]);
                    const float side = 0.5f * (l[i] - r[i]) * w;
                    l[i] = mid + side;
                    r[i] = mid - side;
                }
            }
        }

        // --- Output gain -----------------------------------------------------------
        gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (fx.gainDb));
        for (int i = 0; i < n; ++i)
        {
            const float g = gainSmoothed.getNextValue();
            l[i] *= g;
            r[i] *= g;
        }

        // --- Soft-clip safety --------------------------------------------------
        // Chord peaks that exceed full scale would hard-clip in the host and
        // read as pops. Transparent below the knee, tanh above, asymptote 1.0.
        for (int i = 0; i < n; ++i)
        {
            l[i] = softClip (l[i]);
            r[i] = softClip (r[i]);
        }
    }

private:
    static float softClip (float x) noexcept
    {
        constexpr float knee = 0.8f;
        const float a = std::abs (x);
        if (a <= knee)
            return x;
        const float y = knee + (1.0f - knee) * std::tanh ((a - knee) / (1.0f - knee));
        return x < 0.0f ? -y : y;
    }

    double sr = 44100.0;

    juce::dsp::Chorus<float> chorus;
    juce::Reverb             reverb;

    std::array<std::vector<float>, 2> delayBuf;
    int   delayWrite = 0;
    float delaySamplesSmoothed = 0.0f;

    // Off->on edge detection so a section entered mid-performance starts
    // from silence instead of replaying whatever it held when last active.
    bool chorusWasOn = false, delayWasOn = false, reverbWasOn = false;

    juce::SmoothedValue<float> gainSmoothed { 1.0f };
};

} // namespace axiom
