#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace axiom;

//==============================================================================
AxiomAudioProcessorEditor::AxiomAudioProcessorEditor (AxiomAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (waveformView);
    waveformView.onFileSelected = [this] (const juce::File& f)
    {
        processor.importSampleFile (f);
    };
    waveformView.onZoneChanged = [this] (const AudioZone& z)
    {
        processor.setZoneAndReanalyze (z);
    };

    addAndMakeVisible (sphereView);
    addAndMakeVisible (patchView);

    addAndMakeVisible (presetBar);
    presetBar.onSelect   = [this] (int i) { processor.loadPresetAtIndex (i); };
    presetBar.onNavigate = [this] (int d) { processor.navigatePreset (d); };
    presetBar.onToggleFavorite = [this] { processor.togglePresetFavorite(); };
    presetBar.onDeleteCurrent  = [this] { processor.deleteCurrentPreset(); };
    presetBar.onUpdateCurrent  = [this] { processor.updateCurrentPreset(); };
    presetBar.onRevert         = [this] { processor.revertPresetChanges(); };
    presetBar.onSave = [this] (juce::String name)
    {
        if (auto result = processor.savePresetAs (name); result.failed())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                    "Save Preset", result.getErrorMessage());
    };

    addAndMakeVisible (importButton);
    importButton.onClick = [this] { waveformView.openFilePicker(); };

    addAndMakeVisible (separateButton);
    separateButton.setTooltip ("Split the imported audio into stems");
    separateButton.onClick = [this] { processor.requestStemSeparation(); };

    addAndMakeVisible (exportButton);
    exportButton.onClick = [this] { launchExport(); };

    keyboard.setAvailableRange (21, 108);   // 88 keys
    keyboard.setOctaveForMiddleC (4);
    addAndMakeVisible (keyboard);

    statusLabel.setJustificationType (juce::Justification::centredLeft);
    statusLabel.setColour (juce::Label::textColourId, palette::textDim);
    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    addAndMakeVisible (statusLabel);

    processor.addChangeListener (this);
    startTimerHz (10);

    setResizable (true, true);
    setResizeLimits (940, 640, 1800, 1200);
    setSize (1120, 720);

    refreshFromProcessor();
}

AxiomAudioProcessorEditor::~AxiomAudioProcessorEditor()
{
    processor.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

//==============================================================================
void AxiomAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (palette::background);

    // Header.
    auto header = getLocalBounds().removeFromTop (56).reduced (18, 0);

    g.setColour (palette::text);
    g.setFont (juce::Font (juce::FontOptions (24.0f)).boldened());
    g.drawText ("AXIOM", header.removeFromLeft (110), juce::Justification::centredLeft);
}

void AxiomAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    // Header row: brand left, preset bar centred, action buttons right.
    auto header = area.removeFromTop (56).reduced (18, 12);
    exportButton.setBounds (header.removeFromRight (92));
    header.removeFromRight (8);
    separateButton.setBounds (header.removeFromRight (92));
    header.removeFromRight (8);
    importButton.setBounds (header.removeFromRight (92));
    header.removeFromRight (12);
    header.removeFromLeft (110);              // brand text space (painted)
    presetBar.setBounds (header.withSizeKeepingCentre (juce::jmin (420, header.getWidth()),
                                                       header.getHeight()));

    // Status strip + keyboard at the bottom.
    statusLabel.setBounds (area.removeFromBottom (24).reduced (18, 0));
    keyboard.setBounds (area.removeFromBottom (92).reduced (14, 4));
    keyboard.setKeyWidth (juce::jmax (10.0f, (float) keyboard.getWidth() / 52.0f));

    // Centre: left column (waveform / stem chips / sphere) + right patch panel.
    auto centre = area.reduced (12, 6);
    patchView.setBounds (centre.removeFromRight (juce::jmin (400, centre.getWidth() / 2)));
    centre.removeFromRight (10);

    auto sphereArea = centre.removeFromBottom (juce::jmax (170, centre.getHeight() * 34 / 100));
    auto chipRow    = centre.removeFromBottom (36).reduced (2, 3);
    waveformView.setBounds (centre);
    sphereView.setBounds (sphereArea.withTrimmedTop (4));

    for (auto* chip : stemChips)
    {
        chip->setBounds (chipRow.removeFromLeft (juce::jmin (120, chipRow.getWidth() / juce::jmax (1, stemChips.size()))));
        chipRow.removeFromLeft (6);
    }
}

//==============================================================================
void AxiomAudioProcessorEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    refreshFromProcessor();
}

void AxiomAudioProcessorEditor::refreshFromProcessor()
{
    // Active audio (full mix or stem) -> waveform + zone.
    juce::AudioBuffer<float> audio;
    double sr = 0.0;
    juce::String name;
    if (processor.getActiveAudio (audio, sr, name))
        waveformView.setAudio (audio, sr, name);
    else if (processor.getCurrentSampleFile().existsAsFile())
        waveformView.setFileAudio (processor.getCurrentSampleFile());
    waveformView.setZone (processor.getZone());

    // Patch + constellation. Tier shows both engines when a DDSP layer is
    // loaded ("Analytical DSP v1 + DDSP measured (STFT)").
    {
        auto tier = processor.getReconstructionTier();
        if (const auto ddspTier = processor.getDdspTierName(); ddspTier.isNotEmpty())
            tier += (tier.isNotEmpty() ? " + " : "") + ddspTier;
        patchView.setPatchInfo (processor.getCurrentPatch(), tier);
    }
    if (processor.getStage() == AxiomAudioProcessor::PipelineStage::Ready
        && processor.getLastFeatures().isValid())
        sphereView.setFeatures (processor.getLastFeatures(), processor.getCurrentPatch());

    // Preset bar state.
    {
        juce::StringArray names;
        juce::Array<bool> favorites;
        for (auto& info : processor.getPresetManager().getPresets())
        {
            names.add (info.name);
            favorites.add (info.favorite);
        }
        presetBar.refresh (names, favorites,
                           processor.getPresetManager().getCurrentIndex(),
                           processor.getPresetDisplayName());
    }

    rebuildStemChips();
    updateStatusStrip();
}

void AxiomAudioProcessorEditor::rebuildStemChips()
{
    const auto names  = processor.getStemNames();
    const int  active = processor.getActiveStemIndex();

    stemChips.clear();
    if (! names.isEmpty())
    {
        auto addChip = [this] (const juce::String& text, int stemIndex, bool isActive)
        {
            auto* b = stemChips.add (new juce::TextButton (text));
            b->setClickingTogglesState (false);
            b->setToggleState (isActive, juce::dontSendNotification);
            b->onClick = [this, stemIndex] { processor.setActiveStem (stemIndex); };
            addAndMakeVisible (b);
        };

        addChip ("Full Mix", -1, active < 0);
        for (int i = 0; i < names.size(); ++i)
            addChip (names[i], i, active == i);
    }
    resized();
}

void AxiomAudioProcessorEditor::timerCallback()
{
    updateStatusStrip();
    presetBar.setDirty (processor.isPresetDirty());   // knobs move without broadcasts

    // Pipeline overlay follows the processor's stage.
    using Stage = AxiomAudioProcessor::PipelineStage;
    const auto stage    = processor.getStage();
    const auto progress = processor.getPipelineProgress();

    switch (stage)
    {
        case Stage::Empty:
            waveformView.setStatus ({}, {}, -1.0f);
            break;
        case Stage::Loading:
            waveformView.setStatus ("Importing ", "decoding audio", progress);
            break;
        case Stage::Separating:
            waveformView.setStatus ("Separating stems ",
                                    processor.getSeparationBackendName(), progress);
            break;
        case Stage::Analyzing:
            waveformView.setStatus ("Analyzing ", "pitch  spectrum  envelope  modulation", progress);
            break;
        case Stage::Reconstructing:
            waveformView.setStatus ("Reconstructing instrument ", "inferring synthesis parameters", progress);
            break;
        case Stage::Ready:
        {
            const auto& feats = processor.getLastFeatures();
            juce::String detail;
            if (feats.f0Hz > 0.0f)
                detail = "root " + juce::MidiMessage::getMidiNoteName (feats.rootMidiNote, true, true, 4)
                       + "    " + juce::String (feats.f0Hz, 1) + " Hz";
            waveformView.setStatus (processor.isExporting() ? "Exporting " : "Ready   play the keyboard",
                                    detail,
                                    processor.isExporting() ? processor.getExportProgress() : -1.0f);
            break;
        }
        case Stage::Failed:
            waveformView.setStatus ("Analysis failed", "adjust the zone or pick another stem", -1.0f);
            break;
    }

    const bool busy = processor.isPipelineBusy();
    importButton.setEnabled (! busy);
    separateButton.setEnabled (! busy && processor.getCurrentSampleFile().existsAsFile()
                               && stage != Stage::Empty && stage != Stage::Loading);
    exportButton.setEnabled (stage == Stage::Ready && ! busy && ! processor.isExporting());
}

void AxiomAudioProcessorEditor::updateStatusStrip()
{
    const auto sr    = processor.getReportedSampleRate();
    const auto block = processor.getReportedBlockSize();
    const auto ms    = sr > 0.0 ? 1000.0 * block / sr : 0.0;

    statusLabel.setText (juce::String::formatted ("CPU %4.1f%%      Voices %d/%d      %d smp @ %.0f kHz = %.1f ms      ",
                                                  processor.getCpuLoad() * 100.0f,
                                                  processor.getVoiceCount(),
                                                  axiom::SynthEngine::maxVoices,
                                                  block, sr / 1000.0, ms)
                             + "AI: " + processor.getAiBackendName()
                             + "      Separation: " + processor.getSeparationBackendName()
                             + "      DDSP: " + (processor.getDdspTierName().isNotEmpty()
                                                     ? processor.getDdspTierName()
                                                     : juce::String ("none")),
                         juce::dontSendNotification);
}

//==============================================================================
void AxiomAudioProcessorEditor::launchExport()
{
    exportChooser = std::make_unique<juce::FileChooser> ("Export instrument to folder ",
                                                         juce::File::getSpecialLocation (
                                                             juce::File::userDocumentsDirectory));

    exportChooser->launchAsync (juce::FileBrowserComponent::openMode
                                    | juce::FileBrowserComponent::canSelectDirectories,
                                [this] (const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (! dir.isDirectory())
            return;

        processor.startExport (dir, [] (juce::Result result)
        {
            juce::AlertWindow::showMessageBoxAsync (
                result.wasOk() ? juce::MessageBoxIconType::InfoIcon
                               : juce::MessageBoxIconType::WarningIcon,
                "Axiom Export",
                result.wasOk() ? "Instrument exported: .axiom patch, SFZ + samples, DecentSampler preset."
                               : result.getErrorMessage());
        });
    });
}
