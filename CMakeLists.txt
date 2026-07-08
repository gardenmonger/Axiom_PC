# ==============================================================================
#  Axiom — AI instrument reconstruction synthesizer
#
#  Cross-platform CMake build (Windows / macOS / Linux; VST3 / AU / Standalone).
#  Note: on this machine day-to-day builds also work through Axiom.jucer
#  (Projucer -> Xcode); both builds share the same Source/ tree. CI and
#  non-mac platforms should use this file.
#
#  Options:
#    -DJUCE_SOURCE_DIR=/path/to/JUCE   (default: ../../JUCE next to this repo)
#    -DAXIOM_ENABLE_ONNX=ON            link ONNX Runtime for neural inference;
#                                      OFF (default) uses the analytical DSP
#                                      reconstruction tier only.
# ==============================================================================

cmake_minimum_required(VERSION 3.22)

project(Axiom VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(AXIOM_ENABLE_ONNX "Enable ONNX Runtime neural inference backend" OFF)

if(NOT DEFINED JUCE_SOURCE_DIR)
    set(JUCE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../JUCE")
endif()
add_subdirectory("${JUCE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

juce_add_plugin(Axiom
    PRODUCT_NAME                "Axiom"
    COMPANY_NAME                "Berzerker Beats"
    BUNDLE_ID                   "com.berzerkerbeats.axiom"
    PLUGIN_MANUFACTURER_CODE    Bzrk
    PLUGIN_CODE                 Axm1
    FORMATS                     AU VST3 Standalone
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    COPY_PLUGIN_AFTER_BUILD     TRUE
    VST3_CATEGORIES             Instrument Synth
    AU_MAIN_TYPE                kAudioUnitType_MusicDevice)

juce_generate_juce_header(Axiom)

target_sources(Axiom PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/Analysis/AudioAnalyzer.cpp
    Source/Analysis/PitchDetector.cpp
    Source/AI/OnnxInferenceEngine.cpp
    Source/AI/Reconstructor.cpp
    Source/AI/StemSeparation.cpp
    Source/Synth/SynthEngine.cpp
    Source/Export/InstrumentExporter.cpp
    Source/Preset/PresetManager.cpp
    Source/GUI/AxiomLookAndFeel.cpp
    Source/GUI/EmbeddingSphereView.cpp
    Source/GUI/PresetBar.cpp
    Source/GUI/WaveformView.cpp
    Source/GUI/PatchView.cpp)

target_compile_definitions(Axiom PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    AXIOM_ENABLE_ONNX=$<IF:$<BOOL:${AXIOM_ENABLE_ONNX}>,1,0>)

if(AXIOM_ENABLE_ONNX)
    # Expects an ONNX Runtime distribution; point CMAKE_PREFIX_PATH at it,
    # e.g. -DCMAKE_PREFIX_PATH=$(brew --prefix onnxruntime)
    if(APPLE AND NOT DEFINED CMAKE_PREFIX_PATH)
        list(APPEND CMAKE_PREFIX_PATH /usr/local/opt/onnxruntime /opt/homebrew/opt/onnxruntime)
    endif()
    find_package(onnxruntime REQUIRED)
    target_link_libraries(Axiom PRIVATE onnxruntime::onnxruntime)
endif()

target_link_libraries(Axiom
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
