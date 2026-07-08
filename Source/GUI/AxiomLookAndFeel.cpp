#include "AxiomLookAndFeel.h"

namespace axiom
{

AxiomLookAndFeel::AxiomLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, palette::background);

    setColour (juce::Slider::rotarySliderFillColourId,    palette::accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, palette::outline);
    setColour (juce::Slider::thumbColourId,               palette::text);
    setColour (juce::Slider::textBoxTextColourId,         palette::textDim);
    setColour (juce::Slider::textBoxOutlineColourId,      juce::Colours::transparentBlack);

    setColour (juce::Label::textColourId,                 palette::text);

    setColour (juce::TextButton::buttonColourId,          palette::panelRaised);
    setColour (juce::TextButton::buttonOnColourId,        palette::accent.withAlpha (0.25f));
    setColour (juce::TextButton::textColourOffId,         palette::text);
    setColour (juce::TextButton::textColourOnId,          palette::accent);

    setColour (juce::ComboBox::backgroundColourId,        palette::panelRaised);
    setColour (juce::ComboBox::textColourId,              palette::text);
    setColour (juce::ComboBox::outlineColourId,           palette::outline);
    setColour (juce::ComboBox::arrowColourId,             palette::textDim);

    setColour (juce::PopupMenu::backgroundColourId,       palette::panelRaised);
    setColour (juce::PopupMenu::textColourId,             palette::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, palette::accent.withAlpha (0.2f));
    setColour (juce::PopupMenu::highlightedTextColourId,  palette::accent);

    setColour (juce::MidiKeyboardComponent::whiteNoteColourId,        juce::Colour (0xffe2e4ea));
    setColour (juce::MidiKeyboardComponent::blackNoteColourId,        juce::Colour (0xff20232b));
    setColour (juce::MidiKeyboardComponent::keySeparatorLineColourId, juce::Colour (0xff30343e));
    setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, palette::accent.withAlpha (0.35f));
    setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId,      palette::accent.withAlpha (0.7f));
    setColour (juce::MidiKeyboardComponent::shadowColourId,           palette::shadowDark);

    setColour (juce::AlertWindow::backgroundColourId,     palette::panel);
    setColour (juce::AlertWindow::textColourId,           palette::text);
    setColour (juce::AlertWindow::outlineColourId,        palette::outline);

    setColour (juce::TooltipWindow::backgroundColourId,   palette::panelRaised);
    setColour (juce::TooltipWindow::textColourId,         palette::text);
}

//==============================================================================
void AxiomLookAndFeel::drawPanel (juce::Graphics& g, juce::Rectangle<float> b,
                                  float radius, juce::Colour fill)
{
    // Neomorphic dual shadow: dark drop below-right, light sheen above-left.
    g.setColour (palette::shadowDark);
    g.fillRoundedRectangle (b.translated (0.0f, 2.5f), radius);
    g.setColour (palette::shadowLight);
    g.fillRoundedRectangle (b.translated (0.0f, -1.0f), radius);

    g.setColour (fill);
    g.fillRoundedRectangle (b, radius);

    g.setColour (palette::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), radius, 1.0f);
}

//==============================================================================
void AxiomLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                         float pos, float startAngle, float endAngle,
                                         juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y,
                                                (float) width, (float) height).reduced (4.0f);
    const float size   = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto  centre = bounds.getCentre();
    const float radius = size * 0.5f;
    const float angle  = startAngle + pos * (endAngle - startAngle);

    // Soft body with dual shadow.
    {
        const auto body = juce::Rectangle<float> (size, size).withCentre (centre).reduced (size * 0.10f);
        g.setColour (palette::shadowDark);
        g.fillEllipse (body.translated (0.0f, 2.0f));
        g.setColour (palette::shadowLight);
        g.fillEllipse (body.translated (0.0f, -1.5f));

        g.setGradientFill (juce::ColourGradient (palette::panelRaised.brighter (0.08f),
                                                 centre.x, body.getY(),
                                                 palette::panelRaised.darker (0.15f),
                                                 centre.x, body.getBottom(), false));
        g.fillEllipse (body);
        g.setColour (palette::outline);
        g.drawEllipse (body, 1.0f);
    }

    // Track + value arcs.
    const float arcRadius = radius - 1.5f;
    const float thickness = juce::jmax (2.0f, size * 0.055f);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         startAngle, endAngle, true);
    g.setColour (palette::outline);
    g.strokePath (track, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    const bool enabled = slider.isEnabled();
    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         startAngle, angle, true);
    g.setColour (enabled ? palette::accent : palette::textDim.withAlpha (0.4f));
    g.strokePath (value, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // Pointer.
    const float dotRadius = juce::jmax (2.0f, size * 0.05f);
    const auto  dotPos    = centre.getPointOnCircumference (radius * 0.55f, angle);
    g.setColour (enabled ? palette::text : palette::textDim);
    g.fillEllipse (juce::Rectangle<float> (dotRadius * 2.0f, dotRadius * 2.0f)
                       .withCentre (dotPos));
}

//==============================================================================
void AxiomLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                             const juce::Colour& colour,
                                             bool highlighted, bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
    const float radius = juce::jmin (9.0f, bounds.getHeight() * 0.5f);

    if (! down)
    {
        g.setColour (palette::shadowDark);
        g.fillRoundedRectangle (bounds.translated (0.0f, 1.5f), radius);
    }

    auto fill = colour;
    if (down)             fill = palette::accent.withAlpha (0.30f);
    else if (highlighted) fill = fill.brighter (0.12f);

    g.setColour (fill);
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (highlighted || down ? palette::accent.withAlpha (0.6f) : palette::outline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
}

juce::Font AxiomLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::Font (juce::FontOptions ((float) juce::jmin (15, buttonHeight - 6)))
               .boldened();
}

juce::Font AxiomLookAndFeel::getLabelFont (juce::Label& label)
{
    return juce::Font (juce::FontOptions (juce::jmin (14.0f, (float) label.getHeight() - 2.0f)));
}

//==============================================================================
void AxiomLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                     int, int, int, int, juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (1.0f);
    const float radius = 8.0f;

    g.setColour (palette::shadowDark);
    g.fillRoundedRectangle (bounds.translated (0.0f, 1.0f), radius);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (bounds, radius);
    g.setColour (box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);

    juce::Path arrow;
    const float ax = (float) width - 16.0f, ay = (float) height * 0.5f;
    arrow.addTriangle (ax - 4.0f, ay - 2.5f, ax + 4.0f, ay - 2.5f, ax, ay + 3.5f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId));
    g.fillPath (arrow);
}

} // namespace axiom
