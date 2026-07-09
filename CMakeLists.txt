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
    Source/AI/DdspEncoder.cpp
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
    # e.g. -DCMAKE_PREFIX_PATH=$(brew --prefix onnxruntime) on macOS, or at an
    # extracted onnxruntime-win-x64 release zip on Windows.
    if(APPLE AND NOT DEFINED CMAKE_PREFIX_PATH)
        list(APPEND CMAKE_PREFIX_PATH /usr/local/opt/onnxruntime /opt/homebrew/opt/onnxruntime)
    elseif(WIN32 AND NOT DEFINED CMAKE_PREFIX_PATH)
        list(APPEND CMAKE_PREFIX_PATH "$ENV{USERPROFILE}/Documents/onnxruntime")
    endif()

    find_package(onnxruntime QUIET)
    if(onnxruntime_FOUND)
        target_link_libraries(Axiom PRIVATE onnxruntime::onnxruntime)
    else()
        # The GitHub release zips ship a bare include/ + lib/ layout with no
        # CMake package config, so resolve the pieces directly.
        find_path(AXIOM_ONNX_INCLUDE_DIR onnxruntime_cxx_api.h PATH_SUFFIXES include)
        find_library(AXIOM_ONNX_LIBRARY onnxruntime PATH_SUFFIXES lib)
        if(NOT AXIOM_ONNX_INCLUDE_DIR OR NOT AXIOM_ONNX_LIBRARY)
            message(FATAL_ERROR "AXIOM_ENABLE_ONNX=ON but ONNX Runtime was not found; "
                                "set CMAKE_PREFIX_PATH to a distribution containing include/ and lib/")
        endif()
        target_include_directories(Axiom PRIVATE "${AXIOM_ONNX_INCLUDE_DIR}")
        target_link_libraries(Axiom PRIVATE "${AXIOM_ONNX_LIBRARY}")
    endif()

    if(WIN32)
        # Delay-load so hosts can always scan the plugin without the DLL, then
        # ship onnxruntime.dll next to each binary for the runtime preload
        # (see OnnxInferenceEngine.cpp).
        target_link_options(Axiom PUBLIC /DELAYLOAD:onnxruntime.dll)
        target_link_libraries(Axiom PRIVATE delayimp)
        if(AXIOM_ONNX_LIBRARY)
            get_filename_component(AXIOM_ONNX_LIB_DIR "${AXIOM_ONNX_LIBRARY}" DIRECTORY)
            foreach(format_target IN ITEMS Axiom_Standalone Axiom_VST3)
                if(TARGET ${format_target})
                    add_custom_command(TARGET ${format_target} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                                "${AXIOM_ONNX_LIB_DIR}/onnxruntime.dll"
                                "$<TARGET_FILE_DIR:${format_target}>")
                endif()
            endforeach()
        endif()
    endif()
endif()

target_link_libraries(Axiom
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
