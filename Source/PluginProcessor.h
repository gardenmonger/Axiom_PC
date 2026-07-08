/*
  ==============================================================================

    PluginProcessor.h

    Axiom — AI instrument reconstruction synthesizer.

    The processor is the composition root: it owns the synth engine, the
    AI/analysis pipeline, the parameter tree and the background threads,
    and wires them together (dependency injection — subsystems never reach
    for globals).

    Threading model
    ---------------
      Audio thread       processBlock -> SynthEngine. Lock-free, allocation-free.
      Message thread     GUI, parameter automation, patch application
                         (engine.setPatch via atomic double buffer).
      Analysis thread    file decode -> zone crop/fade -> AudioAnalyzer ->
                         Reconstructor; posts the finished patch back.
      Separation thread  full mix -> stems (neural ONNX model when installed,
                         HPSS DSP fallback otherwise).
      Export thread      offline multisample render + file writing.

    Data model (guarded by dataLock, never touched by the audio thread):
      sourceAudio  — the imported file, decoded once (<= 30 s window)
      stems        — separation results (empty until Separate is run)
      activeStem   — which audio feeds analysis (-1 = full mix)
      zone         — user-selected slice with fades (Core/AudioZone)

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include "Core/AnalysisFeatures.h"
#include "Core/AudioZone.h"
#include "Core/InstrumentPatch.h"
#include "AI/InferenceEngine.h"
#include "AI/Reconstructor.h"
#include "AI/StemSeparation.h"
#include "Preset/PresetManager.h"
#include "Synth/SynthEngine.h"

//==============================================================================
class AxiomAudioProcessor : public juce::AudioProcessor,
                            public juce::ChangeBroadcaster
{
public:
    //==============================================================================
    AxiomAudioProcessor();
    ~AxiomAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }

    //==============================================================================
    const juce::String getName() const override            { return JucePlugin_Name; }
    bool acceptsMidi() const override                      { return true; }
    bool producesMidi() const override                     { return false; }
    bool isMidiEffect() const override                     { return false; }
    double getTailLengthSeconds() const override           { return 4.0; }

    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Reconstruction pipeline (message-thread API for the editor)
    //==============================================================================
    enum class PipelineStage { Empty, Loading, Separating, Analyzing, Reconstructing, Ready, Failed };

    /** Kicks off decode -> analyze -> reconstruct on the analysis thread.
        Cancels any run already in flight. */
    void importSampleFile (const juce::File& file);

    /** Splits the imported source into stems on the separation thread. */
    void requestStemSeparation();

    /** -1 = full mix, otherwise an index into getStemNames(). Triggers
        re-analysis of the selected audio. */
    void setActiveStem (int index);
    int  getActiveStemIndex() const;
    juce::StringArray getStemNames() const;

    axiom::AudioZone getZone() const;
    /** Stores the zone and re-learns the instrument from the new slice. */
    void setZoneAndReanalyze (const axiom::AudioZone& zone);

    /** Copies the active audio (full mix or selected stem, uncropped) for
        display. Returns false when nothing is loaded. */
    bool getActiveAudio (juce::AudioBuffer<float>& out, double& sampleRate,
                         juce::String& name) const;

    PipelineStage getStage() const noexcept                { return stage.load(); }
    float getPipelineProgress() const noexcept             { return pipelineProgress.load(); }
    juce::File getCurrentSampleFile() const                { return sampleFile; }
    bool isPipelineBusy() const;

    axiom::InstrumentPatch getCurrentPatch() const         { return engine.getPatch(); }
    const axiom::AnalysisFeatures& getLastFeatures() const { return lastFeatures; }
    juce::String getReconstructionTier() const             { return lastTier; }
    juce::String getAiBackendName() const                  { return inference->getBackendName(); }
    juce::String getSeparationBackendName() const;

    //==============================================================================
    // Presets (message-thread API for the editor)
    //==============================================================================
    axiom::PresetManager& getPresetManager() noexcept      { return presetManager; }

    void loadPresetAtIndex (int index);
    void navigatePreset (int delta);                        // -1 / +1, wraps
    juce::Result savePresetAs (const juce::String& name);
    void togglePresetFavorite();
    void deleteCurrentPreset();
    /** Current preset name, or "<sample> (unsaved)" / "Init". */
    juce::String getPresetDisplayName() const;

    /** The sound as heard right now: base patch with every knob/automation
        value folded back in. This is what presets save. */
    axiom::InstrumentPatch getCurrentPatchWithOverrides() const;

    /** True when the current preset's knobs have been edited since load/save. */
    bool isPresetDirty() const;
    /** Overwrites the current preset file with the edited sound. */
    void updateCurrentPreset();
    /** Discards knob edits: reloads the current preset as saved. */
    void revertPresetChanges();

    //==============================================================================
    /** Renders the multisample export on a background thread. `onFinished`
        is called on the message thread. */
    void startExport (const juce::File& destinationDir,
                      std::function<void (juce::Result)> onFinished);
    bool isExporting() const noexcept                      { return exporting.load(); }
    float getExportProgress() const noexcept
    {
        auto p = exportProgressShared;
        return p != nullptr ? p->load() : 0.0f;
    }

    //==============================================================================
    float getCpuLoad() const noexcept                      { return cpuLoad.load(); }
    int   getVoiceCount() const noexcept                   { return engine.getActiveVoiceCount(); }
    double getReportedSampleRate() const noexcept          { return currentSampleRate.load(); }
    int   getReportedBlockSize() const noexcept            { return currentBlockSize.load(); }

    juce::AudioProcessorValueTreeState apvts;
    juce::MidiKeyboardState keyboardState;

private:
    //==============================================================================
    class AnalysisJob : public juce::Thread
    {
    public:
        explicit AnalysisJob (AxiomAudioProcessor& p)
            : juce::Thread ("Axiom Analysis"), owner (p) {}
        void run() override    { owner.runPipeline(); }
    private:
        AxiomAudioProcessor& owner;
    };

    class SeparationJob : public juce::Thread
    {
    public:
        explicit SeparationJob (AxiomAudioProcessor& p)
            : juce::Thread ("Axiom Separation"), owner (p) {}
        void run() override    { owner.runSeparation(); }
    private:
        AxiomAudioProcessor& owner;
    };

    void runPipeline();                                     // analysis thread
    void runSeparation();                                   // separation thread
    void postFailure (const juce::String& why);
    void applyReconstruction (const axiom::InstrumentPatch& patch,
                              const axiom::AnalysisFeatures& features,
                              const juce::String& tier);    // message thread
    void applyStems (std::vector<axiom::Stem>&& stems);     // message thread
    void applyPatchToParameters (const axiom::InstrumentPatch& patch);
    void wireOverrides();

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //==============================================================================
    axiom::SynthEngine      engine;
    axiom::RuntimeOverrides overrides;

    std::unique_ptr<axiom::IInferenceEngine>    inference;
    std::unique_ptr<axiom::HybridReconstructor> reconstructor;

    axiom::PresetManager presetManager;

    juce::AudioFormatManager formatManager;

    AnalysisJob   analysisJob   { *this };
    SeparationJob separationJob { *this };

    // --- Audio data model (dataLock; message + worker threads only) ----------
    mutable juce::CriticalSection dataLock;
    juce::AudioBuffer<float>  sourceAudio;
    double                    sourceSampleRate = 0.0;
    std::vector<axiom::Stem>  stems;
    int                       activeStem = -1;
    axiom::AudioZone          zone;
    juce::File                pendingFile;
    bool                      pendingLoad = false;

    // Message-thread state (editor reads on message thread only).
    juce::File              sampleFile;
    axiom::AnalysisFeatures lastFeatures;
    juce::String            lastTier;
    juce::String            presetSnapshotJson;   // effective patch at preset load/save

    std::atomic<PipelineStage> stage { PipelineStage::Empty };
    std::atomic<float> pipelineProgress { -1.0f };

    std::atomic<bool>  exporting { false };
    std::shared_ptr<std::atomic<float>> exportProgressShared;

    std::atomic<float>  cpuLoad { 0.0f };
    std::atomic<double> currentSampleRate { 44100.0 };
    std::atomic<int>    currentBlockSize { 512 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (AxiomAudioProcessor)
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AxiomAudioProcessor)
};
