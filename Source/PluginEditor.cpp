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

    // Separate / Export are primary actions in the SG button system.
    addAndMakeVisible (separateButton);
    separateButton.setTooltip ("Split the imported audio into stems");
    separateButton.onClick = [this] { processor.requestStemSeparation(); };
    AxiomLookAndFeel::setPrimary (separateButton);

    addAndMakeVisible (exportButton);
    exportButton.onClick = [this] { launchExport(); };
    AxiomLookAndFeel::setPrimary (exportButton);

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

    // Navigation bar: elevated strip with a hairline base (SG Vol 3 Ch 2).
    auto headerStrip = getLocalBounds().removeFromTop (metrics::navBarHeight);
    g.setGradientFill (juce::ColourGradient (palette::panel,
                                             0.0f, (float) headerStrip.getY(),
                                             palette::background,
                                             0.0f, (float) headerStrip.getBottom(), false));
    g.fillRect (headerStrip);
    g.setColour (palette::outline.withAlpha (0.5f));
    g.fillRect (headerStrip.removeFromBottom (1));

    // Brand: neural-core dot + wordmark.
    auto header = getLocalBounds().removeFromTop (metrics::navBarHeight).reduced (18, 0);
    {
        auto brand = header.removeFromLeft (110);
        const float cy = (float) brand.getCentreY();
        const float cx = (float) brand.getX() + 7.0f;

        g.setColour (palette::accent.withAlpha (0.25f));            // halo
        g.fillEllipse (cx - 8.0f, cy - 8.0f, 16.0f, 16.0f);
        g.setColour (palette::accent);                              // core
        g.fillEllipse (cx - 3.5f, cy - 3.5f, 7.0f, 7.0f);

        g.setColour (palette::text);
        g.setFont (juce::Font (juce::FontOptions (24.0f)).boldened());
        g.drawText ("AXIOM", brand.withTrimmedLeft (22), juce::Justification::centredLeft);
    }

    // Performance dock: raised panel behind keyboard + status (SG Vol 3 Ch 6).
    if (! dockArea.isEmpty())
        AxiomLookAndFeel::drawPanel (g, dockArea.toFloat(), metrics::radiusCard,
                                     palette::panel);
}

void AxiomAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    // Nav bar: brand left, preset bar centred, action buttons right.
    auto header = area.removeFromTop (metrics::navBarHeight).reduced (18, 14);
    exportButton.setBounds (header.removeFromRight (96));
    header.removeFromRight (metrics::grid);
    separateButton.setBounds (header.removeFromRight (96));
    header.removeFromRight (metrics::grid);
    importButton.setBounds (header.removeFromRight (96));
    header.removeFromRight (12);
    header.removeFromLeft (110);              // brand mark space (painted)
    presetBar.setBounds (header.withSizeKeepingCentre (juce::jmin (420, header.getWidth()),
                                                       header.getHeight()));

    // Performance dock: keyboard + status strip on one raised panel.
    dockArea = area.removeFromBottom (128).reduced (12, 4);
    auto dock = dockArea.reduced (10, 6);
    statusLabel.setBounds (dock.removeFromBottom (22));
    keyboard.setBounds (dock.reduced (2, 2));
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
