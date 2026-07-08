/*
  ==============================================================================

    ParamIDs.h

    Host-automatable parameter identifiers, shared by the APVTS layout
    (PluginProcessor), the runtime override merge (SynthEngine) and the GUI
    attachments. IDs are stable strings — never renumber or rename once
    shipped, or host automation lanes break.

  ==============================================================================
*/

#pragma once

namespace axiom::params
{
    // Filter
    inline constexpr auto cutoff       = "cutoff";
    inline constexpr auto resonance    = "resonance";
    inline constexpr auto drive        = "drive";
    inline constexpr auto filterType   = "filterType";
    inline constexpr auto filterEnvAmt = "filterEnvAmt";

    // Amp envelope
    inline constexpr auto ampAttack   = "ampAttack";
    inline constexpr auto ampDecay    = "ampDecay";
    inline constexpr auto ampSustain  = "ampSustain";
    inline constexpr auto ampRelease  = "ampRelease";

    // Filter envelope
    inline constexpr auto fltAttack   = "fltAttack";
    inline constexpr auto fltDecay    = "fltDecay";
    inline constexpr auto fltSustain  = "fltSustain";
    inline constexpr auto fltRelease  = "fltRelease";

    // LFO
    inline constexpr auto lfoRate     = "lfoRate";
    inline constexpr auto lfoPitch    = "lfoPitch";
    inline constexpr auto lfoCutoff   = "lfoCutoff";

    // Oscillator macro
    inline constexpr auto detune      = "detune";
    inline constexpr auto noiseLevel  = "noiseLevel";
    inline constexpr auto glide       = "glide";

    // FX
    inline constexpr auto satDrive    = "satDrive";
    inline constexpr auto chorusMix   = "chorusMix";
    inline constexpr auto delayMix    = "delayMix";
    inline constexpr auto delayTime   = "delayTime";
    inline constexpr auto delayFeedback = "delayFeedback";
    inline constexpr auto reverbMix   = "reverbMix";
    inline constexpr auto reverbSize  = "reverbSize";
    inline constexpr auto width       = "width";

    // Output
    inline constexpr auto masterGain  = "masterGain";
}
