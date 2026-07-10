#include "SynthEngine.h"

namespace axiom
{

//==============================================================================
void SynthEngine::prepare (double sampleRate, int maxBlockSize)
{
    sr = sampleRate;

    // Build the modern-engine sinc table here (idempotent) so the first
    // HQ-mode voice never constructs it under the render callback.
    dsp::SkSampler::ensureTables();

    for (auto& v : voices)
        v.prepare (sampleRate);
    sustainHeld.fill (false);

    fx.prepare (sampleRate, maxBlockSize);
    mixBuffer.setSize (2, maxBlockSize, false, true, true);

    bendSemis = 0.0f;
    sustainPedal = false;
    lastNoteHz = 0.0f;
}

void SynthEngine::releaseResources()
{
    for (auto& v : voices)
        v.reset();
    activeVoiceCount.store (0, std::memory_order_relaxed);
}

//==============================================================================
int SynthEngine::prepareInactiveSlot (const InstrumentPatch& newPatch)
{
    const int inactive = 1 - activePatchSlot.load (std::memory_order_acquire);
    patchSlots[(size_t) inactive] = newPatch;

    bool wantsTables = false;
    for (auto& o : newPatch.oscs)
        wantsTables = wantsTables || (o.enabled && o.type == OscType::Harmonic);

    if (wantsTables && newPatch.numHarmonics > 0)
        tableSlots[(size_t) inactive].build (newPatch.harmonicGains.data(),
                                             newPatch.numHarmonics);
    else
        tableSlots[(size_t) inactive].clear();

    return inactive;
}

void SynthEngine::setPatch (const InstrumentPatch& newPatch)
{
    const int inactive = prepareInactiveSlot (newPatch);

    // Keep the resynthesis + SK-1 layers alive across knob-driven patch
    // rewrites (message-thread copy; the audio thread only ever reads the
    // active slot).
    ddspSlots[(size_t) inactive]    = ddspSlots[(size_t) (1 - inactive)];
    samplerSlots[(size_t) inactive] = samplerSlots[(size_t) (1 - inactive)];

    activePatchSlot.store (inactive, std::memory_order_release);
}

void SynthEngine::setPatch (const InstrumentPatch& newPatch, DdspTimbre newTimbre,
                            SamplerSource newSample)
{
    const int inactive = prepareInactiveSlot (newPatch);
    ddspSlots[(size_t) inactive]    = std::move (newTimbre);
    samplerSlots[(size_t) inactive] = std::move (newSample);
    activePatchSlot.store (inactive, std::memory_order_release);
}

InstrumentPatch SynthEngine::getPatch() const noexcept
{
    return patchSlots[(size_t) activePatchSlot.load (std::memory_order_acquire)];
}

const DdspTimbre& SynthEngine::getDdspTimbre() const noexcept
{
    return ddspSlots[(size_t) activePatchSlot.load (std::memory_order_acquire)];
}

const SamplerSource& SynthEngine::getSamplerSource() const noexcept
{
    return samplerSlots[(size_t) activePatchSlot.load (std::memory_order_acquire)];
}

//==============================================================================
InstrumentPatch SynthEngine::buildEffectivePatch (const InstrumentPatch& base) const noexcept
{
    InstrumentPatch p = base;

    const auto* o = overrides;
    if (o == nullptr)
        return p;

    auto get = [] (std::atomic<float>* a, float fallback)
               { return a != nullptr ? a->load (std::memory_order_relaxed) : fallback; };

    p.filter.type        = (FilterType) (int) get (o->filterType, (float) (int) p.filter.type);
    p.filter.cutoffHz    = get (o->cutoff,       p.filter.cutoffHz);
    p.filter.resonance   = get (o->resonance,    p.filter.resonance);
    p.filter.drive       = get (o->drive,        p.filter.drive);
    p.filter.envAmountSt = get (o->filterEnvAmt, p.filter.envAmountSt);

    p.ampEnv = { get (o->ampAttack,  p.ampEnv.attack),
                 get (o->ampDecay,   p.ampEnv.decay),
                 get (o->ampSustain, p.ampEnv.sustain),
                 get (o->ampRelease, p.ampEnv.release) };

    p.filterEnv = { get (o->fltAttack,  p.filterEnv.attack),
                    get (o->fltDecay,   p.filterEnv.decay),
                    get (o->fltSustain, p.filterEnv.sustain),
                    get (o->fltRelease, p.filterEnv.release) };

    p.lfo.rateHz          = get (o->lfoRate,   p.lfo.rateHz);
    p.lfo.pitchDepthCents = get (o->lfoPitch,  p.lfo.pitchDepthCents);
    p.lfo.cutoffDepthSt   = get (o->lfoCutoff, p.lfo.cutoffDepthSt);

    // Mod wheel deepens vibrato performance-style (up to +50 cents).
    p.lfo.pitchDepthCents += modWheel * 50.0f;

    // Detune override scales the patch's unison proportionally so a
    // two-osc detune pair keeps its +/- symmetry.
    if (o->detune != nullptr)
    {
        const float target = o->detune->load (std::memory_order_relaxed);
        float maxAbs = 0.0f;
        for (auto& slot : p.oscs)
            if (slot.enabled) maxAbs = std::max (maxAbs, std::abs (slot.detuneCents));

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

    p.noiseLevel = get (o->noiseLevel, p.noiseLevel);
    p.glideMs    = get (o->glide,      p.glideMs);

    p.fx.satDrive      = get (o->satDrive,      p.fx.satDrive);
    p.fx.chorusMix     = get (o->chorusMix,     p.fx.chorusMix);
    p.fx.delayMix      = get (o->delayMix,      p.fx.delayMix);
    p.fx.delayTimeMs   = get (o->delayTime,     p.fx.delayTimeMs);
    p.fx.delayFeedback = get (o->delayFeedback, p.fx.delayFeedback);
    p.fx.reverbMix     = get (o->reverbMix,     p.fx.reverbMix);
    p.fx.reverbSize    = get (o->reverbSize,    p.fx.reverbSize);
    p.fx.stereoWidth   = get (o->width,         p.fx.stereoWidth);
    p.fx.gainDb        = get (o->masterGain,    p.fx.gainDb);

    return p;
}

//==============================================================================
void SynthEngine::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const int numSamples = buffer.getNumSamples();
    if (numSamples == 0)
        return;

    // Defensive: a host handing us more samples than it promised in
    // prepareToPlay would otherwise write past the scratch buffer.
    if (numSamples > mixBuffer.getNumSamples())
        mixBuffer.setSize (2, numSamples, false, true, true);

    // One slot-index load per block: patch, wavetables, timbre and sample
    // stay paired.
    const int slot  = activePatchSlot.load (std::memory_order_acquire);
    effective       = buildEffectivePatch (patchSlots[(size_t) slot]);
    currentTables   = &tableSlots[(size_t) slot];
    currentTimbre   = &ddspSlots[(size_t) slot];
    currentSampler  = &samplerSlots[(size_t) slot];

    // Engine-layer routing. DDSP needs a timbre block and SK-1 needs the
    // source sample; without them the recipe engine stays on regardless of
    // the mode so keys never go silent.
    {
        auto get = [this] (std::atomic<float>* a, float fallback)
        {
            return overrides != nullptr && a != nullptr
                       ? a->load (std::memory_order_relaxed) : fallback;
        };

        const int  mode      = (int) get (overrides != nullptr ? overrides->engineMode : nullptr,
                                          (float) (int) EngineMode::Recipe);
        const bool timbreOk  = currentTimbre->isValid();
        const bool sampleOk  = currentSampler->isValid();

        layerMix.ddspOn    = mode != (int) EngineMode::Recipe && timbreOk;
        layerMix.stretchOn = get (overrides != nullptr ? overrides->stretchOn : nullptr, 0.0f) >= 0.5f
                             && sampleOk;
        layerMix.recipeOn  = (mode != (int) EngineMode::Ddsp || ! timbreOk)
                             && ! (mode == (int) EngineMode::Ddsp && layerMix.stretchOn);

        layerMix.recipeLevel  = get (overrides != nullptr ? overrides->recipeLevel  : nullptr, 1.0f);
        layerMix.ddspLevel    = get (overrides != nullptr ? overrides->ddspLevel    : nullptr, 1.0f);
        layerMix.stretchLevel = get (overrides != nullptr ? overrides->stretchLevel : nullptr, 1.0f);
        layerMix.crush        = get (overrides != nullptr ? overrides->stretchCrush : nullptr, 0.0f);
        layerMix.samplerEngine =
            get (overrides != nullptr ? overrides->samplerEngine : nullptr, 0.0f) >= 0.5f
                ? dsp::SkSampler::Engine::modern
                : dsp::SkSampler::Engine::sk1;
    }

    mixBuffer.clear (0, 0, numSamples);
    mixBuffer.clear (1, 0, numSamples);

    // Sample-accurate MIDI: render voice segments between events.
    int segmentStart = 0;
    for (const auto metadata : midi)
    {
        const int eventPos = juce::jlimit (0, numSamples, metadata.samplePosition);
        if (eventPos > segmentStart)
        {
            renderSegment (segmentStart, eventPos - segmentStart);
            segmentStart = eventPos;
        }
        handleMidiEvent (metadata.getMessage());
    }
    if (segmentStart < numSamples)
        renderSegment (segmentStart, numSamples - segmentStart);

    // Only the samples rendered this block: mixBuffer is sized for the
    // largest block, and running the FX over its stale tail region would
    // recirculate garbage through the delay feedback path.
    fx.process (mixBuffer, effective.fx, numSamples);

    // Copy to the host buffer (mono fold-down if needed).
    if (buffer.getNumChannels() >= 2)
    {
        buffer.copyFrom (0, 0, mixBuffer, 0, 0, numSamples);
        buffer.copyFrom (1, 0, mixBuffer, 1, 0, numSamples);
        for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
            buffer.clear (ch, 0, numSamples);
    }
    else if (buffer.getNumChannels() == 1)
    {
        buffer.copyFrom (0, 0, mixBuffer, 0, 0, numSamples);
        buffer.addFrom  (0, 0, mixBuffer, 1, 0, numSamples);
        buffer.applyGain (0, 0, numSamples, 0.5f);
    }

    int count = 0;
    for (auto& v : voices)
        count += v.isActive() ? 1 : 0;
    activeVoiceCount.store (count, std::memory_order_relaxed);
}

void SynthEngine::renderSegment (int startSample, int numSamples)
{
    float* l = mixBuffer.getWritePointer (0) + startSample;
    float* r = mixBuffer.getWritePointer (1) + startSample;

    for (auto& v : voices)
        if (v.isActive())
            v.render (l, r, numSamples, effective, bendSemis, currentTables,
                      currentTimbre, currentSampler, layerMix);
}

//==============================================================================
void SynthEngine::handleMidiEvent (const juce::MidiMessage& m)
{
    if (m.isNoteOn())
    {
        noteOn (m.getNoteNumber(), m.getFloatVelocity());
    }
    else if (m.isNoteOff())
    {
        noteOff (m.getNoteNumber());
    }
    else if (m.isPitchWheel())
    {
        bendSemis = 2.0f * ((float) m.getPitchWheelValue() - 8192.0f) / 8192.0f;
    }
    else if (m.isController())
    {
        if (m.getControllerNumber() == 1)          // mod wheel
        {
            modWheel = (float) m.getControllerValue() / 127.0f;
        }
        else if (m.getControllerNumber() == 64)    // sustain pedal
        {
            const bool down = m.getControllerValue() >= 64;
            if (! down && sustainPedal)
            {
                for (size_t i = 0; i < voices.size(); ++i)
                {
                    if (sustainHeld[i])
                    {
                        voices[i].stopNote (true);
                        sustainHeld[i] = false;
                    }
                }
            }
            sustainPedal = down;
        }
    }
    else if (m.isAllNotesOff() || m.isAllSoundOff())
    {
        for (auto& v : voices)
            v.stopNote (! m.isAllSoundOff());
        sustainHeld.fill (false);
        sustainPedal = false;
    }
}

void SynthEngine::noteOn (int note, float velocity)
{
    Voice* voice = nullptr;
    for (auto& v : voices)
    {
        if (! v.isActive())
        {
            voice = &v;
            break;
        }
    }

    if (voice != nullptr)
    {
        const auto idx = (size_t) std::distance (voices.data(), voice);
        sustainHeld[idx] = false;
        voice->startNote (note, velocity, effective, lastNoteHz, ++noteCounter);
        voice->setHeld (true);
    }
    else if (auto* stolen = findVoiceToSteal())
    {
        // Restarting a sounding voice in place would snap its phase and
        // filter state audibly; stealNote() fades it out (~2 ms) and starts
        // the new note when the fade lands on silence.
        const auto idx = (size_t) std::distance (voices.data(), stolen);
        sustainHeld[idx] = false;
        stolen->stealNote (note, velocity, lastNoteHz, ++noteCounter);
    }
    else
    {
        return;
    }

    lastNoteHz = dsp::midiToHz ((float) note);
}

void SynthEngine::noteOff (int note)
{
    for (size_t i = 0; i < voices.size(); ++i)
    {
        auto& v = voices[i];
        if (v.isActive() && v.getNote() == note && ! v.isReleasing())
        {
            v.setHeld (false);
            if (sustainPedal)
                sustainHeld[i] = true;
            else
                v.stopNote (true);
        }
    }
}

Voice* SynthEngine::findVoiceToSteal() noexcept
{
    // Prefer the oldest releasing voice; otherwise the oldest playing voice.
    Voice* oldestReleasing = nullptr;
    Voice* oldest          = nullptr;

    for (auto& v : voices)
    {
        if (! v.isActive())
            continue;
        if (oldest == nullptr || v.getStartTime() < oldest->getStartTime())
            oldest = &v;
        if (v.isReleasing()
            && (oldestReleasing == nullptr || v.getStartTime() < oldestReleasing->getStartTime()))
            oldestReleasing = &v;
    }
    return oldestReleasing != nullptr ? oldestReleasing : oldest;
}

//==============================================================================
juce::AudioBuffer<float> SynthEngine::renderNoteOffline (const InstrumentPatch& patch,
                                                         int midiNote, float velocity,
                                                         double holdSeconds, double tailSeconds,
                                                         double sampleRate)
{
    constexpr int block = 512;

    SynthEngine engine;
    engine.prepare (sampleRate, block);
    engine.setPatch (patch);

    const int holdSamples  = (int) (holdSeconds * sampleRate);
    const int totalSamples = (int) ((holdSeconds + tailSeconds) * sampleRate);

    juce::AudioBuffer<float> out (2, totalSamples);
    out.clear();

    juce::AudioBuffer<float> chunk (2, block);
    int pos = 0;
    bool noteOffSent = false;

    while (pos < totalSamples)
    {
        const int thisBlock = juce::jmin (block, totalSamples - pos);
        chunk.setSize (2, thisBlock, true, false, true);

        juce::MidiBuffer midi;
        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, midiNote, velocity), 0);
        if (! noteOffSent && pos + thisBlock > holdSamples)
        {
            midi.addEvent (juce::MidiMessage::noteOff (1, midiNote),
                           juce::jmax (0, holdSamples - pos));
            noteOffSent = true;
        }

        engine.processBlock (chunk, midi);
        for (int ch = 0; ch < 2; ++ch)
            out.copyFrom (ch, pos, chunk, ch, 0, thisBlock);
        pos += thisBlock;
    }
    return out;
}

} // namespace axiom
