#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "AI/DdspEncoder.h"
#include "Analysis/AudioAnalyzer.h"
#include "Core/ParamIDs.h"
#include "Export/InstrumentExporter.h"

using namespace axiom;

//==============================================================================
AxiomAudioProcessor::AxiomAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    formatManager.registerBasicFormats();

    inference     = createInferenceEngine();
    reconstructor = std::make_unique<HybridReconstructor> (*inference);

    wireOverrides();
    engine.setOverrides (&overrides);

    // Sensible default patch so the keyboard makes sound before any import.
    InstrumentPatch defaultPatch;
    defaultPatch.oscs[0].enabled = true;
    defaultPatch.oscs[0].type    = OscType::Saw;
    defaultPatch.oscs[0].level   = 0.9f;
    defaultPatch.filter.cutoffHz = 9000.0f;
    engine.setPatch (defaultPatch);
    applyPatchToParameters (defaultPatch);
}

AxiomAudioProcessor::~AxiomAudioProcessor()
{
    analysisJob.stopThread (4000);
    separationJob.stopThread (4000);
}

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout AxiomAudioProcessor::createParameterLayout()
{
    using namespace juce;
    using namespace params;
    std::vector<std::unique_ptr<RangedAudioParameter>> p;

    auto freqRange = NormalisableRange<float> (20.0f, 20000.0f, 0.0f, 0.25f);
    auto envRange  = [] (float lo, float hi) { return NormalisableRange<float> (lo, hi, 0.0f, 0.3f); };

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { cutoff, 1 },       "Cutoff",        freqRange, 18000.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { resonance, 1 },    "Resonance",     0.0f, 1.0f, 0.1f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { drive, 1 },        "Filter Drive",  0.0f, 1.0f, 0.0f));
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { filterType, 1 },  "Filter Type",
                                                         StringArray { "Low-pass", "Band-pass", "High-pass" }, 0));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { filterEnvAmt, 1 }, "Filter Env Amt",
                                                        NormalisableRange<float> (-36.0f, 36.0f), 0.0f));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampAttack, 1 },  "Amp Attack",  envRange (0.001f, 4.0f), 0.005f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampDecay, 1 },   "Amp Decay",   envRange (0.005f, 8.0f), 0.2f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampSustain, 1 }, "Amp Sustain", 0.0f, 1.0f, 0.8f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ampRelease, 1 }, "Amp Release", envRange (0.01f, 8.0f), 0.3f));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { fltAttack, 1 },  "Filter Attack",  envRange (0.001f, 4.0f), 0.005f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { fltDecay, 1 },   "Filter Decay",   envRange (0.005f, 8.0f), 0.2f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { fltSustain, 1 }, "Filter Sustain", 0.0f, 1.0f, 0.8f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { fltRelease, 1 }, "Filter Release", envRange (0.01f, 8.0f), 0.3f));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { lfoRate, 1 },   "LFO Rate",
                                                        NormalisableRange<float> (0.05f, 20.0f, 0.0f, 0.5f), 5.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { lfoPitch, 1 },  "LFO > Pitch",  0.0f, 100.0f, 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { lfoCutoff, 1 }, "LFO > Cutoff", 0.0f, 24.0f, 0.0f));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { detune, 1 },     "Detune",  0.0f, 50.0f, 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { noiseLevel, 1 }, "Noise",   0.0f, 1.0f, 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { glide, 1 },      "Glide",
                                                        NormalisableRange<float> (0.0f, 500.0f, 0.0f, 0.5f), 0.0f));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { satDrive, 1 },      "Saturation",     0.0f, 1.0f, 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { chorusMix, 1 },     "Chorus",         0.0f, 1.0f, 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { delayMix, 1 },      "Delay Mix",      0.0f, 1.0f, 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { delayTime, 1 },     "Delay Time",
                                                        NormalisableRange<float> (20.0f, 1500.0f, 0.0f, 0.5f), 350.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { delayFeedback, 1 }, "Delay Feedback", 0.0f, 0.9f, 0.35f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { reverbMix, 1 },     "Reverb",         0.0f, 1.0f, 0.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { reverbSize, 1 },    "Reverb Size",    0.0f, 1.0f, 0.5f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { width, 1 },         "Width",          0.0f, 2.0f, 1.0f));

    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { masterGain, 1 },    "Master",
                                                        NormalisableRange<float> (-24.0f, 12.0f), 0.0f));

    // Order matches axiom::EngineMode. Without a DDSP timbre the engine
    // falls back to the recipe path, so any choice is always audible.
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { engineMode, 1 },   "Engine",
                                                         StringArray { "Recipe Synth", "DDSP Resynth", "Both Layered" }, 0));

    // SK-1 pitch-stretch layer: the raw source sample replayed varispeed
    // (speed = pitch, like a Casio SK-1), toggled independently of the
    // engine mode and blended against the other layers by the level knobs.
    p.push_back (std::make_unique<AudioParameterBool>  (ParameterID { stretchOn, 1 },    "SK-1 Stretch", false));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { recipeLevel, 1 },  "Recipe Level", 0.0f, 1.0f, 1.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { ddspLevel, 1 },    "DDSP Level",   0.0f, 1.0f, 1.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { stretchLevel, 1 }, "SK-1 Level",   0.0f, 1.0f, 1.0f));
    p.push_back (std::make_unique<AudioParameterFloat> (ParameterID { stretchCrush, 1 }, "SK-1 Crush",   0.0f, 1.0f, 0.2f));

    // Sample-layer playback engine (order matches dsp::SkSampler::Engine):
    // the vintage SK-1 read (linear interp + crush) or a modern FL Studio
    // style band-limited sinc resampler — clean repitching, crush bypassed.
    p.push_back (std::make_unique<AudioParameterChoice> (ParameterID { samplerEngine, 1 }, "Sampler Engine",
                                                         StringArray { "SK-1 Lo-Fi", "Modern HQ" }, 0));
    return { p.begin(), p.end() };
}

void AxiomAudioProcessor::wireOverrides()
{
    using namespace params;
    auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id); };

    overrides.cutoff        = raw (cutoff);
    overrides.resonance     = raw (resonance);
    overrides.drive         = raw (drive);
    overrides.filterType    = raw (filterType);
    overrides.filterEnvAmt  = raw (filterEnvAmt);

    overrides.ampAttack     = raw (ampAttack);
    overrides.ampDecay      = raw (ampDecay);
    overrides.ampSustain    = raw (ampSustain);
    overrides.ampRelease    = raw (ampRelease);

    overrides.fltAttack     = raw (fltAttack);
    overrides.fltDecay      = raw (fltDecay);
    overrides.fltSustain    = raw (fltSustain);
    overrides.fltRelease    = raw (fltRelease);

    overrides.lfoRate       = raw (lfoRate);
    overrides.lfoPitch      = raw (lfoPitch);
    overrides.lfoCutoff     = raw (lfoCutoff);

    overrides.detune        = raw (detune);
    overrides.noiseLevel    = raw (noiseLevel);
    overrides.glide         = raw (glide);

    overrides.satDrive      = raw (satDrive);
    overrides.chorusMix     = raw (chorusMix);
    overrides.delayMix      = raw (delayMix);
    overrides.delayTime     = raw (delayTime);
    overrides.delayFeedback = raw (delayFeedback);
    overrides.reverbMix     = raw (reverbMix);
    overrides.reverbSize    = raw (reverbSize);
    overrides.width         = raw (width);

    overrides.masterGain    = raw (masterGain);

    overrides.engineMode    = raw (engineMode);

    overrides.stretchOn     = raw (stretchOn);
    overrides.recipeLevel   = raw (recipeLevel);
    overrides.ddspLevel     = raw (ddspLevel);
    overrides.stretchLevel  = raw (stretchLevel);
    overrides.stretchCrush  = raw (stretchCrush);
    overrides.samplerEngine = raw (samplerEngine);
}

//==============================================================================
void AxiomAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    currentSampleRate.store (sampleRate);
    currentBlockSize.store (samplesPerBlock);
}

void AxiomAudioProcessor::releaseResources()
{
    engine.releaseResources();
}

bool AxiomAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void AxiomAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                        juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    const auto startTicks = juce::Time::getHighResolutionTicks();
    const int  numSamples = buffer.getNumSamples();

    keyboardState.processNextMidiBuffer (midiMessages, 0, numSamples, true);
    engine.processBlock (buffer, midiMessages);
    midiMessages.clear();

    // CPU load: fraction of the real-time budget used, smoothed.
    if (numSamples > 0)
    {
        const double elapsed = juce::Time::highResolutionTicksToSeconds (
                                   juce::Time::getHighResolutionTicks() - startTicks);
        const double budget  = numSamples / currentSampleRate.load();
        const float  instant = (float) juce::jlimit (0.0, 4.0, elapsed / juce::jmax (budget, 1.0e-6));
        cpuLoad.store (cpuLoad.load() * 0.9f + instant * 0.1f, std::memory_order_relaxed);
    }
}

//==============================================================================
// Pipeline data model
//==============================================================================
bool AxiomAudioProcessor::isPipelineBusy() const
{
    return analysisJob.isThreadRunning() || separationJob.isThreadRunning();
}

int AxiomAudioProcessor::getActiveStemIndex() const
{
    const juce::ScopedLock sl (dataLock);
    return activeStem;
}

juce::StringArray AxiomAudioProcessor::getStemNames() const
{
    const juce::ScopedLock sl (dataLock);
    juce::StringArray names;
    for (auto& s : stems)
        names.add (s.name);
    return names;
}

AudioZone AxiomAudioProcessor::getZone() const
{
    const juce::ScopedLock sl (dataLock);
    return zone;
}

bool AxiomAudioProcessor::getActiveAudio (juce::AudioBuffer<float>& out, double& sr,
                                          juce::String& name) const
{
    const juce::ScopedLock sl (dataLock);
    if (sourceAudio.getNumSamples() == 0 || sourceSampleRate <= 0.0)
        return false;

    if (activeStem >= 0 && activeStem < (int) stems.size())
    {
        out  = stems[(size_t) activeStem].audio;
        name = stems[(size_t) activeStem].name;
    }
    else
    {
        out  = sourceAudio;
        name = sampleFile.existsAsFile() ? sampleFile.getFileName() : "Full mix";
    }
    sr = sourceSampleRate;
    return true;
}

juce::String AxiomAudioProcessor::getSeparationBackendName() const
{
    return StemSeparationEngine::backendName (*inference);
}

//==============================================================================
// Pipeline entry points (message thread)
//==============================================================================
void AxiomAudioProcessor::importSampleFile (const juce::File& file)
{
    if (! file.existsAsFile())
        return;

    analysisJob.stopThread (4000);           // cancels via shouldAbort polling
    separationJob.stopThread (4000);

    {
        const juce::ScopedLock sl (dataLock);
        pendingFile = file;
        pendingLoad = true;
    }
    sampleFile = file;

    stage.store (PipelineStage::Loading);
    pipelineProgress.store (0.05f);
    sendChangeMessage();

    analysisJob.startThread();
}

void AxiomAudioProcessor::requestStemSeparation()
{
    if (isPipelineBusy())
        return;
    {
        const juce::ScopedLock sl (dataLock);
        if (sourceAudio.getNumSamples() == 0)
            return;
    }

    stage.store (PipelineStage::Separating);
    pipelineProgress.store (0.0f);
    sendChangeMessage();

    separationJob.startThread();
}

void AxiomAudioProcessor::setActiveStem (int index)
{
    if (isPipelineBusy())
        return;
    {
        const juce::ScopedLock sl (dataLock);
        const int clamped = juce::jlimit (-1, (int) stems.size() - 1, index);
        if (clamped == activeStem)
            return;
        activeStem = clamped;
        zone = {};                            // new audio, fresh zone
        pendingLoad = false;
    }
    stage.store (PipelineStage::Analyzing);
    pipelineProgress.store (0.1f);
    sendChangeMessage();
    analysisJob.startThread();
}

void AxiomAudioProcessor::setZoneAndReanalyze (const AudioZone& newZone)
{
    if (isPipelineBusy())
        return;
    {
        const juce::ScopedLock sl (dataLock);
        if (sourceAudio.getNumSamples() == 0)
            return;
        zone = newZone;
        pendingLoad = false;
    }
    stage.store (PipelineStage::Analyzing);
    pipelineProgress.store (0.1f);
    sendChangeMessage();
    analysisJob.startThread();
}

//==============================================================================
// Worker threads
//==============================================================================
void AxiomAudioProcessor::postFailure (const juce::String& why)
{
    DBG ("Axiom pipeline failed: " << why);
    stage.store (PipelineStage::Failed);
    pipelineProgress.store (-1.0f);
    juce::WeakReference<AxiomAudioProcessor> weak (this);
    juce::MessageManager::callAsync ([weak] { if (weak != nullptr) weak->sendChangeMessage(); });
}

void AxiomAudioProcessor::runPipeline()
{
    // --- Decode (only when a new file was imported) -----------------------------
    juce::File fileToLoad;
    {
        const juce::ScopedLock sl (dataLock);
        if (pendingLoad)
            fileToLoad = pendingFile;
    }

    if (fileToLoad.existsAsFile())
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (fileToLoad));
        if (reader == nullptr)
            return postFailure ("unsupported format: " + fileToLoad.getFileName());

        // 30 s is ample for instrument learning; bounds memory and analysis time.
        const auto numSamples = (int) juce::jmin<juce::int64> (reader->lengthInSamples,
                                                               (juce::int64) (30.0 * reader->sampleRate));
        if (numSamples < 1024)
            return postFailure ("file too short");

        juce::AudioBuffer<float> decoded ((int) reader->numChannels, numSamples);
        if (! reader->read (&decoded, 0, numSamples, 0, true, true))
            return postFailure ("decode error");

        const juce::ScopedLock sl (dataLock);
        sourceAudio      = std::move (decoded);
        sourceSampleRate = reader->sampleRate;
        stems.clear();
        activeStem  = -1;
        zone        = {};
        pendingLoad = false;
    }

    if (analysisJob.threadShouldExit()) return;

    // --- Snapshot the active audio with zone crop + fades -----------------------
    juce::AudioBuffer<float> active;
    double sr = 0.0;
    {
        const juce::ScopedLock sl (dataLock);
        if (sourceAudio.getNumSamples() == 0)
            return postFailure ("nothing to analyze");

        const auto& src = (activeStem >= 0 && activeStem < (int) stems.size())
                              ? stems[(size_t) activeStem].audio
                              : sourceAudio;
        sr     = sourceSampleRate;
        active = AudioZone::extract (src, sr, zone);
    }

    if (active.getNumSamples() < 4096)
        return postFailure ("zone too short — widen the markers");

    // --- Analyze ------------------------------------------------------------------
    stage.store (PipelineStage::Analyzing);
    pipelineProgress.store (0.25f);

    auto features = AudioAnalyzer::analyze (active, sr,
                                            [this] { return analysisJob.threadShouldExit(); });
    if (analysisJob.threadShouldExit()) return;

    if (! features.isValid())
        return postFailure ("no stable pitch found — try an isolated, single-note slice");

    // --- Reconstruct ------------------------------------------------------------
    stage.store (PipelineStage::Reconstructing);
    pipelineProgress.store (0.75f);

    auto patch = reconstructor->reconstruct (features);
    const auto tier = reconstructor->getTierName();

    if (analysisJob.threadShouldExit()) return;

    // --- DDSP control frames (resynthesis layer) ---------------------------------
    // Same inference engine instance as separation/refinement — the
    // ddsp_decoder session simply lives alongside the other model slots.
    pipelineProgress.store (0.9f);
    auto ddsp = std::make_shared<DdspTimbre> (
        DdspEncoder::extract (active, sr, features, *inference,
                              [this] { return analysisJob.threadShouldExit(); }));

    if (analysisJob.threadShouldExit()) return;

    // --- SK-1 pitch-stretch sample -------------------------------------------
    // The zone audio itself (mono, length-capped) — the varispeed layer
    // plays this verbatim, so what you hear at the root key IS the source.
    auto sampler = std::make_shared<SamplerSource>();
    {
        const int len = juce::jmin (active.getNumSamples(),
                                    (int) (SamplerSource::maxSeconds * sr));
        sampler->audio.assign ((size_t) len, 0.0f);
        const float chGain = 1.0f / (float) juce::jmax (1, active.getNumChannels());
        for (int ch = 0; ch < active.getNumChannels(); ++ch)
        {
            const float* src = active.getReadPointer (ch);
            for (int i = 0; i < len; ++i)
                sampler->audio[(size_t) i] += src[i] * chGain;
        }
        sampler->sampleRate = sr;
        sampler->rootHz     = features.f0Hz > 0.0f ? features.f0Hz
                                                   : dsp::midiToHz ((float) features.rootMidiNote);
        sampler->oneShot    = ! features.isSustained;

        // Sustain loop: reuse the DDSP loop region when one was measured,
        // otherwise loop the back half of the sample.
        int lo = (int) ((double) len * 0.45);
        int hi = (int) ((double) len * 0.95);
        if (ddsp->isValid() && ! ddsp->oneShot && ddsp->frameRate > 0.0f)
        {
            const int dLo = (int) ((double) ddsp->loopStartFrame / ddsp->frameRate * sr);
            const int dHi = (int) ((double) ddsp->loopEndFrame   / ddsp->frameRate * sr);
            if (dHi - dLo >= 256) { lo = dLo; hi = dHi; }
        }
        sampler->loopStart = juce::jlimit (0, juce::jmax (0, len - 2), lo);
        sampler->loopEnd   = juce::jlimit (sampler->loopStart + 1, len, hi);
    }

    juce::WeakReference<AxiomAudioProcessor> weak (this);
    juce::MessageManager::callAsync ([weak, patch, features, tier, ddsp, sampler]
    {
        if (weak != nullptr)
            weak->applyReconstruction (patch, features, tier,
                                       std::move (*ddsp), std::move (*sampler));
    });
}

void AxiomAudioProcessor::runSeparation()
{
    juce::AudioBuffer<float> src;
    double sr = 0.0;
    {
        const juce::ScopedLock sl (dataLock);
        if (sourceAudio.getNumSamples() == 0)
            return postFailure ("import a file first");
        src = sourceAudio;
        sr  = sourceSampleRate;
    }

    auto result = StemSeparationEngine::separate (src, sr, *inference, pipelineProgress,
                                                  [this] { return separationJob.threadShouldExit(); });
    if (separationJob.threadShouldExit()) return;

    if (result.empty())
        return postFailure ("stem separation failed");

    juce::WeakReference<AxiomAudioProcessor> weak (this);
    // std::function requires copyable captures — share the stem vector.
    auto shared = std::make_shared<std::vector<Stem>> (std::move (result));
    juce::MessageManager::callAsync ([weak, shared]
    {
        if (weak != nullptr)
            weak->applyStems (std::move (*shared));
    });
}

//==============================================================================
// Message-thread appliers
//==============================================================================
void AxiomAudioProcessor::applyReconstruction (const InstrumentPatch& patch,
                                               const AnalysisFeatures& features,
                                               const juce::String& tier,
                                               DdspTimbre ddspTimbre,
                                               SamplerSource sampler)
{
    lastFeatures = features;
    lastTier     = tier;
    lastDdspTier = ddspTimbre.isValid() ? ddspTimbre.tierName : juce::String();
    presetManager.clearCurrent();             // freshly learned sound is unsaved

    engine.setPatch (patch, std::move (ddspTimbre), std::move (sampler));
    applyPatchToParameters (patch);

    stage.store (PipelineStage::Ready);
    pipelineProgress.store (-1.0f);
    sendChangeMessage();
}

void AxiomAudioProcessor::applyStems (std::vector<Stem>&& newStems)
{
    {
        const juce::ScopedLock sl (dataLock);
        stems = std::move (newStems);
    }
    stage.store (PipelineStage::Ready);
    pipelineProgress.store (-1.0f);
    sendChangeMessage();
}

void AxiomAudioProcessor::applyPatchToParameters (const InstrumentPatch& patch)
{
    using namespace params;
    auto set = [this] (const char* id, float value)
    {
        if (auto* param = apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    };

    set (cutoff,       patch.filter.cutoffHz);
    set (resonance,    patch.filter.resonance);
    set (drive,        patch.filter.drive);
    set (filterType,   (float) (int) patch.filter.type);
    set (filterEnvAmt, patch.filter.envAmountSt);

    set (ampAttack,  patch.ampEnv.attack);
    set (ampDecay,   patch.ampEnv.decay);
    set (ampSustain, patch.ampEnv.sustain);
    set (ampRelease, patch.ampEnv.release);

    set (fltAttack,  patch.filterEnv.attack);
    set (fltDecay,   patch.filterEnv.decay);
    set (fltSustain, patch.filterEnv.sustain);
    set (fltRelease, patch.filterEnv.release);

    set (lfoRate,   patch.lfo.rateHz);
    set (lfoPitch,  patch.lfo.pitchDepthCents);
    set (lfoCutoff, patch.lfo.cutoffDepthSt);

    float maxDetune = 0.0f;
    for (auto& o : patch.oscs)
        if (o.enabled) maxDetune = juce::jmax (maxDetune, std::abs (o.detuneCents));
    set (detune,     maxDetune);
    set (noiseLevel, patch.noiseLevel);
    set (glide,      patch.glideMs);

    set (satDrive,      patch.fx.satDrive);
    set (chorusMix,     patch.fx.chorusMix);
    set (delayMix,      patch.fx.delayMix);
    set (delayTime,     patch.fx.delayTimeMs);
    set (delayFeedback, patch.fx.delayFeedback);
    set (reverbMix,     patch.fx.reverbMix);
    set (reverbSize,    patch.fx.reverbSize);
    set (width,         patch.fx.stereoWidth);

    set (masterGain, patch.fx.gainDb);
}

//==============================================================================
// Presets
//==============================================================================
axiom::InstrumentPatch AxiomAudioProcessor::getCurrentPatchWithOverrides() const
{
    InstrumentPatch p = engine.getPatch();

    using namespace params;
    auto val = [this] (const char* id)
    {
        auto* raw = apvts.getRawParameterValue (id);
        return raw != nullptr ? raw->load() : 0.0f;
    };

    p.filter.type        = (FilterType) (int) val (filterType);
    p.filter.cutoffHz    = val (cutoff);
    p.filter.resonance   = val (resonance);
    p.filter.drive       = val (drive);
    p.filter.envAmountSt = val (filterEnvAmt);

    p.ampEnv    = { val (ampAttack), val (ampDecay), val (ampSustain), val (ampRelease) };
    p.filterEnv = { val (fltAttack), val (fltDecay), val (fltSustain), val (fltRelease) };

    p.lfo.rateHz          = val (lfoRate);
    p.lfo.pitchDepthCents = val (lfoPitch);
    p.lfo.cutoffDepthSt   = val (lfoCutoff);

    // Same proportional unison scaling the engine applies live.
    {
        const float target = val (detune);
        float maxAbs = 0.0f;
        for (auto& slot : p.oscs)
            if (slot.enabled) maxAbs = juce::jmax (maxAbs, std::abs (slot.detuneCents));

        if (maxAbs > 0.01f)
        {
            const float scale = target / maxAbs;
            for (auto& slot : p.oscs)
                if (slot.enabled) slot.detuneCents *= scale;
        }
        else if (target > 0.01f && p.oscs[0].enabled)
        {
            p.oscs[0].detuneCents = target;
        }
    }

    p.noiseLevel = val (noiseLevel);
    p.glideMs    = val (glide);

    p.fx.satDrive      = val (satDrive);
    p.fx.chorusMix     = val (chorusMix);
    p.fx.delayMix      = val (delayMix);
    p.fx.delayTimeMs   = val (delayTime);
    p.fx.delayFeedback = val (delayFeedback);
    p.fx.reverbMix     = val (reverbMix);
    p.fx.reverbSize    = val (reverbSize);
    p.fx.stereoWidth   = val (width);
    p.fx.gainDb        = val (masterGain);

    return p;
}

void AxiomAudioProcessor::loadPresetAtIndex (int index)
{
    if (auto preset = presetManager.loadPreset (index))
    {
        // The preset defines the whole sound: its DDSP + SK-1 layers replace
        // the current ones (or clear them when the preset never carried any).
        lastDdspTier = preset->ddsp.isValid() ? preset->ddsp.tierName : juce::String();
        const auto patch = preset->patch;
        engine.setPatch (patch, std::move (preset->ddsp), std::move (preset->sampler));
        applyPatchToParameters (patch);
        // Snapshot through the same param round-trip used by isPresetDirty(),
        // so float quantization can't produce a false "edited" state.
        presetSnapshotJson = patch_io::toJson (getCurrentPatchWithOverrides());
        lastTier = "Preset";
        stage.store (PipelineStage::Ready);
        pipelineProgress.store (-1.0f);
        sendChangeMessage();
    }
}

void AxiomAudioProcessor::navigatePreset (int delta)
{
    const int count = (int) presetManager.getPresets().size();
    if (count == 0)
        return;

    const int cur  = presetManager.getCurrentIndex();
    const int next = cur < 0 ? (delta > 0 ? 0 : count - 1)
                             : ((cur + delta) % count + count) % count;
    loadPresetAtIndex (next);
}

juce::Result AxiomAudioProcessor::savePresetAs (const juce::String& name)
{
    // Save the sound as heard: knob edits are part of the preset, and so are
    // the DDSP resynthesis and SK-1 sample layers.
    const auto effective = getCurrentPatchWithOverrides();

    auto result = presetManager.savePreset (name, effective, &engine.getDdspTimbre(),
                                            &engine.getSamplerSource());
    if (result.wasOk())
    {
        engine.setPatch (effective);          // base patch now matches the file
        presetSnapshotJson = patch_io::toJson (getCurrentPatchWithOverrides());
        sendChangeMessage();
    }
    return result;
}

bool AxiomAudioProcessor::isPresetDirty() const
{
    if (presetManager.getCurrentIndex() < 0 || presetSnapshotJson.isEmpty())
        return false;
    return patch_io::toJson (getCurrentPatchWithOverrides()) != presetSnapshotJson;
}

void AxiomAudioProcessor::updateCurrentPreset()
{
    const auto name = presetManager.getCurrentName();
    if (name.isEmpty())
        return;

    const auto effective = getCurrentPatchWithOverrides();
    if (presetManager.savePreset (name, effective, &engine.getDdspTimbre(),
                                  &engine.getSamplerSource()).wasOk())
    {
        engine.setPatch (effective);
        presetSnapshotJson = patch_io::toJson (getCurrentPatchWithOverrides());
        sendChangeMessage();
    }
}

void AxiomAudioProcessor::revertPresetChanges()
{
    const int index = presetManager.getCurrentIndex();
    if (index >= 0)
        loadPresetAtIndex (index);
}

void AxiomAudioProcessor::togglePresetFavorite()
{
    const int cur = presetManager.getCurrentIndex();
    if (cur < 0)
        return;
    presetManager.setFavorite (cur, ! presetManager.getPresets()[(size_t) cur].favorite);
    sendChangeMessage();
}

void AxiomAudioProcessor::deleteCurrentPreset()
{
    presetManager.deletePreset (presetManager.getCurrentIndex());
    sendChangeMessage();
}

juce::String AxiomAudioProcessor::getPresetDisplayName() const
{
    if (auto name = presetManager.getCurrentName(); name.isNotEmpty())
        return name;
    if (sampleFile.existsAsFile())
        return sampleFile.getFileNameWithoutExtension() + " (unsaved)";
    return "Init";
}

//==============================================================================
void AxiomAudioProcessor::startExport (const juce::File& destinationDir,
                                       std::function<void (juce::Result)> onFinished)
{
    bool expected = false;
    if (! exporting.compare_exchange_strong (expected, true))
    {
        if (onFinished)
            onFinished (juce::Result::fail ("An export is already running."));
        return;
    }

    const auto patch = engine.getPatch();
    auto name = sampleFile.existsAsFile() ? sampleFile.getFileNameWithoutExtension()
                                          : juce::String ("Axiom Instrument");
    {
        const juce::ScopedLock sl (dataLock);
        if (activeStem >= 0 && activeStem < (int) stems.size())
            name << " - " << stems[(size_t) activeStem].name;
    }

    // Progress lives in a shared atomic so the worker never touches the
    // processor: safe even if the plugin is destroyed mid-export.
    auto progress = std::make_shared<std::atomic<float>> (0.0f);
    exportProgressShared = progress;

    juce::WeakReference<AxiomAudioProcessor> weak (this);
    juce::Thread::launch ([weak, patch, destinationDir, name, onFinished, progress]
    {
        const auto result = InstrumentExporter::exportInstrument (patch, destinationDir, name,
                                                                  InstrumentExporter::Options{},
                                                                  *progress);
        juce::MessageManager::callAsync ([weak, result, onFinished]
        {
            if (weak != nullptr)
                weak->exporting.store (false);
            if (onFinished)
                onFinished (result);
        });
    });
}

//==============================================================================
void AxiomAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree root ("AxiomSession");
    root.setProperty ("patchJson",  patch_io::toJson (engine.getPatch()), nullptr);
    root.setProperty ("samplePath", sampleFile.getFullPathName(), nullptr);
    root.setProperty ("tier",       lastTier, nullptr);
    root.setProperty ("presetName", presetManager.getCurrentName(), nullptr);

    // DDSP frames are derived from the source audio, which is NOT re-analyzed
    // on restore — persist them so a reopened project keeps the resynthesis
    // layer (binary blob, ~0.5 MB worst case; hosts handle MB-scale state).
    if (const auto& ddsp = engine.getDdspTimbre(); ddsp.isValid())
        root.setProperty ("ddspTimbre", juce::var (ddsp.toMemoryBlock()), nullptr);

    // Same story for the SK-1 varispeed sample (16-bit PCM blob).
    if (const auto& sk = engine.getSamplerSource(); sk.isValid())
        root.setProperty ("samplerSource", juce::var (sk.toMemoryBlock()), nullptr);
    {
        const juce::ScopedLock sl (dataLock);
        root.setProperty ("zoneStart",   zone.startSec, nullptr);
        root.setProperty ("zoneEnd",     zone.endSec, nullptr);
        root.setProperty ("zoneFadeIn",  zone.fadeInSec, nullptr);
        root.setProperty ("zoneFadeOut", zone.fadeOutSec, nullptr);
    }
    root.appendChild (apvts.copyState(), nullptr);

    juce::MemoryOutputStream stream (destData, false);
    root.writeToStream (stream);
}

void AxiomAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto root = juce::ValueTree::readFromData (data, (size_t) sizeInBytes);
    if (! root.isValid() || ! root.hasType ("AxiomSession"))
        return;

    if (auto params = root.getChildWithName (apvts.state.getType()); params.isValid())
        apvts.replaceState (params);

    DdspTimbre ddsp;
    if (auto* block = root.getProperty ("ddspTimbre").getBinaryData())
        if (auto parsed = DdspTimbre::fromMemoryBlock (block->getData(), block->getSize()))
            ddsp = std::move (*parsed);
    lastDdspTier = ddsp.isValid() ? ddsp.tierName : juce::String();

    SamplerSource sk;
    if (auto* block = root.getProperty ("samplerSource").getBinaryData())
        if (auto parsed = SamplerSource::fromMemoryBlock (block->getData(), block->getSize()))
            sk = std::move (*parsed);

    if (auto patch = patch_io::fromJson (root.getProperty ("patchJson").toString()))
    {
        engine.setPatch (*patch, std::move (ddsp), std::move (sk));
        stage.store (PipelineStage::Ready);
    }

    sampleFile = juce::File (root.getProperty ("samplePath").toString());
    lastTier   = root.getProperty ("tier").toString();
    presetManager.setCurrentByName (root.getProperty ("presetName").toString());
    if (presetManager.getCurrentIndex() >= 0)
        presetSnapshotJson = patch_io::toJson (getCurrentPatchWithOverrides());
    {
        const juce::ScopedLock sl (dataLock);
        zone.startSec   = (double) root.getProperty ("zoneStart",   0.0);
        zone.endSec     = (double) root.getProperty ("zoneEnd",     0.0);
        zone.fadeInSec  = (double) root.getProperty ("zoneFadeIn",  0.01);
        zone.fadeOutSec = (double) root.getProperty ("zoneFadeOut", 0.02);
    }
    sendChangeMessage();
}

//==============================================================================
juce::AudioProcessorEditor* AxiomAudioProcessor::createEditor()
{
    return new AxiomAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AxiomAudioProcessor();
}
