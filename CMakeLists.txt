cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")

project(FlayrTune VERSION 1.2.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FLAYR_TUNE_COPY_AFTER_BUILD "Install the plugins into the user's plug-in folders after building" ON)
option(FLAYR_TUNE_BUILD_TOOLS "Build the UI snapshot and render tools (snapshot needs a desktop session)" OFF)
option(FLAYR_TUNE_BUILD_TESTS "Build the DSP test suite" ON)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

if(APPLE)
    set(FLAYR_TUNE_FORMATS AU VST3)
elseif(WIN32)
    set(FLAYR_TUNE_FORMATS VST3)
else()
    set(FLAYR_TUNE_FORMATS VST3 LV2)
endif()

juce_add_plugin(FlayrTune
    COMPANY_NAME "Flayr Labs"
    COMPANY_WEBSITE "https://github.com/FlayrLabs/flayr-tune"
    BUNDLE_ID com.flayrlabs.flayrtune
    PLUGIN_MANUFACTURER_CODE Flyr
    PLUGIN_CODE FTun
    PRODUCT_NAME "Flayr Tune"
    DESCRIPTION "Real-time pitch correction"
    FORMATS ${FLAYR_TUNE_FORMATS}
    LV2URI "https://github.com/FlayrLabs/flayr-tune"
    VST3_CATEGORIES Fx Pitch Shift
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    # MIDI-controlled effect, so Logic can feed target notes via side-chain MIDI.
    AU_MAIN_TYPE kAudioUnitType_MusicEffect
    # Keep the host's shortcuts (space = play) working while the editor is open.
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${FLAYR_TUNE_COPY_AFTER_BUILD})

target_sources(FlayrTune PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/GraphEditor.cpp)

target_include_directories(FlayrTune PRIVATE Source)

target_compile_definitions(FlayrTune PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(FlayrTune
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(FLAYR_TUNE_BUILD_TESTS)
    enable_testing()
    # The DSP engine is framework-free, so its tests build without JUCE.
    add_executable(engine_test tests/engine_test.cpp)
    target_include_directories(engine_test PRIVATE Source)
    add_test(NAME engine_test COMMAND engine_test)
endif()

if(FLAYR_TUNE_BUILD_TOOLS)
    # UI snapshot without a DAW: FlayrTuneSnapshot out.png [live|graph]
    juce_add_console_app(FlayrTuneSnapshot PRODUCT_NAME "FlayrTuneSnapshot")
    target_sources(FlayrTuneSnapshot PRIVATE tools/snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/GraphEditor.cpp)
    target_include_directories(FlayrTuneSnapshot PRIVATE Source)
    target_compile_definitions(FlayrTuneSnapshot PRIVATE
        JucePlugin_Name="Flayr Tune" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(FlayrTuneSnapshot PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)

    # Real-voice render: render in_mono_f32.wav out_prefix
    add_executable(render tools/render.cpp)
    target_include_directories(render PRIVATE Source)
endif()
