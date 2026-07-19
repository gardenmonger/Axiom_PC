/*
  ==============================================================================

    AxiomLookAndFeel.h

    Axiom's visual language, following the SynthGenesis design system
    (docs/AxiomMan): dark neomorphic surfaces with depth — raised panels lit
    from the upper-left, inset pressed states — Neural Blue as the primary
    accent, Energy Purple / AI Cyan / Gold as semantic secondaries, generous
    corner radii (buttons 14, cards 20, panels 26) on an 8 px grid.

    "Hardware synthesizer feeling with AI intelligence": calm, tactile,
    never flat, never glossy.

    All colours live here as the single source of truth; components pull from
    Palette rather than hard-coding.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace axiom
{

// SynthGenesis Vol 1 colour tokens.
namespace palette
{
    inline const juce::Colour background   { 0xff14171b };   // primary background
    inline const juce::Colour panel        { 0xff1a1e23 };   // secondary background
    inline const juce::Colour panelRaised  { 0xff21262d };   // elevated surface
    inline const juce::Colour card         { 0xff272d35 };   // card surface
    inline const juce::Colour outline      { 0xff313943 };   // highlight / border

    inline const juce::Colour accent       { 0xff58b7ff };   // Neural Blue
    inline const juce::Colour cyan         { 0xff34f5e5 };   // AI Cyan
    inline const juce::Colour accentAlt    { 0xff906bff };   // Energy Purple
    inline const juce::Colour success      { 0xff5cf19a };
    inline const juce::Colour warning      { 0xffffb84f };
    inline const juce::Colour error        { 0xffff6464 };
    inline const juce::Colour gold         { 0xffffd85e };

    inline const juce::Colour text         { 0xffe8ecf2 };
    inline const juce::Colour textDim      { 0xff8b94a6 };

    inline const juce::Colour shadowDark   { 0x99000000 };
    inline const juce::Colour shadowLight  { 0x12ffffff };
    inline const juce::Colour glass        { 0x0dffffff };   // glass surface sheen
}

// SynthGenesis Vol 1 metric tokens (8 px grid).
namespace metrics
{
    inline constexpr float radiusButton = 14.0f;
    inline constexpr float radiusCard   = 20.0f;
    inline constexpr float radiusPanel  = 26.0f;
    inline constexpr int   grid         = 8;
    inline constexpr int   navBarHeight = 64;
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

    /** Soft-raised rounded card used by every section of the editor.
        Neomorphic: light from the upper-left, shadow lower-right. */
    static void drawPanel (juce::Graphics& g, juce::Rectangle<float> bounds,
                           float cornerRadius = metrics::radiusCard,
                           juce::Colour fill = palette::panel);

    /** Marks a button as a primary action (Neural Blue fill — Separate,
        Export…) per the SG button system. */
    static void setPrimary (juce::Button& b) { b.getProperties().set ("sgPrimary", true); }
};

} // namespace axiom
