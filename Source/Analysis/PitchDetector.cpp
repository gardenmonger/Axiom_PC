#include "PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace axiom
{

PitchDetector::PitchDetector (double sr, int window, float minHz, float maxHz)
    : sampleRate (sr), windowSize (window)
{
    minLag = std::max (2,              (int) (sr / maxHz));
    maxLag = std::min (windowSize / 2, (int) (sr / minHz));
    diff.resize ((size_t) maxLag + 1, 0.0f);
}

PitchDetector::Result PitchDetector::detect (const float* x) const
{
    const int half = windowSize / 2;

    // Difference function d(tau) over half the window.
    for (int tau = 0; tau <= maxLag; ++tau)
    {
        float sum = 0.0f;
        for (int i = 0; i < half; ++i)
        {
            const float d = x[i] - x[i + tau];
            sum += d * d;
        }
        diff[(size_t) tau] = sum;
    }

    // Cumulative-mean-normalized difference function (CMNDF).
    float runningSum = 0.0f;
    diff[0] = 1.0f;
    for (int tau = 1; tau <= maxLag; ++tau)
    {
        runningSum += diff[(size_t) tau];
        diff[(size_t) tau] = runningSum > 0.0f
                                 ? diff[(size_t) tau] * (float) tau / runningSum
                                 : 1.0f;
    }

    // Absolute threshold: first dip below 0.15, else global minimum.
    constexpr float threshold = 0.15f;
    int bestTau = -1;
    for (int tau = minLag; tau <= maxLag; ++tau)
    {
        if (diff[(size_t) tau] < threshold)
        {
            while (tau + 1 <= maxLag && diff[(size_t) (tau + 1)] < diff[(size_t) tau])
                ++tau;
            bestTau = tau;
            break;
        }
    }
    if (bestTau < 0)
    {
        float minVal = 1.0e9f;
        for (int tau = minLag; tau <= maxLag; ++tau)
        {
            if (diff[(size_t) tau] < minVal)
            {
                minVal  = diff[(size_t) tau];
                bestTau = tau;
            }
        }
    }
    if (bestTau <= 0)
        return {};

    // Parabolic interpolation around the minimum for sub-sample lag accuracy.
    float refinedTau = (float) bestTau;
    if (bestTau > minLag && bestTau < maxLag)
    {
        const float s0 = diff[(size_t) bestTau - 1];
        const float s1 = diff[(size_t) bestTau];
        const float s2 = diff[(size_t) bestTau + 1];
        const float denom = 2.0f * (2.0f * s1 - s0 - s2);
        if (std::abs (denom) > 1.0e-12f)
            refinedTau += (s2 - s0) / denom;
    }

    Result r;
    r.f0Hz       = (float) (sampleRate / refinedTau);
    r.confidence = std::clamp (1.0f - diff[(size_t) bestTau], 0.0f, 1.0f);
    return r;
}

} // namespace axiom
