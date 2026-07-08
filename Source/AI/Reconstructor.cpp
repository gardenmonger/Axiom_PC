#include "Reconstructor.h"

#include <cmath>

namespace axiom
{

namespace
{
    /** Ideal harmonic magnitude (dB, h1 = 0) for a raw oscillator shape.
        Used both to classify the source and to expose the filter: the
        difference between measured and ideal spectrum IS the filter curve. */
    float idealHarmonicDb (OscType type, int k /*1-based*/, float pulseWidth)
    {
        switch (type)
        {
            case OscType::Saw:
            case OscType::Supersaw:
                return -20.0f * std::log10 ((float) k);

            case OscType::Square:
                return (k % 2 == 1) ? -20.0f * std::log10 ((float) k) : -80.0f;

            case OscType::Triangle:
                return (k % 2 == 1) ? -40.0f * std::log10 ((float) k) : -80.0f;

            case OscType::Pulse:
            {
                const float a = std::abs (std::sin (juce::MathConstants<float>::pi * k * pulseWidth))
                                / (float) k;
                const float a1 = std::abs (std::sin (juce::MathConstants<float>::pi * pulseWidth));
                return a1 > 1.0e-6f ? 20.0f * std::log10 (std::max (a / a1, 1.0e-4f)) : -80.0f;
            }

            case OscType::Sine:
                return k == 1 ? 0.0f : -80.0f;

            case OscType::Noise:
                return 0.0f;
        }
        return -80.0f;
    }
}

//==============================================================================
AnalyticalReconstructor::OscClassification
AnalyticalReconstructor::classifyOscillator (const AnalysisFeatures& f)
{
    OscClassification result;

    const auto& h = f.harmonicDb;
    if (h.size() < 3)
    {
        result.type       = f.spectralFlatness > 0.3f ? OscType::Noise : OscType::Sine;
        result.confidence = 0.3f;
        return result;
    }

    // Pure sine: everything above the fundamental is buried.
    {
        float maxUpper = -120.0f;
        for (size_t k = 1; k < h.size(); ++k)
            maxUpper = std::max (maxUpper, h[k]);
        if (maxUpper < -35.0f)
        {
            result.type       = OscType::Sine;
            result.confidence = 0.9f;
            return result;
        }
    }

    // Odd-only family (square / triangle) vs full-series (saw / pulse).
    // Power-domain odd ratio: square ~0.97, triangle ~0.99, saw ~0.75,
    // 25% pulse ~0.55.
    if (f.oddEvenRatio > 0.92f)
    {
        // Triangle rolls off at ~ -12 dB per harmonic octave, square at ~ -6.
        const bool isTriangle = f.slopeDbPerHarm < -9.0f;
        result.type       = isTriangle ? OscType::Triangle : OscType::Square;
        result.confidence = 0.75f;
        return result;
    }

    if (f.oddEvenRatio < 0.60f)
    {
        // Strong even content -> asymmetric pulse. Estimate duty cycle from
        // the first spectral null: pulse harmonics vanish at k = 1/duty.
        result.type = OscType::Pulse;
        result.pulseWidth = 0.30f;
        for (size_t k = 2; k + 1 < h.size(); ++k)
        {
            if (h[k] < -30.0f && h[k - 1] > -20.0f && h[k + 1] > -24.0f)
            {
                result.pulseWidth = juce::jlimit (0.10f, 0.45f, 1.0f / (float) (k + 1));
                break;
            }
        }
        result.confidence = 0.6f;
        return result;
    }

    // Saw family. Detuned-unison peak widening or a wide image promotes it
    // to supersaw.
    const bool isSupersaw = f.detuneCentsEst > 8.0f
                            || (f.detuneCentsEst > 4.0f && f.stereoWidth > 0.4f);
    result.type       = isSupersaw ? OscType::Supersaw : OscType::Saw;
    result.confidence = 0.7f;
    return result;
}

//==============================================================================
void AnalyticalReconstructor::estimateFilter (const AnalysisFeatures& f, OscType oscType,
                                              float pulseWidth, FilterParams& filter)
{
    filter = {};

    const auto& h = f.harmonicDb;
    if (h.size() < 4 || f.f0Hz <= 0.0f)
    {
        // No harmonic ladder to compare against — bound brightness by rolloff.
        filter.cutoffHz = juce::jlimit (200.0f, 18000.0f,
                                        f.spectralRolloffHz > 0.0f ? f.spectralRolloffHz : 18000.0f);
        return;
    }

    // Residual = measured spectrum minus the ideal oscillator spectrum,
    // re-anchored at h1. Whatever consistently pulls harmonics down beyond
    // the ideal shape is the filter.
    std::vector<float> residual (h.size(), 0.0f);
    const float anchor = h[0] - idealHarmonicDb (oscType, 1, pulseWidth);
    for (size_t k = 0; k < h.size(); ++k)
    {
        const float ideal = idealHarmonicDb (oscType, (int) k + 1, pulseWidth);
        if (ideal <= -70.0f)                      // harmonic absent in the ideal shape
            residual[k] = 0.0f;
        else
            residual[k] = h[k] - (ideal + anchor);
    }

    // Knee: first harmonic where the residual drops below -6 dB and the drop
    // persists (one outlier tolerated).
    int knee = -1;
    for (size_t k = 1; k < residual.size(); ++k)
    {
        const float ideal = idealHarmonicDb (oscType, (int) k + 1, pulseWidth);
        if (ideal <= -70.0f) continue;

        if (residual[k] < -6.0f)
        {
            int below = 0, considered = 0;
            for (size_t j = k; j < residual.size() && considered < 4; ++j)
            {
                if (idealHarmonicDb (oscType, (int) j + 1, pulseWidth) <= -70.0f) continue;
                ++considered;
                if (residual[j] < -6.0f) ++below;
            }
            if (considered == 0 || below >= considered - 1)
            {
                knee = (int) k + 1;               // 1-based harmonic index
                break;
            }
        }
    }

    if (knee > 1)
    {
        // Cutoff sits between the last unattenuated harmonic and the knee.
        filter.cutoffHz = juce::jlimit (100.0f, 18000.0f,
                                        f.f0Hz * std::sqrt ((float) knee * (float) (knee - 1)));

        // Resonance: positive residual bump just before the knee.
        float bump = 0.0f;
        for (int k = std::max (1, knee - 2); k <= std::min ((int) residual.size(), knee); ++k)
            bump = std::max (bump, residual[(size_t) k - 1]);
        filter.resonance = juce::jlimit (0.0f, 0.9f, bump / 12.0f);
    }
    else
    {
        filter.cutoffHz  = 18000.0f;              // filter effectively open
        filter.resonance = 0.1f;
    }

    // Percussive sources (pluck/stab): the brightness dies with the amp —
    // model it as filter-envelope sweep from the measured decay.
    if (! f.isSustained && filter.cutoffHz < 12000.0f)
        filter.envAmountSt = juce::jlimit (0.0f, 36.0f, 24.0f * (1.0f - f.sustainLvl));
}

//==============================================================================
InstrumentPatch AnalyticalReconstructor::reconstruct (const AnalysisFeatures& f)
{
    InstrumentPatch p;

    if (! f.isValid())
    {
        p.confidence = 0.0f;
        return p;                                 // default patch, flagged untrusted
    }

    p.rootNote = f.rootMidiNote;

    // --- Oscillator section --------------------------------------------------
    const auto osc = classifyOscillator (f);
    const int measured = (int) f.harmonicDb.size();

    // Spectral resynthesis: whenever the harmonic ladder is trustworthy,
    // reproduce the MEASURED spectrum exactly (mip-mapped additive
    // wavetable) instead of approximating with an idealized waveform —
    // this is what keeps the instrument faithful to the source.
    const bool spectral = f.f0Hz > 0.0f
                          && measured >= 2
                          && f.pitchConfidence > 0.45f;

    if (spectral)
    {
        p.numHarmonics = juce::jmin (measured, InstrumentPatch::maxHarmonicGains);
        for (int k = 0; k < p.numHarmonics; ++k)
            p.harmonicGains[(size_t) k] =
                std::pow (10.0f, juce::jlimit (-80.0f, 0.0f, f.harmonicDb[(size_t) k]) / 20.0f);

        auto& o0 = p.oscs[0];
        o0.enabled = true;
        o0.type    = OscType::Harmonic;
        o0.level   = 0.9f;

        if (f.detuneCentsEst > 3.0f)
        {
            // Detuned unison pair of the same spectrum recreates the source's
            // beating/width that a single averaged snapshot would lose.
            auto& o1 = p.oscs[1];
            o1 = o0;
            o0.detuneCents  = -0.5f * f.detuneCentsEst;
            o1.detuneCents  = +0.5f * f.detuneCentsEst;
            o0.level = o1.level = 0.65f;
            o0.stereoSpread = o1.stereoSpread = juce::jlimit (0.3f, 0.9f, f.stereoWidth + 0.35f);
        }

        // The wavetable already embeds the source's sustained filtering, so
        // the SVF stays out of the way on sustained sounds. Percussive
        // sources get a downward brightness sweep from the measured decay.
        if (f.isSustained)
        {
            p.filter = {};
            p.filter.cutoffHz  = 18000.0f;
            p.filter.resonance = 0.05f;
            p.filter.keyTrack  = 0.3f;
        }
        else
        {
            p.filter = {};
            p.filter.cutoffHz    = juce::jlimit (250.0f, 9000.0f, f.spectralCentroidHz * 0.6f);
            p.filter.resonance   = 0.1f;
            p.filter.envAmountSt = juce::jlimit (6.0f, 36.0f, 24.0f * (1.0f - f.sustainLvl));
            p.filter.keyTrack    = 0.5f;
        }
    }
    else
    {
        auto& o0 = p.oscs[0];
        o0.enabled    = true;
        o0.type       = osc.type;
        o0.level      = 0.9f;
        o0.pulseWidth = osc.pulseWidth;

        if (osc.type == OscType::Supersaw)
        {
            o0.detuneCents  = juce::jlimit (4.0f, 50.0f, f.detuneCentsEst);
            o0.stereoSpread = juce::jlimit (0.2f, 1.0f, f.stereoWidth + 0.3f);
        }
        else if (f.detuneCentsEst > 3.0f)
        {
            // Two-oscillator detune pair instead of full supersaw.
            auto& o1 = p.oscs[1];
            o1 = o0;
            o0.detuneCents = -0.5f * f.detuneCentsEst;
            o1.detuneCents = +0.5f * f.detuneCentsEst;
            o0.level = o1.level = 0.65f;
        }

        // Sub-octave: strong fundamental relative to a bright upper spectrum
        // suggests a reinforcing sub oscillator.
        if (f.harmonicDb.size() > 4 && f.spectralCentroidHz > 4.0f * f.f0Hz
            && f.harmonicDb[0] >= -1.0f && osc.type != OscType::Sine)
        {
            auto& sub = p.oscs[2];
            sub.enabled = true;
            sub.type    = OscType::Sine;
            sub.octave  = -1;
            sub.level   = 0.35f;
        }

        estimateFilter (f, osc.type, osc.pulseWidth, p.filter);
        p.filter.keyTrack = 0.5f;
    }

    p.noiseLevel = f.noisiness > 0.15f ? juce::jlimit (0.0f, 0.9f, f.noisiness * 0.8f) : 0.0f;

    // --- Envelopes ---------------------------------------------------------------
    p.ampEnv.attack  = f.attackSec;
    p.ampEnv.decay   = f.decaySec;
    p.ampEnv.sustain = f.isSustained ? f.sustainLvl : 0.0f;
    p.ampEnv.release = f.releaseSec;

    p.filterEnv = p.ampEnv;
    if (! f.isSustained)
    {
        p.filterEnv.decay   = std::max (0.02f, f.decaySec * 0.7f);
        p.filterEnv.sustain = 0.0f;
    }

    // --- Modulation -----------------------------------------------------------
    if (f.vibratoDepthCents > 2.0f)
    {
        p.lfo.rateHz          = juce::jlimit (0.3f, 12.0f, f.vibratoRateHz);
        p.lfo.pitchDepthCents = juce::jlimit (0.0f, 100.0f, f.vibratoDepthCents);
    }
    else if (f.centroidModDepth > 0.08f && f.centroidModRateHz > 0.2f)
    {
        p.lfo.rateHz        = juce::jlimit (0.1f, 12.0f, f.centroidModRateHz);
        p.lfo.cutoffDepthSt = juce::jlimit (0.0f, 24.0f, f.centroidModDepth * 24.0f);
    }

    // --- Effects -----------------------------------------------------------------
    p.fx.stereoWidth = 1.0f + juce::jlimit (0.0f, 1.0f, f.stereoWidth);

    // Wide but not unison-detuned -> modulated widening, i.e. chorus.
    if (f.stereoWidth > 0.25f && f.detuneCentsEst < 8.0f && osc.type != OscType::Noise)
        p.fx.chorusMix = juce::jlimit (0.0f, 0.6f, f.stereoWidth * 0.7f);

    // Long measured tail on a sustained source reads as room, not envelope.
    if (f.isSustained && f.releaseSec > 1.0f)
    {
        p.fx.reverbMix  = juce::jlimit (0.0f, 0.35f, (f.releaseSec - 1.0f) * 0.12f);
        p.fx.reverbSize = 0.6f;
        p.ampEnv.release = juce::jlimit (0.05f, 1.2f, f.releaseSec * 0.5f);
    }

    // Harmonics stronger than any raw shape can produce -> waveshaping.
    // (Only for the idealized-waveform path — the spectral wavetable already
    // contains the source's exact harmonic weights.)
    if (! spectral && f.slopeDbPerHarm > -4.0f && f.harmonicDb.size() >= 8
        && osc.type != OscType::Sine)
        p.fx.satDrive = 0.25f;

    // --- Character / dynamics ----------------------------------------------------
    p.character.brightness = juce::jlimit (0.0f, 1.0f, f.spectralCentroidHz / 8000.0f);
    p.character.warmth     = juce::jlimit (0.0f, 1.0f, 1.0f - f.spectralCentroidHz / 6000.0f);
    p.character.movement   = juce::jlimit (0.0f, 1.0f,
                                std::max ({ f.vibratoDepthCents / 50.0f,
                                            f.centroidModDepth * 2.0f,
                                            f.stereoWidth }));
    p.character.complexity = juce::jlimit (0.0f, 1.0f, f.inharmonicity * 2.0f + f.spectralFlatness);

    p.velToAmp = f.dynamicRangeDb > 30.0f ? 0.8f : 0.5f;

    // --- Confidence ----------------------------------------------------------------
    const float harmonicClarity = juce::jlimit (0.0f, 1.0f, (float) f.harmonicDb.size() / 8.0f);
    const float timbreConf      = spectral ? 0.95f : osc.confidence;   // exact spectrum match
    p.confidence = juce::jlimit (0.0f, 1.0f,
                                 f.pitchConfidence * (1.0f - 0.5f * f.noisiness)
                                     * (0.4f + 0.6f * harmonicClarity)
                                     * (0.5f + 0.5f * timbreConf));
    return p;
}

//==============================================================================
// HybridReconstructor
//==============================================================================
std::vector<float> HybridReconstructor::packFeatureVector (const AnalysisFeatures& f)
{
    std::vector<float> v;
    v.reserve (24 + 24);

    for (int k = 0; k < 24; ++k)
        v.push_back (k < (int) f.harmonicDb.size() ? std::max (f.harmonicDb[(size_t) k], -80.0f)
                                                   : -80.0f);
    v.push_back (f.f0Hz);
    v.push_back ((float) f.rootMidiNote);
    v.push_back (f.pitchConfidence);
    v.push_back (f.pitchStability);
    v.push_back (f.oddEvenRatio);
    v.push_back (f.slopeDbPerHarm);
    v.push_back (f.inharmonicity);
    v.push_back (f.spectralCentroidHz);
    v.push_back (f.spectralFlatness);
    v.push_back (f.spectralRolloffHz);
    v.push_back (f.noisiness);
    v.push_back (f.attackSec);
    v.push_back (f.decaySec);
    v.push_back (f.sustainLvl);
    v.push_back (f.releaseSec);
    v.push_back (f.isSustained ? 1.0f : 0.0f);
    v.push_back (f.vibratoRateHz);
    v.push_back (f.vibratoDepthCents);
    v.push_back (f.centroidModRateHz);
    v.push_back (f.centroidModDepth);
    v.push_back (f.detuneCentsEst);
    v.push_back (f.stereoWidth);
    v.push_back (f.dynamicRangeDb);
    v.push_back (f.transientSharpness);
    return v;
}

InstrumentPatch HybridReconstructor::reconstruct (const AnalysisFeatures& features)
{
    // Always compute the analytical result — it is both the fallback and the
    // prior that a refinement model corrects.
    auto analyticalPatch = analytical.reconstruct (features);
    lastTier = analytical.getTierName();

    if (engine.isModelAvailable ("patch_refiner"))
    {
        const auto input = packFeatureVector (features);
        std::vector<float> output;

        if (engine.run ("patch_refiner", input,
                        { 1, (int64_t) input.size() }, output)
            && output.size() >= 8)
        {
            // Model output spec v1 (normalized 0..1, denormalized here):
            // [0] cutoff (log 20 Hz..20 kHz)  [1] resonance   [2] detune 0..50 ct
            // [3] attack 0..4 s (log)          [4] decay        [5] sustain
            // [6] release 0..8 s (log)         [7] confidence
            auto& pf = analyticalPatch;
            auto lerpLog = [] (float n, float lo, float hi)
                           { return lo * std::pow (hi / lo, juce::jlimit (0.0f, 1.0f, n)); };

            pf.filter.cutoffHz  = lerpLog (output[0], 20.0f, 20000.0f);
            pf.filter.resonance = juce::jlimit (0.0f, 0.95f, output[1]);
            pf.ampEnv.attack    = lerpLog (output[3], 0.001f, 4.0f);
            pf.ampEnv.decay     = lerpLog (output[4], 0.005f, 8.0f);
            pf.ampEnv.sustain   = juce::jlimit (0.0f, 1.0f, output[5]);
            pf.ampEnv.release   = lerpLog (output[6], 0.01f, 8.0f);
            pf.confidence       = juce::jlimit (pf.confidence, 1.0f, output[7]);

            lastTier = "Neural (patch_refiner) + Analytical prior";
        }
    }

    return analyticalPatch;
}

} // namespace axiom
