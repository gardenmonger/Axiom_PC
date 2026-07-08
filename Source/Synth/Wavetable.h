/*
  ==============================================================================

    Wavetable.h

    Spectral-resynthesis wavetables for OscType::Harmonic.

    build() renders the measured harmonic ladder (patch.harmonicGains) into a
    set of mip-mapped single-cycle tables — mip m keeps harmonics 1..(64>>m),
    so playback picks the densest table whose top partial stays below
    Nyquist: alias-free at any key, and the sustained timbre matches the
    analyzed sound almost exactly.

    Partial phases are golden-ratio scrambled: steady-state timbre is phase
    insensitive, but scrambling lowers the waveform crest factor (zero-phase
    additive stacks sound identical yet clip headroom).

    build() runs on the message thread when a patch is applied (~1 ms);
    sample()/pickMip() are audio-thread hot-path, allocation-free.

  ==============================================================================
*/

#pragma once

#include <array>
#include <cmath>

namespace axiom::dsp
{

class WavetableSet
{
public:
    static constexpr int tableSize    = 2048;
    static constexpr int maxHarmonics = 64;
    static constexpr int numMips      = 7;      // 64, 32, 16, 8, 4, 2, 1 partials

    void clear() noexcept              { valid = false; }
    bool isValid() const noexcept      { return valid; }

    void build (const float* gains, int numHarmonics) noexcept
    {
        numHarmonics = numHarmonics < 0 ? 0
                     : numHarmonics > maxHarmonics ? maxHarmonics : numHarmonics;
        if (numHarmonics == 0)
        {
            valid = false;
            return;
        }

        for (int m = 0; m < numMips; ++m)
        {
            const int topPartial = std::min (numHarmonics, maxHarmonics >> m);
            auto& table = tables[(size_t) m];

            for (int i = 0; i < tableSize; ++i)
            {
                float acc = 0.0f;
                for (int k = 1; k <= topPartial; ++k)
                {
                    const float g = gains[k - 1];
                    if (g <= 1.0e-5f)
                        continue;
                    const float phase = 6.2831853f
                        * ((float) (k * i) / (float) tableSize
                           + goldenPhase (k));
                    acc += g * std::sin (phase);
                }
                table[(size_t) i] = acc;
            }

            // Peak-normalize each mip (headroom; per-note level comes from
            // the amp envelope / slot level).
            float peak = 1.0e-9f;
            for (int i = 0; i < tableSize; ++i)
                peak = std::max (peak, std::abs (table[(size_t) i]));
            const float norm = 0.95f / peak;
            for (int i = 0; i < tableSize; ++i)
                table[(size_t) i] *= norm;

            table[(size_t) tableSize] = table[0];   // wrap guard for lerp
        }
        valid = true;
    }

    /** Densest mip whose highest partial stays below Nyquist for this
        normalized phase increment (frequency / sampleRate). */
    int pickMip (float phaseInc) const noexcept
    {
        const int allowed = phaseInc > 1.0e-9f ? (int) (0.5f / phaseInc) : maxHarmonics;
        for (int m = 0; m < numMips; ++m)
            if ((maxHarmonics >> m) <= allowed)
                return m;
        return numMips - 1;
    }

    /** Linear-interpolated lookup; phase in [0, 1). */
    float sample (int mip, float phase) const noexcept
    {
        const auto& table = tables[(size_t) mip];
        const float pos  = phase * (float) tableSize;
        const int   idx  = (int) pos;
        const float frac = pos - (float) idx;
        return table[(size_t) idx] + (table[(size_t) idx + 1] - table[(size_t) idx]) * frac;
    }

private:
    static float goldenPhase (int k) noexcept
    {
        const float x = (float) k * 0.6180339887f;
        return x - std::floor (x);
    }

    std::array<std::array<float, tableSize + 1>, numMips> tables {};
    bool valid = false;
};

} // namespace axiom::dsp
