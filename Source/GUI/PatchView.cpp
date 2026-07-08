#include "PatchView.h"
#include "AxiomLookAndFeel.h"
#include "../Core/ParamIDs.h"

namespace axiom
{

namespace
{
    const char* rowTitles[PatchView::numRows] = { "FILTER", "AMP ENVELOPE", "TONE", "SPACE" };
}

PatchView::PatchView (juce::AudioProcessorValueTreeState& apvts)
{
    using namespace params;

    // Row 1 — FILTER
    addKnob (apvts, cutoff,       "Cutoff");
    addKnob (apvts, resonance,    "Reso");
    addKnob (apvts, drive,        "Drive");
    addKnob (apvts, filterEnvAmt, "Env Amt");

    // Row 2 — AMP ENVELOPE
    addKnob (apvts, ampAttack,  "Attack");
    addKnob (apvts, ampDecay,   "Decay");
    addKnob (apvts, ampSustain, "Sustain");
    addKnob (apvts, ampRelease, "Release");

    // Row 3 — TONE
    addKnob (apvts, detune,     "Detune");
    addKnob (apvts, noiseLevel, "Noise");
    addKnob (apvts, satDrive,   "Sat");
    addKnob (apvts, width,      "Width");

    // Row 4 — SPACE
    addKnob (apvts, chorusMix,  "Chorus");
    addKnob (apvts, delayMix,   "Delay");
    addKnob (apvts, reverbMix,  "Reverb");
    addKnob (apvts, masterGain, "Gain");
}

void PatchView::addKnob (juce::AudioProcessorValueTreeState& apvts,
                         const char* paramID, const juce::String& text)
{
    auto knob = std::make_unique<Knob>();

    knob->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob->slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    knob->slider.setPopupDisplayEnabled (true, true, getTopLevelComponent());
    addAndMakeVisible (knob->slider);

    knob->label.setText (text, juce::dontSendNotification);
    knob->label.setJustificationType (juce::Justification::centred);
    knob->label.setColour (juce::Label::textColourId, palette::textDim);
    knob->label.setFont (juce::Font (juce::FontOptions (11.5f)));
    addAndMakeVisible (knob->label);

    knob->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        apvts, paramID, knob->slider);

    knobs.push_back (std::move (knob));
}

//==============================================================================
void PatchView::setPatchInfo (const InstrumentPatch& patch, const juce::String& tierName)
{
    description = patch_io::describe (patch);
    tier        = tierName;
    confidence  = patch.confidence;
    character   = patch.character;

    juce::StringArray oscBits;
    for (auto& o : patch.oscs)
        if (o.enabled)
            oscBits.add (toString (o.type));
    oscSummary = oscBits.isEmpty() ? juce::String() : oscBits.joinIntoString (" + ");

    repaint();
}

//==============================================================================
void PatchView::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    AxiomLookAndFeel::drawPanel (g, bounds, 14.0f);

    auto area = getLocalBounds().reduced (16, 12);

    // Title + tier.
    {
        auto header = area.removeFromTop (22);
        g.setColour (palette::text);
        g.setFont (juce::Font (juce::FontOptions (14.0f)).boldened());
        g.drawText ("RECONSTRUCTED INSTRUMENT", header, juce::Justification::centredLeft);

        if (tier.isNotEmpty())
        {
            g.setColour (palette::accentAlt);
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (tier, header, juce::Justification::centredRight);
        }
    }

    // Discovery description.
    {
        auto descArea = area.removeFromTop (34);
        g.setColour (palette::textDim);
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        g.drawFittedText (description, descArea, juce::Justification::topLeft, 2);
    }

    // Confidence bar.
    {
        auto row = area.removeFromTop (16);
        g.setColour (palette::textDim);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        auto labelArea = row.removeFromLeft (78);
        g.drawText ("Confidence", labelArea, juce::Justification::centredLeft);

        auto bar = row.reduced (0, 5).toFloat();
        g.setColour (palette::outline);
        g.fillRoundedRectangle (bar, 3.0f);
        const auto fillColour = confidence > 0.6f ? palette::accent
                              : confidence > 0.3f ? palette::warning
                                                  : palette::textDim;
        g.setColour (fillColour);
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (4.0f, bar.getWidth() * confidence)), 3.0f);
    }

    area.removeFromTop (6);

    // Character profile.
    {
        auto row = area.removeFromTop (30);
        const std::pair<const char*, float> traits[] = {
            { "Bright", character.brightness }, { "Warm",  character.warmth },
            { "Motion", character.movement },   { "Complex", character.complexity } };

        const int cell = row.getWidth() / 4;
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        for (int i = 0; i < 4; ++i)
        {
            auto c = row.removeFromLeft (cell).reduced (i == 0 ? 0 : 4, 0);
            g.setColour (palette::textDim);
            g.drawText (traits[i].first, c.removeFromTop (13), juce::Justification::centredLeft);
            auto bar = c.removeFromTop (5).toFloat();
            g.setColour (palette::outline);
            g.fillRoundedRectangle (bar, 2.5f);
            g.setColour (palette::accentAlt.withAlpha (0.85f));
            g.fillRoundedRectangle (bar.withWidth (juce::jmax (3.0f, bar.getWidth() * traits[i].second)), 2.5f);
        }
    }

    // Row captions above each knob row.
    g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened());
    for (int rowIdx = 0; rowIdx < numRows; ++rowIdx)
    {
        if (rowAreas[(size_t) rowIdx].isEmpty())
            continue;
        g.setColour (palette::textDim.withAlpha (0.85f));
        g.drawText (juce::String (rowTitles[rowIdx])
                        + (rowIdx == 0 && oscSummary.isNotEmpty() ? " OSC: " + oscSummary : ""),
                    rowAreas[(size_t) rowIdx].withHeight (14),
                    juce::Justification::centredLeft);
    }
}

//==============================================================================
void PatchView::resized()
{
    auto area = getLocalBounds().reduced (16, 12);
    area.removeFromTop (22 + 34 + 16 + 6 + 30 + 4);   // header stack (mirrors paint)

    const int rowHeight = juce::jmax (58, area.getHeight() / numRows);

    for (int rowIdx = 0; rowIdx < numRows; ++rowIdx)
    {
        auto row = area.removeFromTop (rowHeight);
        rowAreas[(size_t) rowIdx] = row;
        row.removeFromTop (14);                        // caption space

        const int cell = row.getWidth() / knobsPerRow;
        for (int col = 0; col < knobsPerRow; ++col)
        {
            const size_t idx = (size_t) (rowIdx * knobsPerRow + col);
            if (idx >= knobs.size()) break;

            auto cellArea = row.removeFromLeft (cell);
            auto labelArea = cellArea.removeFromBottom (14);
            knobs[idx]->slider.setBounds (cellArea.reduced (2));
            knobs[idx]->label.setBounds (labelArea);
        }
    }
}

} // namespace axiom
