/*
  ==============================================================================

    AxiomLookAndFeel.h

    Axiom's visual language: dark neomorphism — deep neutral surfaces, soft
    dual shadows (dark below-right, faint light above-left), rounded corners,
    a single mint accent for value/state, generous spacing. Inspired by
    Teenage Engineering / FabFilter / Ableton: calm, tactile, never cluttered.

    All colours live here as the single source of truth; components pull from
    Palette rather than hard-coding.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace axiom
{

namespace palette
{
    inline const juce::Colour background   { 0xff14161b };
    inline const juce::Colour panel        { 0xff1c1f27 };
    inline const juce::Colour panelRaised  { 0xff232833 };
    inline const juce::Colour outline      { 0xff2e3441 };
    inline const juce::Colour accent       { 0xff5ee1c2 };   // mint
    inline const juce::Colour accentAlt    { 0xff7aa2ff };   // periwinkle
    inline const juce::Colour text         { 0xffe8eaf0 };
    inline const juce::Colour textDim      { 0xff8a90a0 };
    inline const juce::Colour warning      { 0xffffb86b };
    inline const juce::Colour shadowDark   { 0x99000000 };
    inline const juce::Colour shadowLight  { 0x10ffffff };
}

//==============================================================================
class AxiomLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    AxiomLookAndFeel();

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getLabelFont (juce::Label&) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;

    /** Soft-raised rounded panel used by every section of the editor. */
    static void drawPanel (juce::Graphics& g, juce::Rectangle<float> bounds,
                           float cornerRadius = 12.0f,
                           juce::Colour fill = palette::panel);
};

} // namespace axiom
