/*
  ==============================================================================

    DdspResynth.h

    Real-time DDSP harmonic-plus-noise synthesizer — the playback half of
    the DDSP tier (control frames come from AI/DdspEncoder on the analysis
    thread; see Core/DdspTimbre.h).

    One instance lives inside each Voice. Per control tick (16 samples) it
    interpolates the timbre's harmonic/noise frames at the current playhead
    and retargets per-partial amplitude ramps; per sample it sums up to 64
    sine partials from a shared lookup table at k * (played fundamental) —
    the source spectrum re-pitched to whatever key is held, alias-free
    because the partial count is capped below Nyquist per tick — plus white
    noise through a low-pass tracking the residual's measured cutoff.

    Held notes ping-pong the playhead across the timbre's sustain loop so
    the spectral motion never freezes or snaps; one-shots play straight
    through and hold their final frame.

    Audio-thread hot path: allocation-free, lock-free, noexcept throughout.

  ==============================================================================
*/

#pragma once

#include <array>
#include <cmath>

#include "../Core/DdspTimbre.h"
#include "SynthModules.h"

namespace axiom::dsp
{

//==============================================================================
/** Shared 2048-point sine table (wrap-guarded for lerp). Built once at static
    initialization, read-only afterwards — safe from every audio thread. */
struct DdspSineTable
{
    static constexpr int size = 2048;

    DdspSineTable() noexcept
    {
        for (int i = 0; i <= size; ++i)
            t[(size_t) i] = (float) std::sin (6.28318530718 * (double) i / (double) size);
    }

    /** phase in [0, 1). */
    float operator() (float phase) const noexcept
    {
        const float pos  = phase * (float) size;
        const int   idx  = (int) pos;
        const float frac = pos - (float) idx;
        return t[(size_t) idx] + (t[(size_t) idx + 1] - t[(size_t) idx]) * frac;
    }

    std::array<float, size + 1> t {};
};

inline const DdspSineTable ddspSineTable;

//==============================================================================
class DdspResynth
{
public:
    static constexpr int maxHarmonics = DdspTimbre::maxHarmonics;

    void reset() noexcept
    {
        for (int k = 0; k < maxHarmonics; ++k)
        {
            // Golden-ratio start phases: same crest-factor trick as the
            // spectral wavetables — steady-state timbre is phase blind, but
            // scrambling keeps the additive stack out of clipping range.
            const float x = (float) (k + 1) * 0.6180339887f;
            phase[(size_t) k] = x - std::floor (x);
            inc[(size_t) k]   = 0.0f;
            amp[(size_t) k]   = 0.0f;
            step[(size_t) k]  = 0.0f;
        }
        renderK   = 0;
        prevKMax  = 0;
        playhead  = 0.0f;
        direction = 1.0f;
        noiseAmp  = 0.0f;
        noiseStep = 0.0f;
        noiseFilt.reset();
    }

    /** Control-rate update (call every `numSamples` = 16 samples): retunes
        the partial bank to `hz`, retargets amplitude ramps from the frame
        pair under the playhead, and advances/loops the playhead. */
    void controlUpdate (const DdspTimbre& t, float hz, double sampleRate,
                        int numSamples) noexcept
    {
        const float inc0 = (float) ((double) hz / sampleRate);
        const float rn   = 1.0f / (float) numSamples;

        if (inc0 <= 1.0e-9f || inc0 >= 0.5f || ! t.isValid())
        {
            for (int k = 0; k < renderK; ++k)
                step[(size_t) k] = -amp[(size_t) k] * rn;   // fade out, keep rendering one tick
            noiseStep = -noiseAmp * rn;
            return;
        }

        // Alias-free by construction: only partials below Nyquist exist.
        const int kMax = juce::jmin (t.numHarmonics, maxHarmonics,
                                     (int) (0.5f / inc0));

        // Interpolate the frame pair under the playhead.
        const int   last = t.numFrames - 1;
        const float fp   = juce::jlimit (0.0f, (float) last, playhead);
        const int   i0   = (int) fp;
        const int   i1   = juce::jmin (i0 + 1, last);
        const float frac = fp - (float) i0;

        const float* a = t.frame (i0);
        const float* b = t.frame (i1);

        for (int k = 0; k < kMax; ++k)
        {
            inc[(size_t) k] = inc0 * (float) (k + 1);
            const float target = a[k] + (b[k] - a[k]) * frac;
            step[(size_t) k] = (target - amp[(size_t) k]) * rn;
        }
        // Partials that just crossed above Nyquist (pitch rose) ramp to
        // silence over this tick, then drop out of the render loop on the
        // next one (renderK covers new + fading, prevKMax remembers the set
        // that still carries amplitude).
        for (int k = kMax; k < prevKMax; ++k)
            step[(size_t) k] = -amp[(size_t) k] * rn;
        renderK  = juce::jmax (kMax, prevKMax);
        prevKMax = kMax;

        // Noise layer: gain ramp + filter retune.
        {
            const float gTarget = t.noiseGain[(size_t) i0]
                                + (t.noiseGain[(size_t) i1] - t.noiseGain[(size_t) i0]) * frac;
            noiseStep = (gTarget - noiseAmp) * rn;

            const float cutoff = t.noiseCutoffHz[(size_t) i0]
                               + (t.noiseCutoffHz[(size_t) i1] - t.noiseCutoffHz[(size_t) i0]) * frac;
            noiseFilt.setParams (cutoff, 0.15f, sampleRate);
        }

        // Advance the playhead in frames; sustained sounds ping-pong across
        // the loop (reflection keeps the motion continuous), one-shots park
        // on their final frame.
        const float adv = (float) ((double) numSamples / sampleRate) * t.frameRate;
        if (t.oneShot)
        {
            playhead = juce::jmin (playhead + adv, (float) last);
        }
        else
        {
            const float lo = (float) t.loopStartFrame;
            const float hi = (float) juce::jmax (t.loopStartFrame + 1, t.loopEndFrame);
            playhead += adv * direction;
            if (playhead >= hi)
            {
                playhead  = juce::jmax (lo, hi - (playhead - hi));
                direction = -1.0f;
            }
            else if (direction < 0.0f && playhead <= lo)
            {
                playhead  = juce::jmin (hi, lo + (lo - playhead));
                direction = 1.0f;
            }
        }
    }

    /** One mono sample of the harmonic stack + filtered noise. */
    float renderSample (NoiseGen& noise) noexcept
    {
        float acc = 0.0f;
        for (int k = 0; k < renderK; ++k)
        {
            amp[(size_t) k] += step[(size_t) k];
            float& ph = phase[(size_t) k];
            acc += amp[(size_t) k] * ddspSineTable (ph);
            ph += inc[(size_t) k];
            if (ph >= 1.0f) ph -= 1.0f;
        }
        noiseAmp += noiseStep;
        acc += noiseFilt.process (noise.next(), FilterType::LowPass) * noiseAmp;
        return acc;
    }

private:
    std::array<float, maxHarmonics> phase {};
    std::array<float, maxHarmonics> inc {};
    std::array<float, maxHarmonics> amp {};
    std::array<float, maxHarmonics> step {};

    int    renderK  = 0;      // partials rendered this tick (new + fading)
    int    prevKMax = 0;      // partials still carrying amplitude

    float  playhead  = 0.0f;  // fractional frame index
    float  direction = 1.0f;  // ping-pong sign

    float  noiseAmp  = 0.0f;
    float  noiseStep = 0.0f;
    TptSvf noiseFilt;
};

} // namespace axiom::dsp
