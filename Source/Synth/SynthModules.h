/*
  ==============================================================================

    SynthModules.h

    Real-time-safe DSP primitives for the Axiom voice engine.

      - PolyBLEP band-limited oscillators (saw / square / pulse / triangle /
        sine / 7-partial supersaw / noise). PolyBLEP chosen over wavetables
        for v1: zero memory, cheap, alias-suppressed, and — critically — the
        spectra match the ideal shapes the reconstruction stage assumes, so
        what the analyzer infers is what the engine plays.
      - Zavalishin TPT state-variable filter (LP/BP/HP, zero-delay feedback)
        with tanh drive stage.

    Everything here is header-only/inlined (hot path), allocation-free and
    noexcept. Coefficient updates happen at control rate (16-sample
    sub-blocks) in Voice.h, not per sample.

  ==============================================================================
*/

#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "../Core/InstrumentPatch.h"
#include "Wavetable.h"

namespace axiom::dsp
{

//==============================================================================
inline float midiToHz (float midiNote) noexcept
{
    return 440.0f * std::exp2 ((midiNote - 69.0f) / 12.0f);
}

inline float centsToRatio (float cents) noexcept
{
    return std::exp2 (cents / 1200.0f);
}

/** xorshift32 — allocation-free white noise, one state word per voice. */
struct NoiseGen
{
    uint32_t state = 0x12345678u;

    float next() noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float) (int32_t) state * (1.0f / 2147483648.0f);
    }
};

//==============================================================================
/** Residual applied at waveform discontinuities to suppress aliasing.
    t = phase 0..1, dt = phase increment. */
inline float polyBlep (float t, float dt) noexcept
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt)
    {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

//==============================================================================
/** One oscillator slot: up to 7 detuned partials (supersaw) or a single
    band-limited shape. Phases persist across blocks; frequency is set at
    control rate. */
class Oscillator
{
public:
    static constexpr int maxPartials = 7;

    void reset (float initialPhase = 0.0f) noexcept
    {
        for (auto& p : phase) p = initialPhase;
        // Decorrelate supersaw partials so the stack doesn't comb-cancel.
        for (int i = 1; i < maxPartials; ++i)
            phase[(size_t) i] = std::fmod (initialPhase + 0.137f * (float) i, 1.0f);
    }

    /** Sets per-partial phase increments. For non-supersaw types only
        partial 0 is used. `tables` selects the wavetable mip for Harmonic. */
    void setFrequency (OscType type, float baseHz, float detuneCents,
                       double sampleRate, const WavetableSet* tables = nullptr) noexcept
    {
        if (type == OscType::Harmonic)
        {
            inc[0] = (float) ((double) baseHz * centsToRatio (detuneCents) / sampleRate);
            mip = (tables != nullptr && tables->isValid()) ? tables->pickMip (inc[0]) : 0;
            return;
        }
        if (type == OscType::Supersaw)
        {
            // JP-8000-style spread: symmetric fractions of the detune amount.
            static constexpr std::array<float, maxPartials> spread
                { 0.0f, -1.0f, 1.0f, -0.64f, 0.64f, -0.27f, 0.27f };
            for (int i = 0; i < maxPartials; ++i)
                inc[(size_t) i] = (float) ((double) baseHz
                                  * centsToRatio (detuneCents * spread[(size_t) i]) / sampleRate);
        }
        else
        {
            inc[0] = (float) ((double) baseHz * centsToRatio (detuneCents) / sampleRate);
        }
    }

    /** Renders one sample into a stereo pair. `spread` pans supersaw partials
        alternately outward; other shapes stay centred. */
    void renderSample (OscType type, float pulseWidth, float spread,
                       NoiseGen& noise, const WavetableSet* tables,
                       float& outL, float& outR) noexcept
    {
        switch (type)
        {
            case OscType::Harmonic:
            {
                float& hp = phase[0];
                // Fall back to a band-limited saw if no table was built —
                // never silent, never aliasing.
                if (tables == nullptr || ! tables->isValid())
                {
                    const float s = (2.0f * hp - 1.0f) - polyBlep (hp, inc[0]);
                    outL = outR = s;
                }
                else
                {
                    outL = outR = tables->sample (mip, hp);
                }
                hp += inc[0];
                if (hp >= 1.0f) hp -= 1.0f;
                return;
            }

            case OscType::Supersaw:
            {
                float l = 0.0f, r = 0.0f;
                for (int i = 0; i < maxPartials; ++i)
                {
                    float& ph = phase[(size_t) i];
                    const float dt = inc[(size_t) i];
                    const float s  = (2.0f * ph - 1.0f) - polyBlep (ph, dt);

                    // Centre partial full both sides; side partials panned
                    // alternately left/right by the spread amount.
                    const float pan  = (i == 0) ? 0.0f
                                                : ((i % 2 == 1) ? -spread : spread);
                    const float gain = (i == 0) ? 0.45f : 0.22f;
                    l += s * gain * (pan <= 0.0f ? 1.0f : 1.0f - pan);
                    r += s * gain * (pan >= 0.0f ? 1.0f : 1.0f + pan);

                    ph += dt;
                    if (ph >= 1.0f) ph -= 1.0f;
                }
                outL = l; outR = r;
                return;
            }

            case OscType::Noise:
            {
                const float n = noise.next();
                outL = outR = n;
                return;
            }

            default: break;
        }

        float& ph = phase[0];
        const float dt = inc[0];
        float s = 0.0f;

        switch (type)
        {
            case OscType::Sine:
                s = std::sin (6.2831853f * ph);
                break;

            case OscType::Saw:
                s = (2.0f * ph - 1.0f) - polyBlep (ph, dt);
                break;

            case OscType::Square:
            case OscType::Pulse:
            {
                const float pw = (type == OscType::Square) ? 0.5f : pulseWidth;
                s = ph < pw ? 1.0f : -1.0f;
                s += polyBlep (ph, dt);
                float t2 = ph - pw;
                if (t2 < 0.0f) t2 += 1.0f;
                s -= polyBlep (t2, dt);
                break;
            }

            case OscType::Triangle:
                // -12 dB/oct harmonic rolloff keeps naive triangle aliasing
                // inaudible below ~5 kHz fundamentals.
                s = 4.0f * std::abs (ph - 0.5f) - 1.0f;
                break;

            default: break;
        }

        ph += dt;
        if (ph >= 1.0f) ph -= 1.0f;
        outL = outR = s;
    }

private:
    std::array<float, maxPartials> phase {};
    std::array<float, maxPartials> inc {};
    int mip = 0;
};

//==============================================================================
/** Zavalishin topology-preserving-transform SVF. */
class TptSvf
{
public:
    void reset() noexcept                { ic1eq = ic2eq = 0.0f; }

    void setParams (float cutoffHz, float resonance, double sampleRate) noexcept
    {
        const float fc = cutoffHz < 20.0f ? 20.0f
                        : cutoffHz > (float) sampleRate * 0.49f ? (float) sampleRate * 0.49f
                        : cutoffHz;
        g = std::tan (3.14159265f * fc / (float) sampleRate);
        k = 2.0f - 1.95f * (resonance < 0.0f ? 0.0f : resonance > 1.0f ? 1.0f : resonance);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    float process (float x, FilterType type) noexcept
    {
        const float v3 = x - ic2eq;
        const float v1 = a1 * ic1eq + a2 * v3;
        const float v2 = ic2eq + a2 * ic1eq + a3 * v3;
        ic1eq = 2.0f * v1 - ic1eq;
        ic2eq = 2.0f * v2 - ic2eq;

        switch (type)
        {
            case FilterType::LowPass:  return v2;
            case FilterType::BandPass: return v1;
            case FilterType::HighPass: return x - k * v1 - v2;
        }
        return v2;
    }

private:
    float g = 0.1f, k = 2.0f, a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    float ic1eq = 0.0f, ic2eq = 0.0f;
};

/** Soft-clipping drive stage placed before the filter (analog-style). */
inline float driveStage (float x, float drive) noexcept
{
    if (drive <= 0.001f)
        return x;
    const float pre  = 1.0f + 4.0f * drive;
    const float post = 1.0f / std::tanh (pre * 0.7f);   // rough level compensation
    return std::tanh (x * pre * 0.7f) * post * 0.7f;
}

} // namespace axiom::dsp
