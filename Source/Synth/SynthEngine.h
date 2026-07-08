/*
  ==============================================================================

    SynthEngine.h

    The playable instrument: 16-voice polyphonic engine consuming an
    InstrumentPatch, with sample-accurate MIDI, pitch bend, mod wheel,
    sustain pedal, glide, and the post-voice EffectsChain.

    Threading contract
    ------------------
      - processBlock() runs on the audio thread only. No locks, no
        allocations, no system calls.
      - setPatch() runs on the message thread. The patch (trivially
        copyable) is written into the inactive slot of a two-slot buffer,
        then the slot index is flipped atomically. The caller (PluginProcessor)
        coalesces updates to at most one per timer tick, so a slot is never
        rewritten while the audio thread could still be reading it.
      - Continuous user/host parameters (cutoff, ADSR, FX sends…) arrive
        through RuntimeOverrides: raw atomic floats owned by the APVTS,
        merged over the patch at block start. Offline rendering passes no
        overrides and hears the pure patch.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

#include "Voice.h"
#include "EffectsChain.h"
#include "../Core/InstrumentPatch.h"

namespace axiom
{

/** Raw parameter taps from the APVTS (message/audio-thread safe by design of
    juce::AudioProcessorValueTreeState::getRawParameterValue). Null pointers
    mean "no override — use the patch value". */
struct RuntimeOverrides
{
    std::atomic<float>* cutoff        = nullptr;
    std::atomic<float>* resonance     = nullptr;
    std::atomic<float>* drive         = nullptr;
    std::atomic<float>* filterType    = nullptr;
    std::atomic<float>* filterEnvAmt  = nullptr;

    std::atomic<float>* ampAttack     = nullptr;
    std::atomic<float>* ampDecay      = nullptr;
    std::atomic<float>* ampSustain    = nullptr;
    std::atomic<float>* ampRelease    = nullptr;

    std::atomic<float>* fltAttack     = nullptr;
    std::atomic<float>* fltDecay      = nullptr;
    std::atomic<float>* fltSustain    = nullptr;
    std::atomic<float>* fltRelease    = nullptr;

    std::atomic<float>* lfoRate       = nullptr;
    std::atomic<float>* lfoPitch      = nullptr;
    std::atomic<float>* lfoCutoff     = nullptr;

    std::atomic<float>* detune        = nullptr;
    std::atomic<float>* noiseLevel    = nullptr;
    std::atomic<float>* glide         = nullptr;

    std::atomic<float>* satDrive      = nullptr;
    std::atomic<float>* chorusMix     = nullptr;
    std::atomic<float>* delayMix      = nullptr;
    std::atomic<float>* delayTime     = nullptr;
    std::atomic<float>* delayFeedback = nullptr;
    std::atomic<float>* reverbMix     = nullptr;
    std::atomic<float>* reverbSize    = nullptr;
    std::atomic<float>* width         = nullptr;

    std::atomic<float>* masterGain    = nullptr;
};

//==============================================================================
class SynthEngine
{
public:
    static constexpr int maxVoices = 16;

    void prepare (double sampleRate, int maxBlockSize);
    void releaseResources();

    /** Message thread. Copies the patch into the inactive slot and flips. */
    void setPatch (const InstrumentPatch& newPatch) noexcept;

    /** Returns a copy of the patch the audio thread currently plays. */
    InstrumentPatch getPatch() const noexcept;

    void setOverrides (const RuntimeOverrides* o) noexcept   { overrides = o; }

    /** Audio thread. Renders synth + FX into `buffer` (replaces content). */
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi);

    int  getActiveVoiceCount() const noexcept   { return activeVoiceCount.load (std::memory_order_relaxed); }

    /** Offline utility (export/audition): renders one note through a private
        engine instance. Safe to call from any background thread. */
    static juce::AudioBuffer<float> renderNoteOffline (const InstrumentPatch& patch,
                                                       int midiNote, float velocity,
                                                       double holdSeconds, double tailSeconds,
                                                       double sampleRate);

private:
    void handleMidiEvent (const juce::MidiMessage& m);
    void noteOn  (int note, float velocity);
    void noteOff (int note);
    void renderSegment (int startSample, int numSamples);
    InstrumentPatch buildEffectivePatch (const InstrumentPatch& base) const noexcept;

    Voice* findVoiceToSteal() noexcept;

    std::array<Voice, maxVoices> voices;
    std::array<bool, maxVoices>  sustainHeld {};   // released while pedal down

    EffectsChain fx;

    // Two-slot lock-free patch handoff (see header comment). Spectral
    // wavetables are built into the matching slot before the index flips,
    // so patch and tables always swap together.
    std::array<InstrumentPatch, 2>     patchSlots;
    std::array<dsp::WavetableSet, 2>   tableSlots;
    std::atomic<int> activePatchSlot { 0 };

    // Effective (patch + overrides) + tables for the current block; audio
    // thread only, refreshed from a single slot-index load per block.
    InstrumentPatch effective;
    const dsp::WavetableSet* currentTables = nullptr;

    const RuntimeOverrides* overrides = nullptr;

    juce::AudioBuffer<float> mixBuffer;   // preallocated stereo scratch

    double  sr = 44100.0;
    float   bendSemis   = 0.0f;
    float   modWheel    = 0.0f;
    bool    sustainPedal = false;
    float   lastNoteHz  = 0.0f;
    int64_t noteCounter = 0;

    std::atomic<int> activeVoiceCount { 0 };
};

} // namespace axiom
