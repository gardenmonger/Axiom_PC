#include "AxiomLookAndFeel.h"

namespace axiom
{

AxiomLookAndFeel::AxiomLookAndFeel()
{
    // SG typography: Inter when installed, otherwise the system fallback
    // (Segoe UI on Windows) per the Vol 1 font stack.
    for (auto& name : juce::Font::findAllTypefaceNames())
        if (name == "Inter")
        {
            setDefaultSansSerifTypefaceName ("Inter");
            break;
        }

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

    // SG performance dock: warm-gray whites, dark graphite blacks.
    setColour (juce::MidiKeyboardComponent::whiteNoteColourId,        juce::Colour (0xffe9e6e0));
    setColour (juce::MidiKeyboardComponent::blackNoteColourId,        juce::Colour (0xff171a20));
    setColour (juce::MidiKeyboardComponent::keySeparatorLineColourId, juce::Colour (0xff313943));
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
    // Neomorphic dual shadow: dark drop lower-right, light sheen upper-left.
    g.setColour (palette::shadowDark);
    g.fillRoundedRectangle (b.translated (0.0f, 2.5f), radius);
    g.setColour (palette::shadowLight);
    g.fillRoundedRectangle (b.translated (0.0f, -1.0f), radius);

    // Surface with a faint top-down glass gradient so the panel reads as lit
    // from above rather than flat.
    g.setGradientFill (juce::ColourGradient (fill.brighter (0.045f), b.getX(), b.getY(),
                                             fill.darker (0.05f),    b.getX(), b.getBottom(),
                                             false));
    g.fillRoundedRectangle (b, radius);

    g.setColour (palette::outline.withAlpha (0.6f));
    g.drawRoundedRectangle (b.reduced (0.5f), radius, 1.0f);

    // Upper-left highlight edge (light source direction).
    g.setColour (palette::glass);
    g.drawRoundedRectangle (b.reduced (1.5f).withTrimmedBottom (b.getHeight() * 0.5f),
                            radius, 1.0f);
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

    const bool enabled = slider.isEnabled();
    const bool hot     = enabled && slider.isMouseOverOrDragging();

    // Glass body: soft dual shadow, vertical sheen, inner shadow at the base.
    {
        const auto body = juce::Rectangle<float> (size, size).withCentre (centre).reduced (size * 0.12f);
        g.setColour (palette::shadowDark);
        g.fillEllipse (body.translated (0.0f, 2.0f));
        g.setColour (palette::shadowLight);
        g.fillEllipse (body.translated (0.0f, -1.5f));

        g.setGradientFill (juce::ColourGradient (palette::card.brighter (0.10f),
                                                 centre.x, body.getY(),
                                                 palette::panelRaised.darker (0.18f),
                                                 centre.x, body.getBottom(), false));
        g.fillEllipse (body);

        // Glass highlight: small bright arc at the top of the cap.
        g.setColour (palette::glass.withAlpha (0.10f));
        g.fillEllipse (body.reduced (body.getWidth() * 0.14f)
                           .translated (0.0f, -body.getHeight() * 0.10f));

        g.setColour (palette::outline);
        g.drawEllipse (body, 1.0f);
    }

    // Outer illumination ring: track + Neural Blue value arc with glow.
    const float arcRadius = radius - 1.5f;
    const float thickness = juce::jmax (2.0f, size * 0.055f);

    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         startAngle, endAngle, true);
    g.setColour (palette::outline);
    g.strokePath (track, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    const auto ringColour = enabled ? palette::accent : palette::textDim.withAlpha (0.4f);
    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         startAngle, angle, true);

    // Halo pass first (wider, translucent — brighter while hovered/dragged),
    // then the crisp value arc on top.
    g.setColour (ringColour.withAlpha (hot ? 0.45f : 0.22f));
    g.strokePath (value, juce::PathStrokeType (thickness * 2.4f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    g.setColour (hot ? ringColour.brighter (0.15f) : ringColour);
    g.strokePath (value, juce::PathStrokeType (thickness, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // Tick indicator: a line from mid-cap to the cap edge (hardware feel).
    const auto tipOuter = centre.getPointOnCircumference (radius * 0.68f, angle);
    const auto tipInner = centre.getPointOnCircumference (radius * 0.30f, angle);
    g.setColour (enabled ? palette::text : palette::textDim);
    g.drawLine ({ tipInner, tipOuter }, juce::jmax (2.0f, size * 0.045f));
}

//==============================================================================
void AxiomLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                             const juce::Colour& colour,
                                             bool highlighted, bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (1.0f);
    const float radius = juce::jmin (metrics::radiusButton, bounds.getHeight() * 0.5f);

    const bool primary = button.getProperties().contains ("sgPrimary");
    const bool lit     = button.getToggleState() || primary;

    if (! down)
    {
        // Raised: soft floating shadow.
        g.setColour (palette::shadowDark);
        g.fillRoundedRectangle (bounds.translated (0.0f, 1.5f), radius);
        if (lit)   // active elements carry a Neural Blue halo
        {
            g.setColour (palette::accent.withAlpha (highlighted ? 0.30f : 0.18f));
            g.fillRoundedRectangle (bounds.expanded (1.5f), radius + 1.5f);
        }
    }

    auto fill = primary ? palette::accent.withMultipliedBrightness (0.92f)
                        : colour;
    if (down)             fill = primary ? palette::accent.darker (0.25f)
                                         : palette::accent.withAlpha (0.30f);
    else if (highlighted) fill = fill.brighter (0.08f);

    if (primary && ! button.isEnabled())
        fill = fill.withAlpha (0.3f);   // disabled primaries: 30% opacity per spec

    g.setGradientFill (juce::ColourGradient (fill.brighter (down ? 0.0f : 0.06f),
                                             bounds.getX(), bounds.getY(),
                                             fill.darker (down ? 0.0f : 0.08f),
                                             bounds.getX(), bounds.getBottom(), false));
    g.fillRoundedRectangle (bounds, radius);

    if (down)
    {
        // Pressed: inset neomorphic shadow (dark lip at the top edge).
        g.setColour (palette::shadowDark.withAlpha (0.35f));
        g.fillRoundedRectangle (bounds.withHeight (juce::jmin (5.0f, bounds.getHeight())), radius);
    }

    g.setColour (primary ? palette::accent.brighter (0.2f).withAlpha (0.8f)
                 : highlighted || down || button.getToggleState()
                     ? palette::accent.withAlpha (0.6f)
                     : palette::outline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), radius, 1.0f);
}

juce::Font AxiomLookAndFeel::getTextButtonFont (juce::TextButton& button, int buttonHeight)
{
    auto font = juce::Font (juce::FontOptions ((float) juce::jmin (15, buttonHeight - 6)))
                    .boldened();
    if (button.getProperties().contains ("sgPrimary"))
        font = font.withHeight (font.getHeight() + 0.5f);
    return font;
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
    const float radius = juce::jmin (metrics::radiusButton, bounds.getHeight() * 0.5f);

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
