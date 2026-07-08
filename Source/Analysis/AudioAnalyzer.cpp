#include "AudioAnalyzer.h"
#include "PitchDetector.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace axiom
{

namespace
{
    constexpr int fftOrder = 12;                 // 4096-point STFT
    constexpr int fftSize  = 1 << fftOrder;
    constexpr int numBins  = fftSize / 2;
    constexpr int maxHarmonics = 64;             // full ladder for spectral resynthesis

    float median (std::vector<float> v)
    {
        if (v.empty()) return 0.0f;
        const auto mid = v.size() / 2;
        std::nth_element (v.begin(), v.begin() + (long) mid, v.end());
        return v[mid];
    }

    /** Parabolic peak interpolation on a magnitude spectrum. Returns refined
        {bin, magnitude} around an integer candidate bin. */
    std::pair<float, float> refinePeak (const std::vector<float>& mag, int bin)
    {
        if (bin <= 0 || bin >= (int) mag.size() - 1)
            return { (float) bin, mag[(size_t) std::clamp (bin, 0, (int) mag.size() - 1)] };

        const float a = mag[(size_t) bin - 1], b = mag[(size_t) bin], c = mag[(size_t) bin + 1];
        const float denom = a - 2.0f * b + c;
        float offset = 0.0f;
        if (std::abs (denom) > 1.0e-12f)
            offset = std::clamp (0.5f * (a - c) / denom, -0.5f, 0.5f);
        const float peakMag = b - 0.25f * (a - c) * offset;
        return { (float) bin + offset, peakMag };
    }
}

//==============================================================================
AnalysisFeatures AudioAnalyzer::analyze (const juce::AudioBuffer<float>& audio,
                                         double sr,
                                         const std::function<bool()>& shouldAbort)
{
    AnalysisFeatures out;
    out.sampleRate    = sr;
    out.numChannels   = audio.getNumChannels();
    out.lengthSeconds = audio.getNumSamples() / sr;

    const int numSamples = audio.getNumSamples();
    if (numSamples < fftSize || audio.getNumChannels() < 1 || sr <= 0.0)
        return out;

    // Mixdown to a peak-normalized mono working copy: every feature below is
    // level-invariant, and normalization keeps thresholds stable.
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
        return out;                              // silence — nothing to learn
    for (auto& s : mono)
        s /= peak;

    auto aborted = [&shouldAbort] { return shouldAbort && shouldAbort(); };

    analyzeEnvelope (mono.data(), numSamples, sr, out);
    if (aborted()) return out;

    std::vector<float> pitchTrackCents;
    int hop = 512;
    analyzePitch (mono.data(), numSamples, sr, out, pitchTrackCents, hop);
    if (aborted()) return out;

    if (out.f0Hz > 0.0f)
        analyzeSpectrum (mono.data(), numSamples, sr, out);
    if (aborted()) return out;

    analyzeVibrato (pitchTrackCents, sr / hop, out);
    analyzeStereo  (audio, out);
    return out;
}

//==============================================================================
void AudioAnalyzer::analyzePitch (const float* mono, int numSamples, double sr,
                                  AnalysisFeatures& out,
                                  std::vector<float>& pitchTrackCentsOut, int& hopOut)
{
    const int window = 2048;
    const int hop    = 512;
    hopOut = hop;

    PitchDetector detector (sr, window);

    // Skip the attack transient — pitch there is unreliable.
    const int start = std::min ((int) (0.03 * sr), std::max (0, numSamples - window - 1));

    std::vector<float> confidentF0s;
    std::vector<std::pair<int, float>> track;    // frame index -> f0

    for (int pos = start; pos + window <= numSamples; pos += hop)
    {
        const auto r = detector.detect (mono + pos);
        track.emplace_back ((int) track.size(), r.confidence > 0.5f ? r.f0Hz : 0.0f);
        if (r.confidence > 0.5f && r.f0Hz > 20.0f && r.f0Hz < 4000.0f)
            confidentF0s.push_back (r.f0Hz);
    }

    if (confidentF0s.size() < 3)
    {
        out.f0Hz = 0.0f;
        out.pitchConfidence = 0.0f;
        return;
    }

    const float f0 = median (confidentF0s);
    out.f0Hz            = f0;
    out.rootMidiNote    = std::clamp ((int) std::lround (69.0 + 12.0 * std::log2 (f0 / 440.0)), 0, 127);
    out.pitchConfidence = std::clamp ((float) confidentF0s.size() / (float) std::max<size_t> (track.size(), 1), 0.0f, 1.0f);

    // Cents-relative pitch track (only confident frames) for vibrato analysis,
    // and stability = inverse spread of that track.
    pitchTrackCentsOut.clear();
    for (auto& [idx, hz] : track)
        pitchTrackCentsOut.push_back (hz > 0.0f ? 1200.0f * (float) std::log2 (hz / f0) : 0.0f);

    std::vector<float> confidentCents;
    for (auto c : pitchTrackCentsOut)
        if (c != 0.0f) confidentCents.push_back (c);

    if (confidentCents.size() > 2)
    {
        const float mean = std::accumulate (confidentCents.begin(), confidentCents.end(), 0.0f)
                           / (float) confidentCents.size();
        float var = 0.0f;
        for (auto c : confidentCents) var += (c - mean) * (c - mean);
        const float stdCents = std::sqrt (var / (float) confidentCents.size());
        out.pitchStability = std::clamp (1.0f - stdCents / 50.0f, 0.0f, 1.0f);
    }
}

//==============================================================================
void AudioAnalyzer::analyzeSpectrum (const float* mono, int numSamples, double sr,
                                     AnalysisFeatures& out)
{
    juce::dsp::FFT fft (fftOrder);
    juce::dsp::WindowingFunction<float> window ((size_t) fftSize,
                                                juce::dsp::WindowingFunction<float>::hann);

    // Average the power spectrum over the sustained body of the sound
    // (10%..85% of the file) so the attack transient doesn't smear it.
    const int begin = (int) (0.10 * numSamples);
    const int end   = std::max (begin + fftSize, (int) (0.85 * numSamples));
    const int hop   = fftSize / 4;

    std::vector<float> avgPower ((size_t) numBins, 0.0f);
    std::vector<float> frame ((size_t) fftSize * 2, 0.0f);
    std::vector<float> centroidTrack;
    int frameCount = 0;

    for (int pos = begin; pos + fftSize <= std::min (end, numSamples); pos += hop)
    {
        std::copy (mono + pos, mono + pos + fftSize, frame.begin());
        std::fill (frame.begin() + fftSize, frame.end(), 0.0f);
        window.multiplyWithWindowingTable (frame.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (frame.data());

        double num = 0.0, den = 0.0;
        for (int b = 1; b < numBins; ++b)
        {
            const float p = frame[(size_t) b] * frame[(size_t) b];
            avgPower[(size_t) b] += p;
            const double hz = b * sr / fftSize;
            num += hz * p;
            den += p;
        }
        centroidTrack.push_back (den > 0.0 ? (float) (num / den) : 0.0f);
        ++frameCount;
    }
    if (frameCount == 0)
        return;
    for (auto& p : avgPower)
        p /= (float) frameCount;

    std::vector<float> mag ((size_t) numBins, 0.0f);
    for (int b = 0; b < numBins; ++b)
        mag[(size_t) b] = std::sqrt (avgPower[(size_t) b]);

    const double binHz = sr / fftSize;

    // --- Centroid / flatness / rolloff (20 Hz .. 16 kHz band) ---------------
    {
        const int loBin = std::max (1, (int) (20.0 / binHz));
        const int hiBin = std::min (numBins - 1, (int) (16000.0 / binHz));

        double num = 0.0, total = 0.0, logSum = 0.0;
        int count = 0;
        for (int b = loBin; b <= hiBin; ++b)
        {
            const double p = avgPower[(size_t) b];
            num    += (b * binHz) * p;
            total  += p;
            logSum += std::log (p + 1.0e-20);
            ++count;
        }
        out.spectralCentroidHz = total > 0.0 ? (float) (num / total) : 0.0f;
        const double geoMean   = std::exp (logSum / std::max (count, 1));
        const double arithMean = total / std::max (count, 1);
        out.spectralFlatness   = arithMean > 0.0 ? (float) std::clamp (geoMean / arithMean, 0.0, 1.0) : 1.0f;

        double cum = 0.0;
        out.spectralRolloffHz = (float) (hiBin * binHz);
        for (int b = loBin; b <= hiBin; ++b)
        {
            cum += avgPower[(size_t) b];
            if (cum >= 0.95 * total)
            {
                out.spectralRolloffHz = (float) (b * binHz);
                break;
            }
        }
    }

    // --- Harmonic profile ----------------------------------------------------
    const float f0 = out.f0Hz;
    std::vector<float> harmMag;
    double harmonicEnergy = 0.0, totalEnergy = 0.0, inharmSum = 0.0;
    int inharmCount = 0;

    for (int b = 1; b < numBins; ++b)
        totalEnergy += avgPower[(size_t) b];

    for (int k = 1; k <= maxHarmonics; ++k)
    {
        const double targetHz  = (double) k * f0;
        const int    targetBin = (int) std::lround (targetHz / binHz);
        if (targetBin >= numBins - 4)
            break;

        int bestBin = targetBin;
        for (int b = std::max (1, targetBin - 3); b <= std::min (numBins - 2, targetBin + 3); ++b)
            if (mag[(size_t) b] > mag[(size_t) bestBin])
                bestBin = b;

        const auto [refBin, refMag] = refinePeak (mag, bestBin);
        harmMag.push_back (refMag);

        for (int b = std::max (1, bestBin - 2); b <= std::min (numBins - 1, bestBin + 2); ++b)
            harmonicEnergy += avgPower[(size_t) b];

        if (k > 1)
        {
            inharmSum += std::abs (refBin * binHz - targetHz) / (0.5 * f0);
            ++inharmCount;
        }
    }

    if (harmMag.empty() || harmMag[0] <= 0.0f)
        return;

    const float maxHarm = *std::max_element (harmMag.begin(), harmMag.end());
    out.harmonicDb.clear();
    for (auto m : harmMag)
        out.harmonicDb.push_back (20.0f * std::log10 (std::max (m, 1.0e-9f) / maxHarm));

    out.noisiness     = totalEnergy > 0.0
                          ? (float) std::clamp (1.0 - harmonicEnergy / totalEnergy, 0.0, 1.0) : 1.0f;
    out.inharmonicity = inharmCount > 0 ? (float) std::clamp (inharmSum / inharmCount, 0.0, 1.0) : 0.0f;

    // Odd/even balance (power domain, harmonics 1..N).
    {
        double odd = 0.0, even = 0.0;
        for (size_t k = 0; k < harmMag.size(); ++k)
        {
            const double p = (double) harmMag[k] * harmMag[k];
            if ((k + 1) % 2 == 1) odd += p; else even += p;
        }
        out.oddEvenRatio = (odd + even) > 0.0 ? (float) (odd / (odd + even)) : 0.5f;
    }

    // Rolloff slope: least-squares fit of harmonic dB vs log2(harmonic index).
    {
        int n = 0; double sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (size_t k = 0; k < harmMag.size(); ++k)
        {
            if (out.harmonicDb[k] < -70.0f) continue;   // below noise floor
            const double x = std::log2 ((double) k + 1.0);
            const double y = out.harmonicDb[k];
            sx += x; sy += y; sxx += x * x; sxy += x * y; ++n;
        }
        if (n > 2)
        {
            const double denom = n * sxx - sx * sx;
            if (std::abs (denom) > 1.0e-9)
                out.slopeDbPerHarm = (float) std::clamp ((n * sxy - sx * sy) / denom, -60.0, 12.0);
        }
    }

    // Unison-detune estimate: width of the fundamental's spectral peak beyond
    // the Hann-window mainlobe indicates detuned stacked oscillators.
    {
        const int f0Bin = (int) std::lround (f0 / binHz);
        if (f0Bin > 2 && f0Bin < numBins - 2)
        {
            int bestBin = f0Bin;
            for (int b = f0Bin - 2; b <= f0Bin + 2; ++b)
                if (mag[(size_t) b] > mag[(size_t) bestBin]) bestBin = b;

            const float halfMag = mag[(size_t) bestBin] * 0.5f;   // -6 dB point
            int lo = bestBin, hi = bestBin;
            while (lo > 1           && mag[(size_t) lo] > halfMag) --lo;
            while (hi < numBins - 2 && mag[(size_t) hi] > halfMag) ++hi;

            const float widthBins  = (float) (hi - lo);
            const float excessBins = std::max (0.0f, widthBins - 3.0f);  // Hann -6 dB lobe ~= 3 bins
            const float excessHz   = excessBins * (float) binHz;
            out.detuneCentsEst = std::clamp (0.5f * 1731.0f * excessHz / f0, 0.0f, 60.0f);
        }
    }

    // Periodic brightness movement -> filter-LFO candidate. Autocorrelate the
    // detrended per-frame centroid track.
    if (centroidTrack.size() > 16)
    {
        const double frameRate = sr / hop;
        const float meanC = std::accumulate (centroidTrack.begin(), centroidTrack.end(), 0.0f)
                            / (float) centroidTrack.size();
        std::vector<float> c;
        for (auto v : centroidTrack) c.push_back (v - meanC);

        float e0 = 1.0e-12f;
        for (auto v : c) e0 += v * v;

        const int minLag = std::max (1, (int) (frameRate / 12.0));   // <= 12 Hz
        const int maxLag = std::min ((int) c.size() / 2, (int) (frameRate / 0.3));
        float bestCorr = 0.0f; int bestLag = 0;
        for (int lag = minLag; lag < maxLag; ++lag)
        {
            float acc = 0.0f;
            for (size_t i = 0; i + (size_t) lag < c.size(); ++i)
                acc += c[i] * c[i + (size_t) lag];
            const float norm = acc / e0;
            if (norm > bestCorr) { bestCorr = norm; bestLag = lag; }
        }
        if (bestCorr > 0.35f && bestLag > 0 && meanC > 0.0f)
        {
            out.centroidModRateHz = (float) (frameRate / bestLag);
            float var = 0.0f;
            for (auto v : c) var += v * v;
            out.centroidModDepth = std::clamp (std::sqrt (var / (float) c.size()) / meanC, 0.0f, 1.0f);
        }
    }
}

//==============================================================================
void AudioAnalyzer::analyzeEnvelope (const float* mono, int numSamples, double sr,
                                     AnalysisFeatures& out)
{
    // RMS contour, 5 ms window / 2.5 ms hop.
    const int win = std::max (8, (int) (0.005 * sr));
    const int hop = std::max (4, win / 2);

    std::vector<float> env;
    for (int pos = 0; pos + win <= numSamples; pos += hop)
    {
        float acc = 0.0f;
        for (int i = 0; i < win; ++i)
            acc += mono[pos + i] * mono[pos + i];
        env.push_back (std::sqrt (acc / (float) win));
    }
    if (env.size() < 4)
        return;

    // Light smoothing (3-tap) to stop single-cycle ripple from faking segments.
    for (size_t i = 1; i + 1 < env.size(); ++i)
        env[i] = (env[i - 1] + env[i] + env[i + 1]) / 3.0f;

    const auto peakIt  = std::max_element (env.begin(), env.end());
    const float peak   = *peakIt;
    const int  peakIdx = (int) std::distance (env.begin(), peakIt);
    if (peak <= 0.0f)
        return;

    const double framesPerSec = sr / hop;
    auto toSec = [framesPerSec] (int frames) { return (float) (frames / framesPerSec); };

    // Attack: 10% -> 90% of peak.
    int i10 = 0, i90 = peakIdx;
    for (int i = 0; i <= peakIdx; ++i)               { if (env[(size_t) i] >= 0.1f * peak) { i10 = i; break; } }
    for (int i = i10; i <= peakIdx; ++i)             { if (env[(size_t) i] >= 0.9f * peak) { i90 = i; break; } }
    out.attackSec = std::clamp (toSec (std::max (1, i90 - i10)), 0.001f, 4.0f);

    // Transient sharpness: how much of the peak arrives in the first 30 ms.
    {
        const int firstFrames = std::min ((int) env.size(), (int) (0.030 * framesPerSec));
        float early = 0.0f;
        for (int i = 0; i < firstFrames; ++i)
            early = std::max (early, env[(size_t) i]);
        out.transientSharpness = std::clamp (early / peak, 0.0f, 1.0f);
    }

    // Sustain: median level of the 50%..85% region (post-peak). A level that
    // holds above 15% of peak marks a sustained (pad/lead) source; below that
    // it's a one-shot.
    const int susBegin = std::max (peakIdx + 1, (int) (0.50 * env.size()));
    const int susEnd   = (int) (0.85 * env.size());
    float sustain = 0.0f;
    if (susEnd > susBegin + 2)
    {
        std::vector<float> region (env.begin() + susBegin, env.begin() + susEnd);
        sustain = median (region) / peak;
    }
    out.isSustained = sustain > 0.15f;
    out.sustainLvl  = out.isSustained ? std::clamp (sustain, 0.0f, 1.0f) : 0.0f;

    // Decay: peak -> sustain plateau (sustained) or peak -> 10% (one-shot).
    {
        const float target = out.isSustained ? std::min (0.98f, sustain * 1.10f) * peak
                                             : 0.10f * peak;
        int decayEnd = (int) env.size() - 1;
        for (int i = peakIdx; i < (int) env.size(); ++i)
        {
            if (env[(size_t) i] <= target) { decayEnd = i; break; }
        }
        out.decaySec = std::clamp (toSec (std::max (1, decayEnd - peakIdx)), 0.005f, 8.0f);
    }

    // Release: time the tail takes to fall from the sustain level to 5% peak.
    {
        const float refLvl = out.isSustained ? sustain * peak * 0.7f : 0.5f * peak;
        int relStart = -1;
        for (int i = (int) env.size() - 1; i >= peakIdx; --i)
        {
            if (env[(size_t) i] >= refLvl) { relStart = i; break; }
        }
        if (relStart >= 0)
        {
            int relEnd = (int) env.size() - 1;
            for (int i = relStart; i < (int) env.size(); ++i)
                if (env[(size_t) i] <= 0.05f * peak) { relEnd = i; break; }

            const bool tailReachedSilence = env.back() <= 0.06f * peak;
            out.releaseSec = tailReachedSilence
                                 ? std::clamp (toSec (std::max (1, relEnd - relStart)), 0.01f, 8.0f)
                                 : 0.3f;   // sample was cut before the tail ended
        }
    }

    // Dynamic range: peak vs the quiet floor of the contour.
    {
        std::vector<float> sorted (env);
        std::sort (sorted.begin(), sorted.end());
        const float floorLvl = sorted[sorted.size() / 10] + 1.0e-6f;   // 10th percentile
        out.dynamicRangeDb = std::clamp (20.0f * std::log10 (peak / floorLvl), 0.0f, 96.0f);
    }
}

//==============================================================================
void AudioAnalyzer::analyzeVibrato (const std::vector<float>& cents, double frameRate,
                                    AnalysisFeatures& out)
{
    // Collect the confident (non-zero) contiguous samples, detrend, then
    // autocorrelate in the 2..10 Hz vibrato band.
    std::vector<float> v;
    for (auto c : cents)
        if (c != 0.0f) v.push_back (c);
    if (v.size() < 24)
        return;

    const float mean = std::accumulate (v.begin(), v.end(), 0.0f) / (float) v.size();
    for (auto& x : v) x -= mean;

    float e0 = 1.0e-12f;
    for (auto x : v) e0 += x * x;

    const int minLag = std::max (1, (int) (frameRate / 10.0));
    const int maxLag = std::min ((int) v.size() / 2, (int) (frameRate / 2.0));
    float bestCorr = 0.0f; int bestLag = 0;
    for (int lag = minLag; lag < maxLag; ++lag)
    {
        float acc = 0.0f;
        for (size_t i = 0; i + (size_t) lag < v.size(); ++i)
            acc += v[i] * v[i + (size_t) lag];
        const float norm = acc / e0;
        if (norm > bestCorr) { bestCorr = norm; bestLag = lag; }
    }

    if (bestCorr > 0.35f && bestLag > 0)
    {
        float var = 0.0f;
        for (auto x : v) var += x * x;
        const float depth = std::sqrt (2.0f * var / (float) v.size());  // ~sine amplitude

        if (depth > 2.0f)   // ignore sub-2-cent jitter
        {
            out.vibratoRateHz     = (float) (frameRate / bestLag);
            out.vibratoDepthCents = std::clamp (depth, 0.0f, 200.0f);
        }
    }
}

//==============================================================================
void AudioAnalyzer::analyzeStereo (const juce::AudioBuffer<float>& audio, AnalysisFeatures& out)
{
    if (audio.getNumChannels() < 2)
    {
        out.stereoWidth = 0.0f;
        return;
    }

    const float* l = audio.getReadPointer (0);
    const float* r = audio.getReadPointer (1);
    const int n = audio.getNumSamples();

    double sl = 0, sr2 = 0, slr = 0;
    for (int i = 0; i < n; ++i)
    {
        sl  += (double) l[i] * l[i];
        sr2 += (double) r[i] * r[i];
        slr += (double) l[i] * r[i];
    }
    const double denom = std::sqrt (sl * sr2);
    const double corr  = denom > 1.0e-12 ? slr / denom : 1.0;
    out.stereoWidth = (float) std::clamp (1.0 - std::max (0.0, corr), 0.0, 1.0);
}

} // namespace axiom
