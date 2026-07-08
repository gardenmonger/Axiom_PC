/*
  ==============================================================================

    InstrumentExporter.h

    Turns a reconstructed InstrumentPatch into portable instrument formats:

      .axiom       native patch (JSON) — lossless, reload anytime
      .sfz + WAVs  multisampled render (24-bit/48 kHz, every 3 semitones,
                   C1..C7) playable in Sforzando/ARIA, TX16Wx, Kontakt (via
                   SFZ import), etc.
      .dspreset    DecentSampler preset referencing the same WAV pool

    Rendering happens offline through SynthEngine::renderNoteOffline on a
    background thread; `progress` (0..1) is polled by the UI.

    Kontakt's native .nki is a closed format — the supported route is SFZ
    import (see docs/ARCHITECTURE.md, Export pipeline).

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <atomic>

#include "../Core/InstrumentPatch.h"

namespace axiom
{

class InstrumentExporter
{
public:
    struct Options
    {
        bool   writeAxiomPatch    = true;
        bool   writeSfz           = true;
        bool   writeDecentSampler = true;
        int    lowNote            = 24;    // C1
        int    highNote           = 96;    // C7
        int    noteStep           = 3;     // semitones between sampled zones
        double sampleRate         = 48000.0;
        double holdSeconds        = 2.0;
    };

    /** Blocking; call from a background thread. Creates `instrumentName/`
        inside destDir and fills it. Returns ok() with a summary message or
        fail() with the reason. */
    static juce::Result exportInstrument (const InstrumentPatch& patch,
                                          const juce::File& destDir,
                                          const juce::String& instrumentName,
                                          const Options& options,
                                          std::atomic<float>& progress);

private:
    static juce::Result writeWav (const juce::AudioBuffer<float>& audio,
                                  double sampleRate, const juce::File& file);
    static juce::String buildSfz (const InstrumentPatch& patch,
                                  const juce::Array<int>& rootNotes, int lowNote, int highNote);
    static juce::String buildDecentSampler (const InstrumentPatch& patch,
                                            const juce::Array<int>& rootNotes,
                                            int lowNote, int highNote);
    static juce::String sampleFileName (int midiNote);
};

} // namespace axiom
