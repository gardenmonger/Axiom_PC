/*
  ==============================================================================

    EmbeddingSphereView.h

    The Neural Embedding Sphere — "the instrument's brain".

    A slowly rotating 3-D constellation where every particle is one learned
    feature of the analyzed sound: the pitch star, the 24-harmonic cluster,
    timbre points, envelope nodes, modulation points and a noise dust field.
    Particle position is a deterministic function of the feature (same sound
    -> same constellation), size/brightness follow feature magnitude, and
    faint synapse lines link related features. Galaxy palette: deep-space
    indigo background with nebula tints; gold / cyan / violet / magenta /
    teal stars.

    Pure JUCE software rendering at 60 FPS (~150 particles — cheap). The
    view is passive: setFeatures() after analysis, clearFeatures() on new
    import. An idle dust field plays before any sound is learned.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "../Core/AnalysisFeatures.h"
#include "../Core/InstrumentPatch.h"

namespace axiom
{

class EmbeddingSphereView final : public juce::Component,
                                  private juce::Timer
{
public:
    EmbeddingSphereView();

    void setFeatures (const AnalysisFeatures& features, const InstrumentPatch& patch);
    void clearFeatures();

    void paint (juce::Graphics&) override;
    void visibilityChanged() override;

private:
    void timerCallback() override   { phase += 1.0f / 60.0f; repaint(); }

    struct Particle
    {
        float theta = 0.0f, phi = 0.0f;   // spherical position
        float radius = 1.0f;              // 0..1 of sphere radius
        float size = 2.0f;                // px at z = 0
        float twinkleRate = 1.0f, twinklePhase = 0.0f;
        juce::Colour colour;
        int   group = 0;                  // for synapse linking
    };

    struct Edge { int a = 0, b = 0; float alpha = 0.1f; };

    void rebuildIdleField();
    void addParticle (int group, float value01, juce::Colour colour, float baseSize,
                      uint32_t seed);
    void buildEdges();

    static float hash01 (uint32_t x) noexcept;

    std::vector<Particle> particles;
    std::vector<Edge>     edges;
    bool  hasFeatures = false;
    float phase = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EmbeddingSphereView)
};

} // namespace axiom
