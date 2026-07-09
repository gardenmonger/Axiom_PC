/*
  ==============================================================================

    OnnxInferenceEngine.cpp

    ONNX Runtime backend (compiled when AXIOM_ENABLE_ONNX=1). Sessions are
    created lazily per model file and cached for the plugin's lifetime.
    Execution provider preference: CoreML when available, CPU otherwise —
    the provider only affects speed, never output quality.

    All calls happen on worker threads (analysis / separation / export);
    nothing here may be reached from the audio callback.

  ==============================================================================
*/

#include "InferenceEngine.h"

#if AXIOM_ENABLE_ONNX

#include <map>
#include <onnxruntime_cxx_api.h>

#if __has_include(<coreml_provider_factory.h>)
 #include <coreml_provider_factory.h>
 #define AXIOM_HAS_COREML 1
#else
 #define AXIOM_HAS_COREML 0
#endif

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#endif

namespace axiom
{

#if JUCE_WINDOWS
bool preloadOnnxRuntimeLibrary()
{
    static const bool loaded = []
    {
        // The build copies onnxruntime.dll next to the plugin binary (inside
        // the .vst3 bundle) and the standalone exe; hosts never search there
        // for dependent DLLs, so hand the loader an absolute path before the
        // delay-loaded first ORT call resolves by name.
        const auto bundled = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                                 .getSiblingFile ("onnxruntime.dll");

        if (bundled.existsAsFile()
             && ::LoadLibraryW (bundled.getFullPathName().toWideCharPointer()) != nullptr)
            return true;

        return ::LoadLibraryW (L"onnxruntime.dll") != nullptr;  // PATH / System32 fallback
    }();

    return loaded;
}
#endif

struct OnnxInferenceEngine::Impl
{
    Ort::Env env { ORT_LOGGING_LEVEL_WARNING, "Axiom" };
    Ort::MemoryInfo memoryInfo { Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault) };

    juce::CriticalSection lock;
    std::map<juce::String, std::unique_ptr<Ort::Session>> sessions;
    juce::String providerName { "CPU" };

    Ort::Session* getSession (const juce::String& modelName)
    {
        const juce::ScopedLock sl (lock);

        if (auto it = sessions.find (modelName); it != sessions.end())
            return it->second.get();

        const auto file = ModelRegistry::findModel (modelName);
        if (! file.existsAsFile())
            return nullptr;

        try
        {
            Ort::SessionOptions options;
            options.SetIntraOpNumThreads (juce::jmax (1, juce::SystemStats::getNumCpus() - 1));
            options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);

           #if AXIOM_HAS_COREML
            try
            {
                Ort::ThrowOnError (OrtSessionOptionsAppendExecutionProvider_CoreML (
                    options, COREML_FLAG_USE_CPU_AND_GPU));
                providerName = "CoreML";
            }
            catch (...) { providerName = "CPU"; }   // CoreML unsupported for this model
           #endif

            // Ort::Session takes ORTCHAR_T*: wchar_t on Windows, char elsewhere
           #if JUCE_WINDOWS
            auto session = std::make_unique<Ort::Session> (
                env, file.getFullPathName().toWideCharPointer(), options);
           #else
            auto session = std::make_unique<Ort::Session> (
                env, file.getFullPathName().toRawUTF8(), options);
           #endif

            auto* raw = session.get();
            sessions[modelName] = std::move (session);
            return raw;
        }
        catch (const std::exception& e)
        {
            DBG ("Axiom ONNX: failed to load " << modelName << ": " << e.what());
            return nullptr;
        }
    }
};

//==============================================================================
OnnxInferenceEngine::OnnxInferenceEngine()  : impl (std::make_unique<Impl>()) {}
OnnxInferenceEngine::~OnnxInferenceEngine() = default;

bool OnnxInferenceEngine::isModelAvailable (const juce::String& modelName) const
{
    return ModelRegistry::findModel (modelName).existsAsFile();
}

juce::String OnnxInferenceEngine::getBackendName() const
{
    return "ONNX Runtime " + juce::String (ORT_API_VERSION) + " (" + impl->providerName + ")";
}

bool OnnxInferenceEngine::run (const juce::String& modelName,
                               const std::vector<float>& input,
                               const std::vector<int64_t>& inputShape,
                               std::vector<float>& output)
{
    auto* session = impl->getSession (modelName);
    if (session == nullptr)
        return false;

    try
    {
        Ort::AllocatorWithDefaultOptions allocator;
        const auto inputName  = session->GetInputNameAllocated (0, allocator);
        const auto outputName = session->GetOutputNameAllocated (0, allocator);

        auto inputTensor = Ort::Value::CreateTensor<float> (
            impl->memoryInfo, const_cast<float*> (input.data()), input.size(),
            inputShape.data(), inputShape.size());

        const char* inNames[]  = { inputName.get() };
        const char* outNames[] = { outputName.get() };

        auto results = session->Run (Ort::RunOptions { nullptr },
                                     inNames, &inputTensor, 1, outNames, 1);
        if (results.empty() || ! results[0].IsTensor())
            return false;

        const auto info  = results[0].GetTensorTypeAndShapeInfo();
        const auto count = info.GetElementCount();
        const auto* data = results[0].GetTensorData<float>();

        output.assign (data, data + count);
        return true;
    }
    catch (const std::exception& e)
    {
        DBG ("Axiom ONNX: inference failed for " << modelName << ": " << e.what());
        return false;
    }
}

} // namespace axiom

#endif // AXIOM_ENABLE_ONNX
