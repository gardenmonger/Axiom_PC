/*
  ==============================================================================

    InferenceEngine.h

    Abstraction over local neural-network inference. Axiom's AI stack is
    designed so that:

      1. Models are ONNX files discovered at runtime in the user's model
         directory — updating a model NEVER requires recompiling the app.
      2. ONNX Runtime is an optional build dependency (AXIOM_ENABLE_ONNX).
         Without it, NullInferenceEngine reports "unavailable" and the
         pipeline falls back to the analytical estimator, so the product is
         fully functional on machines without the runtime.
      3. Everything is offline. No network access, no Python.

    Planned model slots (see docs/ARCHITECTURE.md):
      - "stem_separator"    : song -> vocals/drums/bass/synth/other stems
      - "patch_refiner"     : AnalysisFeatures + mel patches -> InstrumentPatch
                              parameter refinement (Neural Patch Discovery)

    Execution providers when AXIOM_ENABLE_ONNX is on: CoreML (macOS),
    DirectML (Windows), CUDA (Linux/Windows), CPU fallback everywhere.

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <vector>

#include "../Core/AppPaths.h"

namespace axiom
{

//==============================================================================
class IInferenceEngine
{
public:
    virtual ~IInferenceEngine() = default;

    /** True when the runtime is compiled in AND the named model file exists. */
    virtual bool isModelAvailable (const juce::String& modelName) const = 0;

    /** Synchronous inference: flat float tensor in, flat float tensor out.
        Call from a background thread only. Returns false on failure. */
    virtual bool run (const juce::String& modelName,
                      const std::vector<float>& input,
                      const std::vector<int64_t>& inputShape,
                      std::vector<float>& output) = 0;

    virtual juce::String getBackendName() const = 0;
};

//==============================================================================
/** Locates replaceable .onnx model files. Models live outside the binary in
    the per-user application data directory so they can be added or updated
    independently of app releases. */
class ModelRegistry
{
public:
    static juce::File getModelDirectory()
    {
        return paths::modelDirectory();
    }

    static juce::File findModel (const juce::String& modelName)
    {
        return getModelDirectory().getChildFile (modelName + ".onnx");
    }

    static juce::StringArray listInstalledModels()
    {
        juce::StringArray names;
        for (auto& f : getModelDirectory().findChildFiles (juce::File::findFiles, false, "*.onnx"))
            names.add (f.getFileNameWithoutExtension());
        return names;
    }
};

//==============================================================================
/** Fallback engine used when ONNX Runtime is not compiled in. Always reports
    models as unavailable; the reconstruction pipeline then routes everything
    through the analytical estimator. */
class NullInferenceEngine final : public IInferenceEngine
{
public:
    bool isModelAvailable (const juce::String&) const override        { return false; }
    bool run (const juce::String&, const std::vector<float>&,
              const std::vector<int64_t>&, std::vector<float>&) override { return false; }
    juce::String getBackendName() const override                       { return "Analytical DSP (no ONNX runtime)"; }
};

#if AXIOM_ENABLE_ONNX
/** ONNX Runtime backed engine. Sessions are created lazily per model and
    cached; execution provider preference: CoreML > CUDA > DirectML > CPU. */
class OnnxInferenceEngine final : public IInferenceEngine
{
public:
    OnnxInferenceEngine();
    ~OnnxInferenceEngine() override;

    bool isModelAvailable (const juce::String& modelName) const override;
    bool run (const juce::String& modelName,
              const std::vector<float>& input,
              const std::vector<int64_t>& inputShape,
              std::vector<float>& output) override;
    juce::String getBackendName() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
#endif

/** Factory: returns the best engine available in this build. */
inline std::unique_ptr<IInferenceEngine> createInferenceEngine()
{
   #if AXIOM_ENABLE_ONNX
    return std::make_unique<OnnxInferenceEngine>();
   #else
    return std::make_unique<NullInferenceEngine>();
   #endif
}

} // namespace axiom
