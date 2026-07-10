/*
  ==============================================================================

    PatchView.h

    Right-hand panel: the reconstructed instrument.

      - Patch Discovery readout: human-readable synthesis recipe inferred
        from the sample ("Supersaw (18 ct) · Low-pass 3.4 kHz · ADSR …"),
        the reconstruction tier that produced it, and its confidence.
      - Character profile bars (brightness / warmth / movement / complexity).
      - Engine selector chips (RECIPE / DDSP / BOTH) bound to the engineMode
        parameter, plus an independent SK-1 toggle chip (stretchOn): raw
        varispeed playback of the source sample layered on top of any
        recipe/DDSP combination. An HQ chip (samplerEngine) swaps that
        layer's read between the SK-1 lo-fi path and a modern FL-style
        band-limited sinc resampler.
      - 20 macro knobs bound to host-automatable parameters, grouped
        FILTER / AMP ENV / TONE / SPACE / LAYER MIX. The LAYER MIX row
        blends the three engine layers and drives the SK-1 bitcrush.

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
    static constexpr int numRows     = 5;

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
    // so host automation moves the chips too), plus an independent SK-1
    // toggle chip driving the stretchOn bool parameter.
    std::array<juce::TextButton, 3> modeButtons;
    std::unique_ptr<juce::ParameterAttachment> modeAttachment;
    juce::TextButton stretchButton;
    std::unique_ptr<juce::ParameterAttachment> stretchAttachment;

    // HQ chip: flips the sample layer between the SK-1 lo-fi read and the
    // modern band-limited resampler (samplerEngine choice parameter).
    juce::TextButton hqButton;
    std::unique_ptr<juce::ParameterAttachment> hqAttachment;

    juce::String description { "Import a sample to reconstruct an instrument." };
    juce::String tier;
    float        confidence = 0.0f;
    CharacterProfile character;
    juce::String oscSummary;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PatchView)
};

} // namespace axiom
