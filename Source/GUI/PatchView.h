/*
  ==============================================================================

    PatchView.h

    Right-hand panel: the reconstructed instrument.

      - Patch Discovery readout: human-readable synthesis recipe inferred
        from the sample ("Supersaw (18 ct) · Low-pass 3.4 kHz · ADSR …"),
        the reconstruction tier that produced it, and its confidence.
      - Character profile bars (brightness / warmth / movement / complexity).
      - Engine selector chips (RECIPE / DDSP / BOTH) bound to the engineMode
        parameter: play the reconstructed recipe, the DDSP resynthesis of
        the source stretched across every key, or both layered.
      - 16 macro knobs bound to host-automatable parameters, grouped
        FILTER / AMP ENV / TONE / SPACE. Editing them is editing the
        reconstruction — the patch stays a living synth recipe, never
        frozen audio.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "../Core/InstrumentPatch.h"

namespace axiom
{

class PatchView final : public juce::Component
{
public:
    explicit PatchView (juce::AudioProcessorValueTreeState& apvts);

    /** Message thread: refreshes the discovery readout after reconstruction
        or state restore. */
    void setPatchInfo (const InstrumentPatch& patch, const juce::String& tierName);

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int knobsPerRow = 4;
    static constexpr int numRows     = 4;

private:
    struct Knob
    {
        juce::Slider slider;
        juce::Label  label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    void addKnob (juce::AudioProcessorValueTreeState& apvts,
                  const char* paramID, const juce::String& text);

    std::vector<std::unique_ptr<Knob>> knobs;
    std::array<juce::Rectangle<int>, numRows> rowAreas {};

    // Engine selector: three radio-style chips driving the engineMode
    // choice parameter (toggle state mirrors the param via the attachment,
    // so host automation moves the chips too).
    std::array<juce::TextButton, 3> modeButtons;
    std::unique_ptr<juce::ParameterAttachment> modeAttachment;

    juce::String description { "Import a sample to reconstruct an instrument." };
    juce::String tier;
    float        confidence = 0.0f;
    CharacterProfile character;
    juce::String oscSummary;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatchView)
};

} // namespace axiom
