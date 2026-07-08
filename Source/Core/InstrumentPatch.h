/*
  ==============================================================================

    InstrumentPatch.h

    The central data model of Axiom: a complete, human-readable description
    of a synthesizer patch. This is what the reconstruction pipeline PRODUCES
    and what the synth engine CONSUMES.

    Design notes:
      - Plain-old-data + std::array only: trivially copyable, so it can be
        handed to the audio thread through a lock-free double buffer.
      - Every field is a *synthesis* parameter (osc types, filter, envelopes),
        never audio data. Axiom reconstructs instruments, it does not stretch
        samples.
      - Serializes to/from JSON (juce::var) for the native .axiom format,
        session state, and as the interchange format between the analytical
        estimator and future ONNX parameter-prediction models.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <array>
#include <optional>

namespace axiom
{

//==============================================================================
enum class OscType : int
{
    Sine = 0,
    Triangle,
    Saw,
    Square,
    Pulse,
    Supersaw,
    Noise,
    Harmonic     // spectral resynthesis: plays the measured harmonic ladder
                 // (patch.harmonicGains) via mip-mapped additive wavetables.
                 // Appended last — serialized as int, order is frozen.
};

enum class FilterType : int
{
    LowPass = 0,
    BandPass,
    HighPass
};

inline const char* toString (OscType t) noexcept
{
    switch (t)
    {
        case OscType::Sine:     return "Sine";
        case OscType::Triangle: return "Triangle";
        case OscType::Saw:      return "Saw";
        case OscType::Square:   return "Square";
        case OscType::Pulse:    return "Pulse";
        case OscType::Supersaw: return "Supersaw";
        case OscType::Noise:    return "Noise";
        case OscType::Harmonic: return "Spectral";
    }
    return "?";
}

inline const char* toString (FilterType t) noexcept
{
    switch (t)
    {
        case FilterType::LowPass:  return "Low-pass";
        case FilterType::BandPass: return "Band-pass";
        case FilterType::HighPass: return "High-pass";
    }
    return "?";
}

//==============================================================================
struct OscSlot
{
    bool     enabled            = false;
    OscType  type               = OscType::Saw;
    float    level              = 0.0f;   // linear 0..1
    float    detuneCents        = 0.0f;   // +/- cents from note pitch
    int      octave             = 0;      // -2..+2
    float    pulseWidth         = 0.5f;   // Pulse only, 0.05..0.95
    float    stereoSpread       = 0.0f;   // 0..1 (Supersaw / unison width)
};

struct EnvParams
{
    float attack  = 0.005f;   // seconds
    float decay   = 0.200f;
    float sustain = 0.800f;   // 0..1
    float release = 0.300f;
};

struct LfoParams
{
    float rateHz          = 5.0f;
    float pitchDepthCents = 0.0f;   // vibrato depth
    float cutoffDepthSt   = 0.0f;   // filter wobble in semitones
};

struct FilterParams
{
    FilterType type          = FilterType::LowPass;
    float      cutoffHz      = 18000.0f;
    float      resonance     = 0.10f;   // 0..1
    float      drive         = 0.0f;    // 0..1
    float      envAmountSt   = 0.0f;    // filter-env modulation, semitones
    float      keyTrack      = 0.5f;    // 0..1
};

struct FxParams
{
    float satDrive      = 0.0f;    // 0..1
    float chorusMix     = 0.0f;    // 0..1
    float chorusRateHz  = 0.8f;
    float chorusDepth   = 0.25f;
    float delayMix      = 0.0f;
    float delayTimeMs   = 350.0f;
    float delayFeedback = 0.35f;
    float reverbMix     = 0.0f;
    float reverbSize    = 0.5f;
    float stereoWidth   = 1.0f;    // 0 = mono, 1 = natural, 2 = wide
    float gainDb        = 0.0f;
};

/** Perceptual character scores (0..1) surfaced in the UI and used by the
    reconstruction pipeline to describe the instrument. */
struct CharacterProfile
{
    float brightness = 0.5f;
    float warmth     = 0.5f;
    float movement   = 0.0f;
    float complexity = 0.0f;
};

//==============================================================================
/** A complete reconstructed instrument. Trivially copyable — safe to move
    across threads by value. */
struct InstrumentPatch
{
    static constexpr int maxOscs = 3;
    static constexpr int maxHarmonicGains = 64;

    std::array<OscSlot, maxOscs> oscs {};

    /** Measured harmonic amplitudes (linear, peak-normalized) driving
        OscType::Harmonic — the source sound's exact sustained spectrum. */
    std::array<float, maxHarmonicGains> harmonicGains {};
    int          numHarmonics   = 0;

    float        noiseLevel     = 0.0f;

    FilterParams filter;
    EnvParams    ampEnv;
    EnvParams    filterEnv;
    LfoParams    lfo;
    FxParams     fx;

    int          rootNote       = 60;      // MIDI note of the source sample
    float        glideMs        = 0.0f;
    float        velToAmp       = 0.7f;    // velocity sensitivity 0..1
    float        velToCutoff    = 0.0f;

    CharacterProfile character;

    /** Overall reconstruction confidence 0..1 (pitch clarity x harmonic clarity). */
    float        confidence     = 0.0f;

    int numActiveOscs() const noexcept
    {
        int n = 0;
        for (auto& o : oscs) n += o.enabled ? 1 : 0;
        return n;
    }
};

//==============================================================================
// JSON serialization (native .axiom patch format, version-tagged so future
// model updates never break old sessions).
//==============================================================================
namespace patch_io
{
    inline constexpr int formatVersion = 1;

    inline juce::var toVar (const InstrumentPatch& p)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("version", formatVersion);

        juce::Array<juce::var> oscArray;
        for (auto& o : p.oscs)
        {
            auto* ov = new juce::DynamicObject();
            ov->setProperty ("enabled",      o.enabled);
            ov->setProperty ("type",         (int) o.type);
            ov->setProperty ("level",        o.level);
            ov->setProperty ("detuneCents",  o.detuneCents);
            ov->setProperty ("octave",       o.octave);
            ov->setProperty ("pulseWidth",   o.pulseWidth);
            ov->setProperty ("stereoSpread", o.stereoSpread);
            oscArray.add (juce::var (ov));
        }
        obj->setProperty ("oscs", oscArray);

        if (p.numHarmonics > 0)
        {
            juce::Array<juce::var> gains;
            for (int i = 0; i < juce::jmin (p.numHarmonics, InstrumentPatch::maxHarmonicGains); ++i)
                gains.add (p.harmonicGains[(size_t) i]);
            obj->setProperty ("harmonics", gains);
        }

        obj->setProperty ("noiseLevel", p.noiseLevel);

        auto envToVar = [] (const EnvParams& e)
        {
            auto* ev = new juce::DynamicObject();
            ev->setProperty ("a", e.attack);  ev->setProperty ("d", e.decay);
            ev->setProperty ("s", e.sustain); ev->setProperty ("r", e.release);
            return juce::var (ev);
        };
        obj->setProperty ("ampEnv",    envToVar (p.ampEnv));
        obj->setProperty ("filterEnv", envToVar (p.filterEnv));

        auto* fv = new juce::DynamicObject();
        fv->setProperty ("type",        (int) p.filter.type);
        fv->setProperty ("cutoffHz",    p.filter.cutoffHz);
        fv->setProperty ("resonance",   p.filter.resonance);
        fv->setProperty ("drive",       p.filter.drive);
        fv->setProperty ("envAmountSt", p.filter.envAmountSt);
        fv->setProperty ("keyTrack",    p.filter.keyTrack);
        obj->setProperty ("filter", juce::var (fv));

        auto* lv = new juce::DynamicObject();
        lv->setProperty ("rateHz",          p.lfo.rateHz);
        lv->setProperty ("pitchDepthCents", p.lfo.pitchDepthCents);
        lv->setProperty ("cutoffDepthSt",   p.lfo.cutoffDepthSt);
        obj->setProperty ("lfo", juce::var (lv));

        auto* xv = new juce::DynamicObject();
        xv->setProperty ("satDrive",      p.fx.satDrive);
        xv->setProperty ("chorusMix",     p.fx.chorusMix);
        xv->setProperty ("chorusRateHz",  p.fx.chorusRateHz);
        xv->setProperty ("chorusDepth",   p.fx.chorusDepth);
        xv->setProperty ("delayMix",      p.fx.delayMix);
        xv->setProperty ("delayTimeMs",   p.fx.delayTimeMs);
        xv->setProperty ("delayFeedback", p.fx.delayFeedback);
        xv->setProperty ("reverbMix",     p.fx.reverbMix);
        xv->setProperty ("reverbSize",    p.fx.reverbSize);
        xv->setProperty ("stereoWidth",   p.fx.stereoWidth);
        xv->setProperty ("gainDb",        p.fx.gainDb);
        obj->setProperty ("fx", juce::var (xv));

        obj->setProperty ("rootNote",    p.rootNote);
        obj->setProperty ("glideMs",     p.glideMs);
        obj->setProperty ("velToAmp",    p.velToAmp);
        obj->setProperty ("velToCutoff", p.velToCutoff);

        auto* cv = new juce::DynamicObject();
        cv->setProperty ("brightness", p.character.brightness);
        cv->setProperty ("warmth",     p.character.warmth);
        cv->setProperty ("movement",   p.character.movement);
        cv->setProperty ("complexity", p.character.complexity);
        obj->setProperty ("character", juce::var (cv));

        obj->setProperty ("confidence", p.confidence);
        return juce::var (obj);
    }

    inline InstrumentPatch fromVar (const juce::var& v)
    {
        InstrumentPatch p;
        auto num  = [] (const juce::var& src, const char* key, float def)
                    { auto x = src.getProperty (key, def); return (float) (double) x; };
        auto numi = [] (const juce::var& src, const char* key, int def)
                    { return (int) src.getProperty (key, def); };

        if (auto* oscArray = v.getProperty ("oscs", {}).getArray())
        {
            for (int i = 0; i < juce::jmin ((int) oscArray->size(), InstrumentPatch::maxOscs); ++i)
            {
                auto& ov = oscArray->getReference (i);
                auto& o  = p.oscs[(size_t) i];
                o.enabled      = (bool) ov.getProperty ("enabled", false);
                o.type         = (OscType) numi (ov, "type", (int) OscType::Saw);
                o.level        = num (ov, "level", 0.0f);
                o.detuneCents  = num (ov, "detuneCents", 0.0f);
                o.octave       = numi (ov, "octave", 0);
                o.pulseWidth   = num (ov, "pulseWidth", 0.5f);
                o.stereoSpread = num (ov, "stereoSpread", 0.0f);
            }
        }
        if (auto* gains = v.getProperty ("harmonics", {}).getArray())
        {
            p.numHarmonics = juce::jmin ((int) gains->size(), InstrumentPatch::maxHarmonicGains);
            for (int i = 0; i < p.numHarmonics; ++i)
                p.harmonicGains[(size_t) i] = (float) (double) gains->getReference (i);
        }

        p.noiseLevel = num (v, "noiseLevel", 0.0f);

        auto envFromVar = [&num] (const juce::var& ev, EnvParams def)
        {
            EnvParams e = def;
            if (ev.isObject())
            {
                e.attack  = num (ev, "a", def.attack);
                e.decay   = num (ev, "d", def.decay);
                e.sustain = num (ev, "s", def.sustain);
                e.release = num (ev, "r", def.release);
            }
            return e;
        };
        p.ampEnv    = envFromVar (v.getProperty ("ampEnv", {}),    {});
        p.filterEnv = envFromVar (v.getProperty ("filterEnv", {}), {});

        auto fv = v.getProperty ("filter", {});
        if (fv.isObject())
        {
            p.filter.type        = (FilterType) numi (fv, "type", 0);
            p.filter.cutoffHz    = num (fv, "cutoffHz", 18000.0f);
            p.filter.resonance   = num (fv, "resonance", 0.1f);
            p.filter.drive       = num (fv, "drive", 0.0f);
            p.filter.envAmountSt = num (fv, "envAmountSt", 0.0f);
            p.filter.keyTrack    = num (fv, "keyTrack", 0.5f);
        }

        auto lv = v.getProperty ("lfo", {});
        if (lv.isObject())
        {
            p.lfo.rateHz          = num (lv, "rateHz", 5.0f);
            p.lfo.pitchDepthCents = num (lv, "pitchDepthCents", 0.0f);
            p.lfo.cutoffDepthSt   = num (lv, "cutoffDepthSt", 0.0f);
        }

        auto xv = v.getProperty ("fx", {});
        if (xv.isObject())
        {
            p.fx.satDrive      = num (xv, "satDrive", 0.0f);
            p.fx.chorusMix     = num (xv, "chorusMix", 0.0f);
            p.fx.chorusRateHz  = num (xv, "chorusRateHz", 0.8f);
            p.fx.chorusDepth   = num (xv, "chorusDepth", 0.25f);
            p.fx.delayMix      = num (xv, "delayMix", 0.0f);
            p.fx.delayTimeMs   = num (xv, "delayTimeMs", 350.0f);
            p.fx.delayFeedback = num (xv, "delayFeedback", 0.35f);
            p.fx.reverbMix     = num (xv, "reverbMix", 0.0f);
            p.fx.reverbSize    = num (xv, "reverbSize", 0.5f);
            p.fx.stereoWidth   = num (xv, "stereoWidth", 1.0f);
            p.fx.gainDb        = num (xv, "gainDb", 0.0f);
        }

        p.rootNote    = numi (v, "rootNote", 60);
        p.glideMs     = num (v, "glideMs", 0.0f);
        p.velToAmp    = num (v, "velToAmp", 0.7f);
        p.velToCutoff = num (v, "velToCutoff", 0.0f);

        auto cv = v.getProperty ("character", {});
        if (cv.isObject())
        {
            p.character.brightness = num (cv, "brightness", 0.5f);
            p.character.warmth     = num (cv, "warmth", 0.5f);
            p.character.movement   = num (cv, "movement", 0.0f);
            p.character.complexity = num (cv, "complexity", 0.0f);
        }

        p.confidence = num (v, "confidence", 0.0f);
        return p;
    }

    inline juce::String toJson (const InstrumentPatch& p)
    {
        return juce::JSON::toString (toVar (p), false);
    }

    inline std::optional<InstrumentPatch> fromJson (const juce::String& json)
    {
        auto parsed = juce::JSON::parse (json);
        if (! parsed.isObject())
            return std::nullopt;
        return fromVar (parsed);
    }

    /** Builds the human-readable "patch discovery" summary shown in the UI,
        e.g. "2x detuned Saw (+/-14 ct) -> Low-pass 3.2 kHz, res 0.35". */
    inline juce::String describe (const InstrumentPatch& p)
    {
        juce::StringArray parts;

        juce::StringArray oscParts;
        for (auto& o : p.oscs)
        {
            if (! o.enabled) continue;
            juce::String s (toString (o.type));
            if (o.type == OscType::Harmonic && p.numHarmonics > 0)
                s << " (" << p.numHarmonics << " partials)";
            if (o.octave != 0)                      s << " " << (o.octave > 0 ? "+" : "") << o.octave << " oct";
            if (std::abs (o.detuneCents) > 1.0f)    s << " (" << juce::String (o.detuneCents, 0) << " ct)";
            oscParts.add (s);
        }
        if (! oscParts.isEmpty())
            parts.add (oscParts.joinIntoString (" + "));
        if (p.noiseLevel > 0.05f)
            parts.add ("noise " + juce::String (juce::roundToInt (p.noiseLevel * 100)) + "%");

        {
            juce::String f (toString (p.filter.type));
            if (p.filter.cutoffHz < 16000.0f)
            {
                f << " " << (p.filter.cutoffHz >= 1000.0f
                                ? juce::String (p.filter.cutoffHz / 1000.0f, 1) + " kHz"
                                : juce::String (juce::roundToInt (p.filter.cutoffHz)) + " Hz");
                if (p.filter.resonance > 0.15f)
                    f << ", res " << juce::String (p.filter.resonance, 2);
                parts.add (f);
            }
        }

        {
            auto ms = [] (float sec) { return juce::String (juce::roundToInt (sec * 1000.0f)) + "ms"; };
            parts.add ("ADSR " + ms (p.ampEnv.attack) + "/" + ms (p.ampEnv.decay) + "/"
                       + juce::String (p.ampEnv.sustain, 2) + "/" + ms (p.ampEnv.release));
        }

        if (p.lfo.pitchDepthCents > 2.0f)
            parts.add ("vibrato " + juce::String (p.lfo.rateHz, 1) + " Hz");
        if (p.lfo.cutoffDepthSt > 0.5f)
            parts.add ("filter LFO " + juce::String (p.lfo.rateHz, 1) + " Hz");
        if (p.fx.chorusMix > 0.1f)   parts.add ("chorus");
        if (p.fx.delayMix > 0.1f)    parts.add ("delay");
        if (p.fx.reverbMix > 0.1f)   parts.add ("reverb");
        if (p.fx.satDrive > 0.1f)    parts.add ("saturation");

        return parts.joinIntoString ("  ·  ");
    }
} // namespace patch_io

} // namespace axiom
