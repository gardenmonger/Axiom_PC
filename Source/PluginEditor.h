/*
  ==============================================================================

    PluginEditor.h

    Main window layout:

        ┌────────────────────────────────────────────────────────────┐
        │ AXIOM · tagline        [Import] [Separate] [Export]        │  header
        ├──────────────────────────────┬─────────────────────────────┤
        │  WaveformView                │  RECONSTRUCTED INSTRUMENT   │
        │  (drop zone · zone markers · │  discovery · confidence     │
        │   fades · analysis status)   │  character · 16 macro knobs │
        │──────────────────────────────│  (PatchView)                │
        │  [Full Mix][Stem chips…]     │                             │
        │──────────────────────────────│                             │
        │  EmbeddingSphereView         │                             │
        │  (neural constellation)      │                             │
        ├──────────────────────────────┴─────────────────────────────┤
        │            88-key keyboard (MidiKeyboardComponent)         │
        │  CPU · voices · latency · engine backend                   │  status
        └────────────────────────────────────────────────────────────┘

    The editor is a passive view: it renders processor state (ChangeListener
    for pipeline transitions, 10 Hz timer for meters) and forwards user
    intent (import/separate/zone/stem/export/keys). All heavy work stays in
    the processor.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "GUI/AxiomLookAndFeel.h"
#include "GUI/EmbeddingSphereView.h"
#include "GUI/PatchView.h"
#include "GUI/PresetBar.h"
#include "GUI/WaveformView.h"

//==============================================================================
class AxiomAudioProcessorEditor : public juce::AudioProcessorEditor,
                                  private juce::ChangeListener,
                                  private juce::Timer
{
public:
    explicit AxiomAudioProcessorEditor (AxiomAudioProcessor&);
    ~AxiomAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;

    void refreshFromProcessor();
    void rebuildStemChips();
    void updateStatusStrip();
    void launchExport();

    AxiomAudioProcessor& processor;

    axiom::AxiomLookAndFeel lookAndFeel;

    axiom::WaveformView        waveformView;
    axiom::EmbeddingSphereView sphereView;
    axiom::PatchView           patchView { processor.apvts };
    axiom::PresetBar           presetBar;

    juce::TextButton importButton   { "Import" };
    juce::TextButton separateButton { "Separate" };
    juce::TextButton exportButton   { "Export" };

    juce::OwnedArray<juce::TextButton> stemChips;   // [0] = Full Mix

    juce::MidiKeyboardComponent keyboard { processor.keyboardState,
                                           juce::MidiKeyboardComponent::horizontalKeyboard };

    juce::Label statusLabel;

    std::unique_ptr<juce::FileChooser> exportChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AxiomAudioProcessorEditor)
};
