#include "DdspEncoder.h"

#include <algorithm>
#include <cmath>

namespace axiom
{

namespace
{
    constexpr int fftOrder = 12;                 // 4096-point STFT
    constexpr int fftSize  = 1 << fftOrder;
    constexpr int numBins  = fftSize / 2;

    /** Parabolic peak refinement on a magnitude spectrum (same scheme as
        AudioAnalyzer). Returns the interpolated peak magnitude. */
    float refinePeakMag (const std::vector<float>& mag, int bin)
    {
        if (bin <= 0 || bin >= (int) mag.size() - 1)
            return mag[(size_t) std::clamp (bin, 0, (int) mag.size() - 1)];

        const float a = mag[(size_t) bin - 1], b = mag[(size_t) bin], c = mag[(size_t) bin + 1];
        const float denom = a - 2.0f * b + c;
        float offset = 0.0f;
        if (std::abs (denom) > 1.0e-12f)
            offset = std::clamp (0.5f * (a - c) / denom, -0.5f, 0.5f);
        return b - 0.25f * (a - c) * offset;
    }
}

//==============================================================================
DdspTimbre DdspEncoder::extract (const juce::AudioBuffer<float>& audio,
                                 double sr,
                                 const AnalysisFeatures& features,
                                 IInferenceEngine& inference,
                                 const std::function<bool()>& shouldAbort)
{
    DdspTimbre t;

    const int numSamples = audio.getNumSamples();
    if (features.f0Hz <= 20.0f || numSamples < fftSize || sr <= 0.0
        || audio.getNumChannels() < 1)
        return {};

    auto aborted = [&shouldAbort] { return shouldAbort && shouldAbort(); };

    // --- Mono mixdown, peak-normalized (mirrors AudioAnalyzer) ---------------
    std::vector<float> mono ((size_t) numSamples, 0.0f);
    for (int ch = 0; ch < audio.getNumChannels(); ++ch)
    {
        const float* src = audio.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
            mono[(size_t) i] += src[i];
    }
    const float chScale = 1.0f / (float) audio.getNumChannels();
    float peak = 0.0f;
    for (auto& s : mono) { s *= chScale; peak = std::max (peak, std::abs (s)); }
    if (peak < 1.0e-5f)
        return {};
    for (auto& s : mono)
        s /= peak;

    // --- Frame grid: dense enough for timbre motion, capped for memory -------
    const int hop = std::max ({ 256, fftSize / 8,
                                (numSamples + DdspTimbre::maxFrames - 1) / DdspTimbre::maxFrames });
    const int numFrames = std::clamp (numSamples / hop, 2, DdspTimbre::maxFrames);

    t.numFrames    = numFrames;
    t.frameRate    = (float) (sr / hop);
    t.sourceRootHz = features.f0Hz;
    t.numHarmonics = std::clamp ((int) (0.45 * sr / features.f0Hz), 1,
                                 DdspTimbre::maxHarmonics);

    t.harm.assign ((size_t) numFrames * DdspTimbre::maxHarmonics, 0.0f);
    t.noiseGain.assign ((size_t) numFrames, 0.0f);
    t.noiseCutoffHz.assign ((size_t) numFrames, 8000.0f);
    t.loudness.assign ((size_t) numFrames, 0.0f);

    std::vector<float> frameF0 ((size_t) numFrames, features.f0Hz);

    juce::dsp::FFT fft (fftOrder);
    juce::dsp::WindowingFunction<float> window ((size_t) fftSize,
                                                juce::dsp::WindowingFunction<float>::hann);

    const double binHz = sr / fftSize;
    std::vector<float>   buf ((size_t) fftSize * 2, 0.0f);
    std::vector<float>   mag ((size_t) numBins, 0.0f);
    std::vector<uint8_t> isHarmonicBin ((size_t) numBins, 0);

    const int loBin = std::max (1, (int) (20.0 / binHz));
    const int hiBin = std::min (numBins - 1, (int) (16000.0 / binHz));

    float lastF0 = features.f0Hz;

    // --- Per-frame harmonic + noise measurement -------------------------------
    for (int f = 0; f < numFrames; ++f)
    {
        if ((f & 31) == 0 && aborted())
            return {};

        const int pos   = f * hop;
        const int avail = std::min (fftSize, numSamples - pos);
        if (avail <= 0)
            break;

        std::copy (mono.begin() + pos, mono.begin() + pos + avail, buf.begin());
        std::fill (buf.begin() + avail, buf.end(), 0.0f);

        double rms = 0.0;
        for (int i = 0; i < avail; ++i)
            rms += (double) buf[(size_t) i] * buf[(size_t) i];
        t.loudness[(size_t) f] = (float) std::sqrt (rms / std::max (avail, 1));

        window.multiplyWithWindowingTable (buf.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (buf.data());
        std::copy (buf.begin(), buf.begin() + numBins, mag.begin());

        // Refine this frame's f0 near the previous one (tracks vibrato /
        // slow drift; jumps are rejected and fall back to the YIN median).
        {
            const int centre = (int) std::lround (lastF0 / binHz);
            const int span   = std::max (2, (int) (centre * 0.06));
            int best = std::clamp (centre, 1, numBins - 2);
            for (int b = std::max (1, centre - span);
                 b <= std::min (numBins - 2, centre + span); ++b)
                if (mag[(size_t) b] > mag[(size_t) best])
                    best = b;

            if (mag[(size_t) best] > 1.0e-6f)
            {
                const float refined = (float) (best * binHz);
                if (refined > 0.7f * features.f0Hz && refined < 1.4f * features.f0Hz)
                    lastF0 = refined;
            }
        }
        frameF0[(size_t) f] = lastF0;

        // Harmonic ladder at k * frameF0 (±3-bin search + parabolic refine).
        std::fill (isHarmonicBin.begin(), isHarmonicBin.end(), (uint8_t) 0);
        float* row = t.harm.data() + (size_t) f * DdspTimbre::maxHarmonics;

        for (int k = 1; k <= t.numHarmonics; ++k)
        {
            const int targetBin = (int) std::lround ((double) k * lastF0 / binHz);
            if (targetBin >= numBins - 4)
                break;

            int bestBin = std::clamp (targetBin, 1, numBins - 2);
            for (int b = std::max (1, targetBin - 3);
                 b <= std::min (numBins - 2, targetBin + 3); ++b)
                if (mag[(size_t) b] > mag[(size_t) bestBin])
                    bestBin = b;

            row[k - 1] = refinePeakMag (mag, bestBin);

            for (int b = std::max (0, bestBin - 2);
                 b <= std::min (numBins - 1, bestBin + 2); ++b)
                isHarmonicBin[(size_t) b] = 1;
        }

        // Residual = everything that is not a harmonic neighborhood. With a
        // Hann window the scale factors cancel such that sqrt(residual power)
        // is directly comparable to the harmonic peak magnitudes above.
        double residual = 0.0, cum = 0.0;
        for (int b = loBin; b <= hiBin; ++b)
            if (! isHarmonicBin[(size_t) b])
                residual += (double) mag[(size_t) b] * mag[(size_t) b];

        t.noiseGain[(size_t) f] = (float) std::sqrt (std::max (0.0, residual));

        float cutoff = 8000.0f;
        for (int b = loBin; b <= hiBin; ++b)
        {
            if (! isHarmonicBin[(size_t) b])
                cum += (double) mag[(size_t) b] * mag[(size_t) b];
            if (cum >= 0.75 * residual && residual > 0.0)
            {
                cutoff = (float) (b * binHz);
                break;
            }
        }
        t.noiseCutoffHz[(size_t) f] = juce::jlimit (200.0f, 18000.0f, cutoff);
    }

    // Normalize loudness to 0..1 for the decoder input / serialization.
    {
        float maxLoud = 1.0e-9f;
        for (auto v : t.loudness) maxLoud = std::max (maxLoud, v);
        for (auto& v : t.loudness) v /= maxLoud;
    }

    // --- Optional neural tier: ddsp_decoder predicts the frames --------------
    t.tierName = "DDSP measured (STFT)";

    if (inference.isModelAvailable (modelSlotName))
    {
        std::vector<float> input;
        input.reserve ((size_t) numFrames * 2);
        for (int f = 0; f < numFrames; ++f)
        {
            const float midi = 69.0f + 12.0f * std::log2 (frameF0[(size_t) f] / 440.0f);
            input.push_back (juce::jlimit (0.0f, 1.0f, midi / 127.0f));
            input.push_back (t.loudness[(size_t) f]);
        }

        std::vector<float> output;
        if (! aborted()
            && inference.run (modelSlotName, input, { 1, numFrames, 2 }, output)
            && output.size() == (size_t) numFrames * 66)
        {
            for (int f = 0; f < numFrames; ++f)
            {
                const float* o   = output.data() + (size_t) f * 66;
                float*       row = t.harm.data() + (size_t) f * DdspTimbre::maxHarmonics;
                for (int k = 0; k < DdspTimbre::maxHarmonics; ++k)
                    row[k] = std::max (0.0f, o[k]);

                t.noiseGain[(size_t) f]     = std::max (0.0f, o[64]);
                t.noiseCutoffHz[(size_t) f] = juce::jlimit (
                    200.0f, 18000.0f,
                    20.0f * std::exp2 (juce::jlimit (0.0f, 1.0f, o[65]) * 10.0f));
            }
            t.numHarmonics = DdspTimbre::maxHarmonics;
            t.tierName     = "DDSP neural (ONNX)";
        }
    }

    // --- Level calibration -----------------------------------------------------
    // Scale so the loudest frame's partial sum is 1.0: the additive stack's
    // waveform peak is bounded by that sum, putting the layer in the same
    // headroom class as the spectral wavetables (table peak 0.95) before the
    // per-voice 0.32 gain. Noise scales identically to preserve the measured
    // harmonic/noise balance.
    {
        float maxFrameSum = 0.0f;
        for (int f = 0; f < numFrames; ++f)
        {
            const float* row = t.frame (f);
            float sum = 0.0f;
            for (int k = 0; k < t.numHarmonics; ++k)
                sum += row[k];
            maxFrameSum = std::max (maxFrameSum, sum);
        }
        if (maxFrameSum < 1.0e-9f)
            return {};                          // nothing tonal to resynthesize

        const float scale = 1.0f / maxFrameSum;
        for (auto& v : t.harm)      v *= scale;
        for (auto& v : t.noiseGain) v *= scale;
    }

    // --- Sustain loop ------------------------------------------------------------
    const double lengthSec = numSamples / sr;
    const double loopInSec = std::min ((double) (features.attackSec + features.decaySec) + 0.05,
                                       0.4 * lengthSec);
    t.loopStartFrame = juce::jlimit (0, numFrames - 2, (int) (loopInSec * t.frameRate));
    t.loopEndFrame   = juce::jlimit (t.loopStartFrame + 1, numFrames - 1,
                                     (int) (0.9 * lengthSec * t.frameRate));
    t.oneShot        = ! features.isSustained;

    return t;
}

} // namespace axiom
