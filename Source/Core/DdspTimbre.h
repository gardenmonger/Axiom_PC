/*
  ==============================================================================

    DdspTimbre.h

    The DDSP resynthesis data model: a time series of harmonic-plus-noise
    control frames extracted from the imported sample (Differentiable DSP,
    Engel et al. 2020 — the *synthesizer* half runs here in real time; the
    control frames come from the analysis thread, either measured directly
    from the source STFT or predicted by the optional "ddsp_decoder" ONNX
    model).

    Each frame holds a 64-partial harmonic amplitude distribution plus a
    filtered-noise gain/cutoff pair. At play time DdspResynth re-renders the
    frames additively at ANY fundamental — the source timbre stretches to
    every key played, polyphonically, alias-free — while the recipe engine
    can keep playing alongside it (EngineMode::Both).

    Unlike InstrumentPatch this block is heap-backed (up to ~0.5 MB), so it
    travels next to the patch through SynthEngine's two-slot handoff instead
    of inside it, and serializes to a compact binary blob (session state) or
    base64 string (.axiom presets).

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <optional>
#include <vector>

namespace axiom
{

/** Which synthesis path the voices render. Values are the engineMode
    parameter's choice indices — order is frozen (host automation). */
enum class EngineMode : int
{
    Recipe = 0,   // classic reconstructed synth recipe (oscillators)
    Ddsp   = 1,   // DDSP harmonic+noise resynthesis of the source
    Both   = 2    // both layers, summed per voice
};

//==============================================================================
struct DdspTimbre
{
    static constexpr int maxHarmonics = 64;
    static constexpr int maxFrames    = 2048;

    int   numFrames    = 0;
    int   numHarmonics = 0;
    float frameRate    = 187.5f;    // control frames per second
    float sourceRootHz = 261.63f;   // f0 the frames were measured at

    // Held notes ping-pong between these frames once the playhead arrives;
    // one-shots ignore them and hold the final frame instead.
    int   loopStartFrame = 0;
    int   loopEndFrame   = 0;
    bool  oneShot        = false;

    /** "DDSP neural (ONNX)" or "DDSP measured (STFT)" — shown in the UI. */
    juce::String tierName;

    // Frame-major data. `harm` is numFrames rows of maxHarmonics linear
    // amplitudes (unused tail of each row zeroed); the per-frame vectors
    // hold the noise layer and the loudness curve fed to the ONNX decoder.
    std::vector<float> harm;
    std::vector<float> noiseGain;
    std::vector<float> noiseCutoffHz;
    std::vector<float> loudness;

    bool isValid() const noexcept
    {
        return numFrames > 1 && numHarmonics > 0
            && (int) harm.size()          >= numFrames * maxHarmonics
            && (int) noiseGain.size()     >= numFrames
            && (int) noiseCutoffHz.size() >= numFrames;
    }

    /** Audio-thread safe read of one frame's harmonic row (no bounds check —
        caller clamps the index). */
    const float* frame (int index) const noexcept
    {
        return harm.data() + (size_t) index * (size_t) maxHarmonics;
    }

    void clear()
    {
        *this = DdspTimbre();
    }

    //==========================================================================
    // Serialization: versioned binary blob. Raw float blocks are written
    // little-endian native — every platform Axiom ships on is LE.
    //==========================================================================
    static constexpr int32_t serialMagic   = 0x41584454;   // "AXDT"
    static constexpr int32_t serialVersion = 1;

    juce::MemoryBlock toMemoryBlock() const
    {
        juce::MemoryOutputStream out;
        out.writeInt (serialMagic);
        out.writeInt (serialVersion);
        out.writeInt (numFrames);
        out.writeInt (numHarmonics);
        out.writeFloat (frameRate);
        out.writeFloat (sourceRootHz);
        out.writeInt (loopStartFrame);
        out.writeInt (loopEndFrame);
        out.writeBool (oneShot);
        out.writeString (tierName);

        const auto writeVec = [&out] (const std::vector<float>& v, size_t expected)
        {
            jassert (v.size() >= expected);
            out.write (v.data(), expected * sizeof (float));
        };
        writeVec (harm,          (size_t) numFrames * maxHarmonics);
        writeVec (noiseGain,     (size_t) numFrames);
        writeVec (noiseCutoffHz, (size_t) numFrames);
        writeVec (loudness,      (size_t) numFrames);

        return out.getMemoryBlock();
    }

    static std::optional<DdspTimbre> fromMemoryBlock (const void* data, size_t size)
    {
        juce::MemoryInputStream in (data, size, false);

        if (in.readInt() != serialMagic || in.readInt() != serialVersion)
            return std::nullopt;

        DdspTimbre t;
        t.numFrames      = in.readInt();
        t.numHarmonics   = in.readInt();
        t.frameRate      = in.readFloat();
        t.sourceRootHz   = in.readFloat();
        t.loopStartFrame = in.readInt();
        t.loopEndFrame   = in.readInt();
        t.oneShot        = in.readBool();
        t.tierName       = in.readString();

        if (t.numFrames < 2 || t.numFrames > maxFrames
            || t.numHarmonics < 1 || t.numHarmonics > maxHarmonics
            || t.frameRate <= 0.0f)
            return std::nullopt;

        const auto readVec = [&in] (std::vector<float>& v, size_t count)
        {
            v.resize (count);
            return in.read (v.data(), (int) (count * sizeof (float)))
                       == (int) (count * sizeof (float));
        };
        if (! readVec (t.harm,          (size_t) t.numFrames * maxHarmonics)
            || ! readVec (t.noiseGain,     (size_t) t.numFrames)
            || ! readVec (t.noiseCutoffHz, (size_t) t.numFrames)
            || ! readVec (t.loudness,      (size_t) t.numFrames))
            return std::nullopt;

        t.loopStartFrame = juce::jlimit (0, t.numFrames - 2, t.loopStartFrame);
        t.loopEndFrame   = juce::jlimit (t.loopStartFrame + 1, t.numFrames - 1, t.loopEndFrame);
        return t;
    }

    juce::String toBase64() const
    {
        const auto block = toMemoryBlock();
        return juce::Base64::toBase64 (block.getData(), block.getSize());
    }

    static std::optional<DdspTimbre> fromBase64 (const juce::String& encoded)
    {
        if (encoded.isEmpty())
            return std::nullopt;

        juce::MemoryOutputStream decoded;
        if (! juce::Base64::convertFromBase64 (decoded, encoded))
            return std::nullopt;
        return fromMemoryBlock (decoded.getData(), decoded.getDataSize());
    }
};

} // namespace axiom
