/*
  ==============================================================================

    SkSampler.h

    Real-time varispeed sample player — the playback half of the
    pitch-stretch tier (the audio itself comes from the analysis pipeline;
    see Core/SamplerSource.h).

    One instance lives inside each Voice. There is no time-stretching and no
    resynthesis: the read head simply moves through the source at
    (played f0 / source f0), so pitch and speed are locked together — high
    keys chirp past faster, low keys slow into slow-motion. Sustained
    sources loop their sustain region forward with a short equal-gain
    crossfade; one-shots play through once and stop.

    Two playback engines share that read head (Engine, selected per block):

      - sk1:    the original Casio SK-1 character — linear interpolation
                (aliasing and all) plus a per-voice bitcrusher (bit-depth
                quantize + zero-order sample-and-hold decimation) driven by
                one 0..1 "crush" knob: 0 is transparent, 1 is ~4-bit at a
                heavily reduced rate.

      - modern: an FL Studio-style clean resampler — 32-point windowed-sinc
                (Blackman-Harris) interpolation whose kernel widens and its
                cutoff drops by the speed ratio when transposing up, so the
                read stays band-limited and alias-free like FL's sinc
                interpolation modes. The bitcrusher is bypassed: the layer
                is transparent apart from the varispeed repitch itself
                (which is exactly what an FL sampler channel in Resample
                mode does).

    Audio-thread hot path: allocation-free, lock-free, noexcept throughout.
    Call ensureTables() once from a non-audio thread (SynthEngine::prepare)
    so the sinc table is never built under a render callback.

  ==============================================================================
*/

#pragma once

#include <array>
#include <cmath>

#include "../Core/SamplerSource.h"

namespace axiom::dsp
{

class SkSampler
{
public:
    /** Playback engine — indices match the samplerEngine choice parameter. */
    enum class Engine
    {
        sk1    = 0,   // linear interp + bitcrush, vintage lo-fi
        modern = 1    // band-limited windowed sinc, FL Studio-style clean
    };

    /** Builds the shared windowed-sinc table (idempotent). Call from a
        non-audio thread before the first render. */
    static void ensureTables() noexcept   { (void) sincTable(); }

    void reset() noexcept
    {
        pos        = 0.0;
        inc        = 0.0;
        finished   = false;
        engine     = Engine::sk1;
        crushHold  = 1.0f;
        crushPhase = 1.0e9f;    // force a fresh hold sample immediately
        crushScale = 32768.0f;
        held       = 0.0f;
    }

    bool isFinished() const noexcept   { return finished; }

    /** Control-rate update (every 16 samples): retunes the read speed to
        `hz`, selects the playback engine and maps the crush knob to hold
        length + quantize step (SK-1 engine only). */
    void controlUpdate (const SamplerSource& s, float hz, double sampleRate,
                        float crush, Engine mode) noexcept
    {
        inc = s.isValid() && hz > 0.0f && sampleRate > 0.0
                  ? ((double) hz / (double) s.rootHz) * (s.sampleRate / sampleRate)
                  : 0.0;

        engine = mode;

        // Crush knob: bit depth 16 -> ~4 bits, hold 1 -> ~40 samples
        // (squared so the lo-fi mangling arrives progressively).
        const float c = juce::jlimit (0.0f, 1.0f, crush);
        crushHold  = 1.0f + c * c * 39.0f;
        crushScale = std::exp2 (15.0f - 12.0f * c);   // quantize levels (half range)
    }

    /** One mono sample of the varispeed read (+ bitcrush in SK-1 mode). */
    float renderSample (const SamplerSource& s) noexcept
    {
        if (finished || inc <= 0.0 || ! s.isValid())
            return 0.0f;

        const int length = (int) s.audio.size();

        float x;
        if (s.oneShot)
        {
            if (pos >= (double) (length - 1))
            {
                finished = true;
                return 0.0f;
            }
            x = read (s, pos, length);
        }
        else
        {
            const int loopLen = juce::jmax (2, s.loopEnd - s.loopStart);

            // Equal-gain crossfade into the loop start keeps the seam
            // click-free without any preprocessing of the source.
            const double xf      = juce::jmin (1024.0, loopLen * 0.5);
            const double fadeIn  = (double) s.loopEnd - xf;

            x = read (s, pos, length);
            if (pos > fadeIn)
            {
                const float  a    = (float) (((double) s.loopEnd - pos) / xf);
                const double back = pos - (double) loopLen;
                if (back >= 0.0)
                    x = x * a + read (s, back, length) * (1.0f - a);
            }
        }

        pos += inc;
        if (! s.oneShot && pos >= (double) s.loopEnd)
            pos -= (double) juce::jmax (2, s.loopEnd - s.loopStart);

        if (engine == Engine::modern)
            return x;               // clean path: no crush stage at all

        // Bitcrush: sample-and-hold decimation, then bit-depth quantize of
        // the held value. Near-transparent at crush = 0 (hold 1, 16 bit).
        crushPhase += 1.0f;
        if (crushPhase >= crushHold)
        {
            crushPhase -= crushHold;
            if (crushPhase >= crushHold)      // reset() / hold shrank underneath us
                crushPhase = 0.0f;
            held = std::round (x * crushScale) / crushScale;
        }
        return held;
    }

private:
    float read (const SamplerSource& s, double p, int length) const noexcept
    {
        return engine == Engine::modern ? readSinc (s, p, length, inc)
                                        : readLerp (s, p, length);
    }

    static float readLerp (const SamplerSource& s, double p, int length) noexcept
    {
        const int    i0   = juce::jlimit (0, length - 1, (int) p);
        const int    i1   = juce::jmin (i0 + 1, length - 1);
        const float  frac = (float) (p - (double) i0);
        const float  a    = s.audio[(size_t) i0];
        return a + (s.audio[(size_t) i1] - a) * frac;
    }

    //==========================================================================
    // Modern engine: band-limited fractional read, Julius O. Smith style.
    // A one-sided Blackman-Harris windowed sinc is tabulated at sincPhases
    // points per zero crossing; the kernel is evaluated by linear-interpolated
    // table lookup. When the read runs faster than 1x (transposing up) the
    // kernel is time-stretched by the speed ratio, which lowers its cutoff to
    // the post-transpose Nyquist — the same trick FL Studio's sinc
    // interpolation uses to keep upward repitches alias-free.
    //==========================================================================
    static constexpr int halfTaps   = 16;    // 32-point kernel at 1x speed
    static constexpr int sincPhases = 128;   // table points per zero crossing
    static constexpr double maxRatio = 4.0;  // cap kernel widening (+2 octaves)

    static const float* sincTable() noexcept
    {
        static const auto table = []
        {
            std::array<float, halfTaps * sincPhases + 2> t {};
            constexpr double pi = 3.14159265358979323846;
            for (int i = 0; i <= halfTaps * sincPhases; ++i)
            {
                const double u    = (double) i / (double) sincPhases;  // zero crossings
                const double px   = pi * u;
                const double sinc = i == 0 ? 1.0 : std::sin (px) / px;
                const double th   = pi * u / (double) halfTaps;        // window arg
                const double win  = 0.35875 + 0.48829 * std::cos (th)
                                  + 0.14128 * std::cos (2.0 * th)
                                  + 0.01168 * std::cos (3.0 * th);
                t[(size_t) i] = (float) (sinc * win);
            }
            t[t.size() - 1] = 0.0f;                                    // lookup pad
            return t;
        }();
        return table.data();
    }

    static float readSinc (const SamplerSource& s, double p, int length,
                           double speed) noexcept
    {
        // ratio > 1 widens the kernel and drops its cutoff (anti-aliasing);
        // reads at or below 1x use the full-bandwidth 32-point kernel.
        const double ratio = juce::jlimit (1.0, maxRatio, speed);
        const double step  = (double) sincPhases / ratio;   // table pts per source sample
        const int    span  = (int) ((double) halfTaps * ratio);
        const int    i0    = (int) p;
        const float* tbl   = sincTable();
        constexpr int tableEnd = halfTaps * sincPhases;

        float acc = 0.0f, norm = 0.0f;
        for (int k = i0 - span; k <= i0 + span + 1; ++k)
        {
            const double u = std::abs ((double) k - p) * step;
            if (u >= (double) tableEnd)
                continue;
            const int   ui = (int) u;
            const float w  = tbl[ui] + (tbl[ui + 1] - tbl[ui]) * (float) (u - ui);

            // Out-of-range taps weigh in as silence so note edges fade
            // band-limited instead of smearing the boundary sample.
            if (k >= 0 && k < length)
                acc += s.audio[(size_t) k] * w;
            norm += w;
        }
        // Normalizing by the kernel sum pins DC gain to exactly 1 for every
        // fractional phase (removes the tabulation ripple).
        return norm > 1.0e-6f ? acc / norm : 0.0f;
    }

    double pos = 0.0;         // fractional read head, samples of the source
    double inc = 0.0;         // read speed (1 = source rate at root pitch)
    bool   finished = false;
    Engine engine   = Engine::sk1;

    float crushHold  = 1.0f;  // output samples per held value
    float crushPhase = 0.0f;
    float crushScale = 32768.0f;
    float held       = 0.0f;
};

} // namespace axiom::dsp
