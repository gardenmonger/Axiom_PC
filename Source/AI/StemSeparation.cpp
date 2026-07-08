#include "StemSeparation.h"

#include <algorithm>
#include <cmath>

namespace axiom
{

namespace
{
    //==========================================================================
    // Shared helpers
    //==========================================================================

    /** Linear-interpolating resampler — quality is ample for handing audio to
        a separation network and back (the network is the fidelity bottleneck). */
    juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& in,
                                       double fromRate, double toRate)
    {
        if (std::abs (fromRate - toRate) < 1.0)
            return in;

        const double ratio = fromRate / toRate;
        const int outLen = (int) std::floor (in.getNumSamples() / ratio);
        juce::AudioBuffer<float> out (in.getNumChannels(), outLen);

        for (int ch = 0; ch < in.getNumChannels(); ++ch)
        {
            const float* src = in.getReadPointer (ch);
            float* dst = out.getWritePointer (ch);
            for (int i = 0; i < outLen; ++i)
            {
                const double pos = i * ratio;
                const int idx = (int) pos;
                const float frac = (float) (pos - idx);
                const float a = src[juce::jmin (idx, in.getNumSamples() - 1)];
                const float b = src[juce::jmin (idx + 1, in.getNumSamples() - 1)];
                dst[i] = a + (b - a) * frac;
            }
        }
        return out;
    }

    struct SeparatorManifest
    {
        juce::StringArray stems { "Drums", "Bass", "Other", "Vocals" };
        double modelSampleRate = 44100.0;
        double chunkSeconds    = 10.0;
        double overlapSeconds  = 1.0;
    };

    SeparatorManifest loadManifest()
    {
        SeparatorManifest m;
        const auto file = ModelRegistry::getModelDirectory().getChildFile ("stem_separator.json");
        if (! file.existsAsFile())
            return m;

        const auto parsed = juce::JSON::parse (file.loadFileAsString());
        if (auto* stems = parsed.getProperty ("stems", {}).getArray())
        {
            m.stems.clear();
            for (auto& s : *stems)
                m.stems.add (s.toString());
        }
        m.modelSampleRate = (double) parsed.getProperty ("sampleRate",   m.modelSampleRate);
        m.chunkSeconds    = (double) parsed.getProperty ("chunkSeconds", m.chunkSeconds);
        m.overlapSeconds  = (double) parsed.getProperty ("overlapSeconds", m.overlapSeconds);
        return m;
    }
}

//==============================================================================
juce::String StemSeparationEngine::backendName (const IInferenceEngine& engine)
{
    return engine.isModelAvailable ("stem_separator") ? "Neural (stem_separator)"
                                                      : "DSP (harmonic / percussive)";
}

std::vector<Stem> StemSeparationEngine::separate (const juce::AudioBuffer<float>& mix,
                                                  double sampleRate,
                                                  IInferenceEngine& engine,
                                                  std::atomic<float>& progress,
                                                  const std::function<bool()>& shouldAbort)
{
    progress = 0.0f;
    if (mix.getNumSamples() == 0)
        return {};

    if (engine.isModelAvailable ("stem_separator"))
    {
        auto stems = separateNeural (mix, sampleRate, engine, progress, shouldAbort);
        if (! stems.empty())
            return stems;
        // Model present but failed to run — degrade gracefully to DSP.
    }
    return separateHpss (mix, sampleRate, progress, shouldAbort);
}

//==============================================================================
// Neural path
//==============================================================================
std::vector<Stem> StemSeparationEngine::separateNeural (const juce::AudioBuffer<float>& mixIn,
                                                        double sampleRate,
                                                        IInferenceEngine& engine,
                                                        std::atomic<float>& progress,
                                                        const std::function<bool()>& shouldAbort)
{
    const auto manifest = loadManifest();
    const int numStems  = juce::jmax (1, manifest.stems.size());

    // Model I/O is stereo at the model's native rate.
    juce::AudioBuffer<float> stereo (2, mixIn.getNumSamples());
    stereo.copyFrom (0, 0, mixIn, 0, 0, mixIn.getNumSamples());
    stereo.copyFrom (1, 0, mixIn, juce::jmin (1, mixIn.getNumChannels() - 1), 0, mixIn.getNumSamples());

    auto work = resample (stereo, sampleRate, manifest.modelSampleRate);
    const int total = work.getNumSamples();

    const int chunk   = juce::jmax (1024, (int) (manifest.chunkSeconds   * manifest.modelSampleRate));
    const int overlap = juce::jlimit (0, chunk / 2, (int) (manifest.overlapSeconds * manifest.modelSampleRate));
    const int hop     = chunk - overlap;

    std::vector<juce::AudioBuffer<float>> stems ((size_t) numStems);
    juce::AudioBuffer<float> weight (1, total);
    weight.clear();
    for (auto& s : stems)
    {
        s.setSize (2, total);
        s.clear();
    }

    std::vector<float> input ((size_t) chunk * 2, 0.0f);
    std::vector<float> output;

    const int numChunks = juce::jmax (1, (total + hop - 1) / hop);
    int chunkIndex = 0;

    for (int pos = 0; pos < total; pos += hop, ++chunkIndex)
    {
        if (shouldAbort && shouldAbort())
            return {};

        const int len = juce::jmin (chunk, total - pos);

        // Pack [1, 2, chunk] — zero-padded tail keeps the tensor shape fixed.
        std::fill (input.begin(), input.end(), 0.0f);
        for (int ch = 0; ch < 2; ++ch)
            std::copy_n (work.getReadPointer (ch) + pos, len,
                         input.begin() + (size_t) ch * (size_t) chunk);

        if (! engine.run ("stem_separator", input, { 1, 2, (int64_t) chunk }, output)
            || output.size() < (size_t) numStems * 2 * (size_t) chunk)
        {
            DBG ("Axiom separation: model run failed at chunk " << chunkIndex);
            return {};
        }

        // Triangular chunk weighting -> smooth crossfade in the overlap zones.
        for (int i = 0; i < len; ++i)
        {
            float w = 1.0f;
            if (overlap > 0)
            {
                if (pos > 0 && i < overlap)              w = (float) i / (float) overlap;
                if (pos + chunk < total && i >= len - overlap)
                    w = juce::jmin (w, (float) (len - 1 - i) / (float) overlap);
            }
            weight.getWritePointer (0)[pos + i] += w;
        }

        for (int s = 0; s < numStems; ++s)
            for (int ch = 0; ch < 2; ++ch)
            {
                const float* src = output.data()
                                 + ((size_t) s * 2 + (size_t) ch) * (size_t) chunk;
                float* dst = stems[(size_t) s].getWritePointer (ch) + pos;
                for (int i = 0; i < len; ++i)
                {
                    float w = 1.0f;
                    if (overlap > 0)
                    {
                        if (pos > 0 && i < overlap)              w = (float) i / (float) overlap;
                        if (pos + chunk < total && i >= len - overlap)
                            w = juce::jmin (w, (float) (len - 1 - i) / (float) overlap);
                    }
                    dst[i] += src[i] * w;
                }
            }

        progress = 0.95f * (float) (chunkIndex + 1) / (float) numChunks;
    }

    // Normalize by accumulated weights, then return to the source sample rate.
    const float* w = weight.getReadPointer (0);
    for (auto& s : stems)
        for (int ch = 0; ch < 2; ++ch)
        {
            float* d = s.getWritePointer (ch);
            for (int i = 0; i < total; ++i)
                d[i] /= juce::jmax (w[i], 1.0e-4f);
        }

    std::vector<Stem> result;
    for (int s = 0; s < numStems; ++s)
        result.push_back ({ manifest.stems[s],
                            resample (stems[(size_t) s], manifest.modelSampleRate, sampleRate) });

    progress = 1.0f;
    return result;
}

//==============================================================================
// HPSS fallback (median-filtering harmonic/percussive separation)
//==============================================================================
namespace
{
    constexpr int hpssOrder   = 11;               // 2048-point STFT
    constexpr int hpssSize    = 1 << hpssOrder;
    constexpr int hpssHop     = hpssSize / 4;
    constexpr int hpssBins    = hpssSize / 2 + 1;
    constexpr int medianSpan  = 17;               // frames (time) / bins (freq)

    float medianOf (std::vector<float>& scratch)
    {
        const auto mid = scratch.size() / 2;
        std::nth_element (scratch.begin(), scratch.begin() + (long) mid, scratch.end());
        return scratch[mid];
    }
}

std::vector<Stem> StemSeparationEngine::separateHpss (const juce::AudioBuffer<float>& mix,
                                                      double /*sampleRate*/,
                                                      std::atomic<float>& progress,
                                                      const std::function<bool()>& shouldAbort)
{
    const int numChannels = juce::jmin (2, mix.getNumChannels());
    const int numSamples  = mix.getNumSamples();
    const int numFrames   = juce::jmax (1, (numSamples - hpssSize) / hpssHop + 1);

    juce::dsp::FFT fft (hpssOrder);
    juce::dsp::WindowingFunction<float> window ((size_t) hpssSize,
                                                juce::dsp::WindowingFunction<float>::hann,
                                                false);

    std::vector<Stem> result (2);
    result[0].name = "Harmonic";
    result[1].name = "Percussive";
    for (auto& s : result)
    {
        s.audio.setSize (numChannels, numSamples);
        s.audio.clear();
    }

    // Per-channel processing keeps the stereo image of each stem intact.
    for (int ch = 0; ch < numChannels; ++ch)
    {
        // --- Analysis STFT --------------------------------------------------
        // spectra: interleaved re/im per frame; mag: magnitude matrix.
        std::vector<std::vector<float>> spectra ((size_t) numFrames);
        std::vector<std::vector<float>> mag ((size_t) numFrames,
                                             std::vector<float> ((size_t) hpssBins));

        {
            std::vector<float> frame ((size_t) hpssSize * 2, 0.0f);
            const float* src = mix.getReadPointer (ch);

            for (int f = 0; f < numFrames; ++f)
            {
                if (shouldAbort && shouldAbort()) return {};

                const int pos = f * hpssHop;
                const int len = juce::jmin (hpssSize, numSamples - pos);
                std::fill (frame.begin(), frame.end(), 0.0f);
                std::copy_n (src + pos, len, frame.begin());
                window.multiplyWithWindowingTable (frame.data(), (size_t) hpssSize);

                fft.performRealOnlyForwardTransform (frame.data(), true);

                spectra[(size_t) f].assign (frame.begin(),
                                            frame.begin() + (size_t) hpssBins * 2);
                for (int b = 0; b < hpssBins; ++b)
                {
                    const float re = frame[(size_t) b * 2];
                    const float im = frame[(size_t) b * 2 + 1];
                    mag[(size_t) f][(size_t) b] = std::sqrt (re * re + im * im);
                }
            }
        }

        // --- Median filtering -------------------------------------------------
        // Harmonic: median across time per bin. Percussive: across frequency.
        std::vector<std::vector<float>> harm ((size_t) numFrames,
                                              std::vector<float> ((size_t) hpssBins));
        std::vector<std::vector<float>> perc ((size_t) numFrames,
                                              std::vector<float> ((size_t) hpssBins));
        {
            std::vector<float> scratch;
            scratch.reserve (medianSpan);
            const int half = medianSpan / 2;

            for (int b = 0; b < hpssBins; ++b)
            {
                if (shouldAbort && shouldAbort()) return {};
                for (int f = 0; f < numFrames; ++f)
                {
                    scratch.clear();
                    for (int k = juce::jmax (0, f - half);
                         k <= juce::jmin (numFrames - 1, f + half); ++k)
                        scratch.push_back (mag[(size_t) k][(size_t) b]);
                    harm[(size_t) f][(size_t) b] = medianOf (scratch);
                }
                progress = 0.1f + 0.35f * ((float) ch + (float) b / hpssBins) / (float) numChannels;
            }

            for (int f = 0; f < numFrames; ++f)
            {
                if (shouldAbort && shouldAbort()) return {};
                for (int b = 0; b < hpssBins; ++b)
                {
                    scratch.clear();
                    for (int k = juce::jmax (0, b - half);
                         k <= juce::jmin (hpssBins - 1, b + half); ++k)
                        scratch.push_back (mag[(size_t) f][(size_t) k]);
                    perc[(size_t) f][(size_t) b] = medianOf (scratch);
                }
                progress = 0.45f + 0.35f * ((float) ch + (float) f / numFrames) / (float) numChannels;
            }
        }

        // --- Wiener masks + synthesis (weighted overlap-add) ---------------------
        {
            std::vector<float> frame ((size_t) hpssSize * 2, 0.0f);
            std::vector<float> norm ((size_t) numSamples, 1.0e-6f);

            // Pre-compute window^2 overlap normalization.
            {
                std::vector<float> w ((size_t) hpssSize, 1.0f);
                window.multiplyWithWindowingTable (w.data(), (size_t) hpssSize);
                for (int f = 0; f < numFrames; ++f)
                {
                    const int pos = f * hpssHop;
                    for (int i = 0; i < hpssSize && pos + i < numSamples; ++i)
                        norm[(size_t) (pos + i)] += w[(size_t) i] * w[(size_t) i];
                }
            }

            for (int stemIdx = 0; stemIdx < 2; ++stemIdx)
            {
                float* dst = result[(size_t) stemIdx].audio.getWritePointer (ch);

                for (int f = 0; f < numFrames; ++f)
                {
                    if (shouldAbort && shouldAbort()) return {};

                    for (int b = 0; b < hpssBins; ++b)
                    {
                        const float h2 = harm[(size_t) f][(size_t) b] * harm[(size_t) f][(size_t) b];
                        const float p2 = perc[(size_t) f][(size_t) b] * perc[(size_t) f][(size_t) b];
                        const float maskDen = h2 + p2 + 1.0e-12f;
                        const float mask = stemIdx == 0 ? h2 / maskDen : p2 / maskDen;

                        frame[(size_t) b * 2]     = spectra[(size_t) f][(size_t) b * 2]     * mask;
                        frame[(size_t) b * 2 + 1] = spectra[(size_t) f][(size_t) b * 2 + 1] * mask;
                    }
                    std::fill (frame.begin() + (size_t) hpssBins * 2, frame.end(), 0.0f);

                    fft.performRealOnlyInverseTransform (frame.data());
                    window.multiplyWithWindowingTable (frame.data(), (size_t) hpssSize);

                    const int pos = f * hpssHop;
                    for (int i = 0; i < hpssSize && pos + i < numSamples; ++i)
                        dst[pos + i] += frame[(size_t) i];
                }

                for (int i = 0; i < numSamples; ++i)
                    dst[i] /= norm[(size_t) i];

                progress = 0.8f + 0.2f * ((float) ch + 0.5f * (float) (stemIdx + 1)) / (float) numChannels;
            }
        }
    }

    progress = 1.0f;
    return result;
}

} // namespace axiom
