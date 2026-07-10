/*
  ==============================================================================

    SamplerSource.h

    The SK-1 pitch-stretch data model: the imported zone audio itself,
    downmixed to mono and capped in length, kept for raw varispeed playback
    (Synth/SkSampler). Unlike the recipe and DDSP layers this path plays the
    *actual sample* — every key just changes the read speed, so high notes
    play faster and shorter, exactly like a Casio SK-1 lo-fi sampler.

    Like DdspTimbre this block is heap-backed, so it travels next to the
    patch through SynthEngine's two-slot handoff instead of inside it, and
    serializes to a compact binary blob (16-bit PCM — session state) or
    base64 string (.axiom presets).

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <optional>
#include <vector>

namespace axiom
{

struct SamplerSource
{
    /** Length cap (seconds) — bounds preset/session size; ample for an
        instrument slice (the SK-1 itself held 1.4 s). */
    static constexpr double maxSeconds = 10.0;

    double sampleRate = 0.0;      // capture rate of `audio`
    float  rootHz     = 261.63f;  // f0 the sample sounds at when unshifted

    // Held notes loop this region (forward, crossfaded by the player);
    // one-shots play straight through and stop.
    int    loopStart = 0;         // samples
    int    loopEnd   = 0;
    bool   oneShot   = false;

    std::vector<float> audio;     // mono

    bool isValid() const noexcept
    {
        return (int) audio.size() > 64 && sampleRate > 0.0 && rootHz > 0.0f;
    }

    void clear()
    {
        *this = SamplerSource();
    }

    //==========================================================================
    // Serialization: versioned binary blob, samples stored as 16-bit PCM
    // (half the size of float; the layer is lo-fi by design).
    //==========================================================================
    static constexpr int32_t serialMagic   = 0x4158534B;   // "AXSK"
    static constexpr int32_t serialVersion = 1;

    juce::MemoryBlock toMemoryBlock() const
    {
        juce::MemoryOutputStream out;
        out.writeInt (serialMagic);
        out.writeInt (serialVersion);
        out.writeDouble (sampleRate);
        out.writeFloat (rootHz);
        out.writeInt (loopStart);
        out.writeInt (loopEnd);
        out.writeBool (oneShot);
        out.writeInt ((int) audio.size());

        for (float s : audio)
            out.writeShort ((short) juce::roundToInt (
                                juce::jlimit (-1.0f, 1.0f, s) * 32767.0f));

        return out.getMemoryBlock();
    }

    static std::optional<SamplerSource> fromMemoryBlock (const void* data, size_t size)
    {
        juce::MemoryInputStream in (data, size, false);

        if (in.readInt() != serialMagic || in.readInt() != serialVersion)
            return std::nullopt;

        SamplerSource s;
        s.sampleRate = in.readDouble();
        s.rootHz     = in.readFloat();
        s.loopStart  = in.readInt();
        s.loopEnd    = in.readInt();
        s.oneShot    = in.readBool();

        const int numSamples = in.readInt();
        if (s.sampleRate <= 0.0 || s.rootHz <= 0.0f || numSamples < 0
            || numSamples > (int) (maxSeconds * 192000.0)
            || in.getNumBytesRemaining() < (juce::int64) numSamples * 2)
            return std::nullopt;

        s.audio.resize ((size_t) numSamples);
        for (auto& v : s.audio)
            v = (float) in.readShort() / 32767.0f;

        s.loopStart = juce::jlimit (0, juce::jmax (0, numSamples - 2), s.loopStart);
        s.loopEnd   = juce::jlimit (s.loopStart + 1, numSamples, s.loopEnd);
        return s;
    }

    juce::String toBase64() const
    {
        const auto block = toMemoryBlock();
        return juce::Base64::toBase64 (block.getData(), block.getSize());
    }

    static std::optional<SamplerSource> fromBase64 (const juce::String& encoded)
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
