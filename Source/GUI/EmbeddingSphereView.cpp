#include "EmbeddingSphereView.h"
#include "AxiomLookAndFeel.h"

namespace axiom
{

namespace
{
    // Galaxy palette.
    const juce::Colour spaceDeep   { 0xff05060e };
    const juce::Colour spaceMid    { 0xff0b0d1f };
    const juce::Colour nebulaViolet{ 0x2b6a4bd8 };
    const juce::Colour nebulaBlue  { 0x22325bd8 };
    const juce::Colour nebulaTeal  { 0x1a2fbfa8 };

    const juce::Colour starGold    { 0xffffd27a };   // pitch
    const juce::Colour starCyan    { 0xff7ad7ff };   // harmonics
    const juce::Colour starViolet  { 0xffb48cff };   // timbre
    const juce::Colour starMagenta { 0xffff7ad7 };   // envelope
    const juce::Colour starTeal    { 0xff7affd0 };   // modulation / phase / stereo
    const juce::Colour starDust    { 0xffdfe6ff };   // noise dust

    enum Groups { gPitch = 0, gHarmonic, gTimbre, gEnvelope, gModulation, gNoise, gAmbient };
}

//==============================================================================
EmbeddingSphereView::EmbeddingSphereView()
{
    setInterceptsMouseClicks (false, false);
    rebuildIdleField();
    startTimerHz (60);
}

void EmbeddingSphereView::visibilityChanged()
{
    if (isVisible()) startTimerHz (60);
    else             stopTimer();
}

//==============================================================================
float EmbeddingSphereView::hash01 (uint32_t x) noexcept
{
    x ^= x >> 16;  x *= 0x7feb352dU;
    x ^= x >> 15;  x *= 0x846ca68bU;
    x ^= x >> 16;
    return (float) (x & 0xffffff) / (float) 0x1000000;
}

void EmbeddingSphereView::addParticle (int group, float value01, juce::Colour colour,
                                       float baseSize, uint32_t seed)
{
    Particle p;
    // Deterministic placement: same feature -> same star, value nudges the
    // latitude so the constellation visibly reshapes with the sound.
    p.theta  = hash01 (seed * 2654435761U + 1) * juce::MathConstants<float>::twoPi;
    p.phi    = std::acos (juce::jlimit (-1.0f, 1.0f,
                 2.0f * (0.15f + 0.7f * hash01 (seed * 40503U + 7)) - 1.0f
                 + 0.25f * (value01 - 0.5f)));
    p.radius = 0.55f + 0.45f * hash01 (seed * 9176U + 3);
    p.size   = baseSize * (0.5f + 1.1f * juce::jlimit (0.0f, 1.0f, value01));
    p.twinkleRate  = 0.4f + 2.2f * hash01 (seed * 118057U + 11);
    p.twinklePhase = hash01 (seed * 51329U + 17) * juce::MathConstants<float>::twoPi;
    p.colour = colour;
    p.group  = group;
    particles.push_back (p);
}

void EmbeddingSphereView::rebuildIdleField()
{
    particles.clear();
    edges.clear();
    for (uint32_t i = 0; i < 46; ++i)
        addParticle (gAmbient, 0.25f + 0.4f * hash01 (i * 977U + 5),
                     starDust.withAlpha (0.5f), 1.6f, i + 991U);
    hasFeatures = false;
}

//==============================================================================
void EmbeddingSphereView::setFeatures (const AnalysisFeatures& f, const InstrumentPatch& patch)
{
    particles.clear();
    edges.clear();

    uint32_t seed = 1;
    auto next = [&seed] { return seed += 0x9e3779b9U; };

    // Ambient dust bed (always present — the void isn't empty).
    for (int i = 0; i < 34; ++i)
        addParticle (gAmbient, 0.2f + 0.3f * hash01 ((uint32_t) i * 31337U),
                     starDust.withAlpha (0.35f), 1.4f, next());

    // The pitch star — the constellation's sun.
    addParticle (gPitch, 0.5f + 0.5f * f.pitchConfidence, starGold,
                 5.0f + 3.0f * f.pitchConfidence, next());

    // Harmonic cluster: one star per detected harmonic, brightness from level.
    for (size_t k = 0; k < f.harmonicDb.size(); ++k)
    {
        const float level01 = juce::jlimit (0.0f, 1.0f, (f.harmonicDb[k] + 60.0f) / 60.0f);
        if (level01 <= 0.02f) continue;
        addParticle (gHarmonic, level01,
                     starCyan.interpolatedWith (starViolet, (float) k / 24.0f),
                     3.2f, next());
    }

    // Timbre points.
    addParticle (gTimbre, juce::jlimit (0.0f, 1.0f, f.spectralCentroidHz / 8000.0f), starViolet, 3.4f, next());
    addParticle (gTimbre, f.spectralFlatness,                                        starViolet, 3.0f, next());
    addParticle (gTimbre, juce::jlimit (0.0f, 1.0f, f.spectralRolloffHz / 16000.0f), starViolet, 3.0f, next());
    addParticle (gTimbre, f.oddEvenRatio,                                            starViolet, 2.8f, next());
    addParticle (gTimbre, juce::jlimit (0.0f, 1.0f, f.inharmonicity * 3.0f),         starViolet, 2.8f, next());

    // Envelope nodes (A / D / S / R + transient).
    addParticle (gEnvelope, juce::jlimit (0.0f, 1.0f, f.attackSec / 2.0f),  starMagenta, 3.2f, next());
    addParticle (gEnvelope, juce::jlimit (0.0f, 1.0f, f.decaySec / 4.0f),   starMagenta, 3.0f, next());
    addParticle (gEnvelope, f.sustainLvl,                                   starMagenta, 3.0f, next());
    addParticle (gEnvelope, juce::jlimit (0.0f, 1.0f, f.releaseSec / 4.0f), starMagenta, 3.0f, next());
    addParticle (gEnvelope, f.transientSharpness,                           starMagenta, 2.6f, next());

    // Modulation / phase / stereo.
    addParticle (gModulation, juce::jlimit (0.0f, 1.0f, f.vibratoDepthCents / 50.0f), starTeal, 3.0f, next());
    addParticle (gModulation, juce::jlimit (0.0f, 1.0f, f.centroidModDepth * 3.0f),   starTeal, 2.8f, next());
    addParticle (gModulation, juce::jlimit (0.0f, 1.0f, f.detuneCentsEst / 40.0f),    starTeal, 2.8f, next());
    addParticle (gModulation, f.stereoWidth,                                          starTeal, 2.8f, next());
    addParticle (gModulation, juce::jlimit (0.0f, 1.0f, patch.character.movement),    starTeal, 2.6f, next());

    // Noise dust: density follows noisiness.
    const int dustCount = 6 + (int) (f.noisiness * 30.0f);
    for (int i = 0; i < dustCount; ++i)
        addParticle (gNoise, 0.15f + 0.35f * hash01 ((uint32_t) i * 7907U),
                     starDust.withAlpha (0.55f), 1.6f, next());

    buildEdges();
    hasFeatures = true;
}

void EmbeddingSphereView::clearFeatures()
{
    rebuildIdleField();
}

void EmbeddingSphereView::buildEdges()
{
    // Synapses: connect each non-ambient particle to its nearest neighbour in
    // the same group and to the pitch star (hub-and-cluster brain topology).
    edges.clear();
    int pitchIdx = -1;
    for (int i = 0; i < (int) particles.size(); ++i)
        if (particles[(size_t) i].group == gPitch) { pitchIdx = i; break; }

    auto dist3 = [this] (int a, int b)
    {
        const auto& p = particles[(size_t) a];
        const auto& q = particles[(size_t) b];
        const float ax = p.radius * std::sin (p.phi) * std::cos (p.theta);
        const float ay = p.radius * std::cos (p.phi);
        const float az = p.radius * std::sin (p.phi) * std::sin (p.theta);
        const float bx = q.radius * std::sin (q.phi) * std::cos (q.theta);
        const float by = q.radius * std::cos (q.phi);
        const float bz = q.radius * std::sin (q.phi) * std::sin (q.theta);
        return (ax - bx) * (ax - bx) + (ay - by) * (ay - by) + (az - bz) * (az - bz);
    };

    for (int i = 0; i < (int) particles.size(); ++i)
    {
        const auto& p = particles[(size_t) i];
        if (p.group == gAmbient || p.group == gNoise || p.group == gPitch)
            continue;

        int best = -1;
        float bestDist = 1.0e9f;
        for (int j = 0; j < (int) particles.size(); ++j)
        {
            if (j == i || particles[(size_t) j].group != p.group) continue;
            const float d = dist3 (i, j);
            if (d < bestDist) { bestDist = d; best = j; }
        }
        if (best >= 0)
            edges.push_back ({ i, best, 0.12f });
        if (pitchIdx >= 0 && hash01 ((uint32_t) i * 2246822519U) > 0.45f)
            edges.push_back ({ i, pitchIdx, 0.06f });
    }
}

//==============================================================================
void EmbeddingSphereView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);
    AxiomLookAndFeel::drawPanel (g, bounds, metrics::radiusCard, spaceMid);

    g.saveState();
    juce::Path clip;
    clip.addRoundedRectangle (bounds, metrics::radiusCard);
    g.reduceClipRegion (clip);

    // Deep-space gradient + nebulae.
    const auto centre = bounds.getCentre();
    g.setGradientFill (juce::ColourGradient (spaceMid, centre.x, centre.y,
                                             spaceDeep, bounds.getX(), bounds.getY(), true));
    g.fillRect (bounds);

    const float R = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.38f;
    auto nebula = [&g, &centre, R] (juce::Colour c, float dx, float dy, float scale)
    {
        const auto pos = centre.translated (dx * R, dy * R);
        g.setGradientFill (juce::ColourGradient (c, pos.x, pos.y,
                                                 c.withAlpha (0.0f),
                                                 pos.x + R * scale, pos.y + R * scale, true));
        g.fillEllipse (pos.x - R * scale, pos.y - R * scale, R * scale * 2.0f, R * scale * 2.0f);
    };
    nebula (nebulaViolet, -0.55f, -0.35f, 1.5f);
    nebula (nebulaBlue,    0.65f,  0.45f, 1.3f);
    nebula (nebulaTeal,    0.15f, -0.65f, 1.0f);

    // Core nucleus: slow 3 s breathing glow — Neural Blue while idle,
    // stable gold once an instrument is ready (SG Neural Core states).
    {
        const float breath = 0.5f + 0.5f * std::sin (phase * 2.094f);   // ~3 s cycle
        const auto  coreColour = hasFeatures ? starGold : starCyan;
        const float coreR = R * (hasFeatures ? 0.30f : 0.24f) * (0.85f + 0.15f * breath);
        g.setGradientFill (juce::ColourGradient (
            coreColour.withAlpha (hasFeatures ? 0.16f : 0.10f), centre.x, centre.y,
            coreColour.withAlpha (0.0f), centre.x + coreR, centre.y + coreR, true));
        g.fillEllipse (centre.x - coreR, centre.y - coreR, coreR * 2.0f, coreR * 2.0f);
    }

    // 3-D -> 2-D: slow Y rotation + slight tilt, perspective by depth.
    const float rotY = phase * 0.22f;
    const float tilt = 0.42f;

    struct Projected { float x, y, z, size, twinkle; juce::Colour colour; };
    std::vector<Projected> proj (particles.size());

    for (size_t i = 0; i < particles.size(); ++i)
    {
        const auto& p = particles[i];
        float x = p.radius * std::sin (p.phi) * std::cos (p.theta + rotY);
        float z = p.radius * std::sin (p.phi) * std::sin (p.theta + rotY);
        float y = p.radius * std::cos (p.phi);

        const float y2 = y * std::cos (tilt) - z * std::sin (tilt);
        const float z2 = y * std::sin (tilt) + z * std::cos (tilt);

        const float persp = 1.0f / (1.6f - z2 * 0.55f);
        proj[i] = { centre.x + x * R * 1.35f * persp,
                    centre.y + y2 * R * 1.1f * persp,
                    z2,
                    p.size * persp,
                    0.72f + 0.28f * std::sin (phase * p.twinkleRate
                                               * juce::MathConstants<float>::twoPi
                                               + p.twinklePhase),
                    p.colour };
    }

    // Synapse lines behind the stars.
    for (const auto& e : edges)
    {
        const auto& a = proj[(size_t) e.a];
        const auto& b = proj[(size_t) e.b];
        const float depthFade = juce::jlimit (0.2f, 1.0f, 0.6f + 0.4f * (a.z + b.z) * 0.5f);
        g.setColour (starCyan.withAlpha (e.alpha * depthFade
                                         * (0.7f + 0.3f * std::sin (phase * 1.7f + (float) e.a))));
        g.drawLine (a.x, a.y, b.x, b.y, 0.6f);
    }

    // Stars: soft halo + hot core, alpha by depth and twinkle.
    for (const auto& s : proj)
    {
        const float depthAlpha = juce::jlimit (0.15f, 1.0f, 0.55f + 0.45f * s.z);
        const float alpha = depthAlpha * s.twinkle;

        g.setColour (s.colour.withAlpha (alpha * 0.16f));
        g.fillEllipse (s.x - s.size * 2.6f, s.y - s.size * 2.6f, s.size * 5.2f, s.size * 5.2f);
        g.setColour (s.colour.withAlpha (alpha * 0.5f));
        g.fillEllipse (s.x - s.size * 1.2f, s.y - s.size * 1.2f, s.size * 2.4f, s.size * 2.4f);
        g.setColour (s.colour.brighter (0.4f).withAlpha (alpha));
        g.fillEllipse (s.x - s.size * 0.5f, s.y - s.size * 0.5f, s.size, s.size);
    }

    // Caption.
    g.setColour (palette::textDim.withAlpha (0.9f));
    g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened());
    g.drawText ("NEURAL CORE", bounds.reduced (14.0f, 10.0f),
                juce::Justification::topLeft);
    if (! hasFeatures)
    {
        g.setColour (palette::textDim.withAlpha (0.6f));
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText ("import a sound to grow its mind",
                    bounds, juce::Justification::centred);
    }

    g.restoreState();
}

} // namespace axiom
