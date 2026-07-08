/*
  ==============================================================================

    Voice.h

    One polyphonic voice: 3 oscillator slots + noise -> drive -> stereo TPT
    SVF -> amp envelope. Modulation (vibrato LFO, filter envelope, filter
    LFO, key tracking, velocity) is evaluated at control rate (16-sample
    sub-blocks); sample-rate work is limited to oscillator ticks, the filter
    and the amp envelope.

    Voices are owned by SynthEngine in a fixed array — no allocation at
    note-on. The patch is passed by const reference each block; a voice
    latches oscillator configuration at startNote() and tracks continuous
    parameters (cutoff, envelope times, LFO) live so GUI edits are audible
    on held notes.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "SynthModules.h"
#include "../Core/InstrumentPatch.h"

namespace axiom
{

class Voice
{
public:
    static constexpr int controlInterval = 16;   // samples per mod update

    void prepare (double sampleRate) noexcept
    {
        sr = sampleRate;
        ampEnv.setSampleRate (sampleRate);
        filterEnv.setSampleRate (sampleRate);
        reset();
    }

    void reset() noexcept
    {
        active = false;
        noteNumber = -1;
        fading = false;
        fadeGain = 1.0f;
        hasPendingNote = false;
        filtL.reset();
        filtR.reset();
    }

    void startNote (int midiNote, float vel, const InstrumentPatch& patch,
                    float glideFromHz, int64_t timestamp) noexcept
    {
        noteNumber = midiNote;
        velocity   = vel;
        startTime  = timestamp;
        active     = true;

        fading         = false;
        fadeGain       = 1.0f;
        hasPendingNote = false;

        for (auto& o : oscs)
            o.reset();
        noise.state = 0x9E3779B9u ^ (uint32_t) (timestamp & 0xffffffff);

        filtL.reset();
        filtR.reset();

        targetHz  = dsp::midiToHz ((float) midiNote);
        currentHz = (patch.glideMs > 1.0f && glideFromHz > 0.0f) ? glideFromHz : targetHz;

        updateEnvelopeParams (patch);
        ampEnv.reset();                  // attack from silence: juce::ADSR
        filterEnv.reset();               // otherwise ramps from its old level
        ampEnv.noteOn();
        filterEnv.noteOn();

        lfoPhase = 0.0f;
        samplesUntilControl = 0;
    }

    /** Click-free voice steal: fades the current sound over ~2 ms, then
        starts the new note from silence inside render(). The voice reports
        the new note number immediately so a fast note-off still matches. */
    void stealNote (int midiNote, float vel, float glideFromHz, int64_t timestamp) noexcept
    {
        pendingNote    = midiNote;
        pendingVel     = vel;
        pendingGlideHz = glideFromHz;
        pendingStamp   = timestamp;
        hasPendingNote = true;

        noteNumber = midiNote;
        startTime  = timestamp;
        heldFlag   = true;
        beginFade();
    }

    void stopNote (bool allowTail) noexcept
    {
        if (! active)
            return;
        if (hasPendingNote)          // note released while its steal-fade was
        {                            // still running: cancel the restart and
            hasPendingNote = false;  // let the fade finish into silence
            return;
        }
        if (allowTail)
        {
            ampEnv.noteOff();
            filterEnv.noteOff();
        }
        else
        {
            beginFade();             // fast fade instead of an instant cut
        }
    }

    bool  isActive() const noexcept        { return active; }
    bool  isReleasing() const noexcept     { return active && ! heldFlag; }
    int   getNote() const noexcept         { return noteNumber; }
    int64_t getStartTime() const noexcept  { return startTime; }
    float getCurrentHz() const noexcept    { return currentHz; }

    /** Additively renders `numSamples` into outL/outR. `bendSemis` is the
        engine-wide pitch-bend offset; `tables` backs OscType::Harmonic.
        Returns false when the voice finished its release during this block. */
    bool render (float* outL, float* outR, int numSamples,
                 const InstrumentPatch& patch, float bendSemis,
                 const dsp::WavetableSet* tables) noexcept
    {
        if (! active)
            return false;

        updateEnvelopeParams (patch);

        const float lfoInc = patch.lfo.rateHz / (float) sr;

        // Recomputed in the control-rate update as well: a steal-fade can
        // restart the voice mid-block with a new velocity.
        float glideCoef = patch.glideMs > 1.0f
            ? 1.0f - std::exp (-1.0f / (0.001f * patch.glideMs * (float) sr))
            : 1.0f;
        float velAmp = 1.0f - patch.velToAmp * (1.0f - velocity);

        int i = 0;
        while (i < numSamples)
        {
            // ---- Control-rate update ------------------------------------
            if (samplesUntilControl <= 0)
            {
                samplesUntilControl = controlInterval;

                glideCoef = patch.glideMs > 1.0f
                    ? 1.0f - std::exp (-1.0f / (0.001f * patch.glideMs * (float) sr))
                    : 1.0f;
                velAmp = 1.0f - patch.velToAmp * (1.0f - velocity);

                currentHz += (targetHz - currentHz)
                             * (1.0f - std::pow (1.0f - glideCoef, (float) controlInterval));

                const float lfoVal = std::sin (6.2831853f * lfoPhase);
                lfoPhase += lfoInc * (float) controlInterval;
                if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;

                const float pitchCents = bendSemis * 100.0f
                                       + lfoVal * patch.lfo.pitchDepthCents;

                for (size_t s = 0; s < patch.oscs.size(); ++s)
                {
                    const auto& slot = patch.oscs[s];
                    if (! slot.enabled) continue;
                    const float hz = currentHz * std::exp2 ((float) slot.octave)
                                     * dsp::centsToRatio (pitchCents);
                    oscs[s].setFrequency (slot.type, hz, slot.detuneCents, sr, tables);
                }

                fEnvLevel = filterEnv.getNextSample();
                for (int skip = 1; skip < controlInterval; ++skip)
                    filterEnv.getNextSample();     // keep env clock aligned to sample rate

                const float keyTrackSt = patch.filter.keyTrack
                                         * (float) (noteNumber - patch.rootNote);
                const float modSt = patch.filter.envAmountSt * fEnvLevel
                                  + patch.lfo.cutoffDepthSt * lfoVal
                                  + keyTrackSt
                                  + patch.velToCutoff * 24.0f * (velocity - 0.5f);
                const float cutoff = patch.filter.cutoffHz * std::exp2 (modSt / 12.0f);

                filtL.setParams (cutoff, patch.filter.resonance, sr);
                filtR.setParams (cutoff, patch.filter.resonance, sr);
            }

            // ---- Sample-rate segment -------------------------------------
            const int segment = juce::jmin (numSamples - i, samplesUntilControl);
            int  consumed  = segment;
            bool restarted = false;
            for (int n = 0; n < segment; ++n)
            {
                float l = 0.0f, r = 0.0f;

                for (size_t s = 0; s < patch.oscs.size(); ++s)
                {
                    const auto& slot = patch.oscs[s];
                    if (! slot.enabled) continue;
                    float ol, or_;
                    oscs[s].renderSample (slot.type, slot.pulseWidth,
                                          slot.stereoSpread, noise, tables, ol, or_);

                    // Non-supersaw slots with spread pan by detune sign, so a
                    // detuned spectral pair opens into a stereo image.
                    float gl = 1.0f, gr = 1.0f;
                    if (slot.stereoSpread > 0.01f && slot.type != OscType::Supersaw)
                    {
                        const float pan = (slot.detuneCents < 0.0f ? -1.0f : 1.0f)
                                          * slot.stereoSpread;
                        gl = pan <= 0.0f ? 1.0f : 1.0f - pan;
                        gr = pan >= 0.0f ? 1.0f : 1.0f + pan;
                    }
                    l += ol * slot.level * gl;
                    r += or_ * slot.level * gr;
                }
                if (patch.noiseLevel > 0.001f)
                {
                    const float nz = noise.next() * patch.noiseLevel;
                    l += nz; r += nz;
                }

                l = dsp::driveStage (l, patch.filter.drive);
                r = dsp::driveStage (r, patch.filter.drive);

                l = filtL.process (l, patch.filter.type);
                r = filtR.process (r, patch.filter.type);

                // 0.32 leaves headroom for chords: voices sum directly into
                // the mix, and a clipped sum reads as pops, not loudness.
                float amp = ampEnv.getNextSample() * velAmp * 0.32f;

                if (fading)
                {
                    fadeGain = juce::jmax (0.0f, fadeGain - fadeStep);
                    amp *= fadeGain;

                    if (fadeGain <= 0.0f)
                    {
                        if (hasPendingNote)
                        {
                            // Faded to silence: safe to restart in place.
                            // startNote() clears the fade and forces a
                            // control-rate update on the next sample.
                            startNote (pendingNote, pendingVel, patch,
                                       pendingGlideHz, pendingStamp);
                            consumed  = n + 1;
                            restarted = true;
                            break;
                        }
                        active = false;
                        return false;
                    }
                }

                outL[i + n] += l * amp;
                outR[i + n] += r * amp;
            }
            i += consumed;
            if (restarted)
                continue;
            samplesUntilControl -= consumed;

            if (! ampEnv.isActive())
            {
                active = false;
                return false;
            }
        }
        return true;
    }

    void setHeld (bool h) noexcept   { heldFlag = h; }

private:
    void beginFade() noexcept
    {
        if (! fading)
        {
            fading   = true;
            fadeStep = 1.0f / juce::jmax (1.0f, 0.002f * (float) sr);   // ~2 ms
        }
    }

    void updateEnvelopeParams (const InstrumentPatch& patch) noexcept
    {
        // Floor the amp times: a reconstructed patch can carry 0 s attack or
        // release, and a zero-time jump in juce::ADSR is an audible click.
        ampEnv.setParameters ({ juce::jmax (0.002f, patch.ampEnv.attack),
                                juce::jmax (0.002f, patch.ampEnv.decay),
                                juce::jlimit (0.0f, 1.0f, patch.ampEnv.sustain),
                                juce::jmax (0.005f, patch.ampEnv.release) });
        filterEnv.setParameters ({ patch.filterEnv.attack, patch.filterEnv.decay,
                                   patch.filterEnv.sustain, patch.filterEnv.release });
    }

    double sr = 44100.0;

    std::array<dsp::Oscillator, InstrumentPatch::maxOscs> oscs;
    dsp::NoiseGen noise;
    dsp::TptSvf   filtL, filtR;
    juce::ADSR    ampEnv, filterEnv;

    float  lfoPhase = 0.0f;
    float  fEnvLevel = 0.0f;
    int    samplesUntilControl = 0;

    float  currentHz = 440.0f, targetHz = 440.0f;
    float  velocity = 1.0f;
    int    noteNumber = -1;
    int64_t startTime = 0;
    bool   active = false;
    bool   heldFlag = false;

    // Anti-click fast fade (~2 ms) for voice stealing and hard kills.
    bool    fading = false;
    float   fadeGain = 1.0f;
    float   fadeStep = 0.0f;
    bool    hasPendingNote = false;   // steal: note to start once faded out
    int     pendingNote = -1;
    float   pendingVel = 0.0f;
    float   pendingGlideHz = 0.0f;
    int64_t pendingStamp = 0;
};

} // namespace axiom
