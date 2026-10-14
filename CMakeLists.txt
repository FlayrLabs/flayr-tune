cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")

project(FlayrTune VERSION 1.3.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FLAYR_TUNE_COPY_AFTER_BUILD "Install the plugins into the user's plug-in folders after building" ON)
option(FLAYR_TUNE_BUILD_TOOLS "Build the UI snapshot and render tools (snapshot needs a desktop session)" OFF)
option(FLAYR_TUNE_BUILD_TESTS "Build the DSP test suite" ON)
option(FLAYR_TUNE_BUILD_FUZZERS "Build the ASan/UBSan fuzz targets (development only, never for releases)" OFF)

include(FetchContent)
set(FLAYR_JUCE_PATCHES)
foreach(name no-network xml-entity-overread xml-depth-limit double-exponent-overflow vst3-private-state wrapper-state-checks
             base64-declared-size)
    list(APPEND FLAYR_JUCE_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/juce-${name}.patch)
endforeach()
# The patch contents are part of the patch command, so editing a patch re-runs the step.
set(flayr_patch_hashes "")
foreach(patch IN LISTS FLAYR_JUCE_PATCHES)
    file(SHA256 "${patch}" hash)
    string(APPEND flayr_patch_hashes "${hash}")
endforeach()
string(SHA256 FLAYR_JUCE_PATCH_SET "${flayr_patch_hashes}")
string(REPLACE ";" "|" FLAYR_JUCE_PATCHES "${FLAYR_JUCE_PATCHES}") # survives PATCH_COMMAND
# Pinned to the commit, not the tag, so a moved tag can't change what gets built.
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG ea77444750de45e036e0279bb0291144bf8e3688 # 8.0.16-security
    # Our patches to JUCE; the header of each explains it.
    #  no-network: compiles juce_core's HTTP, socket and interface-enumeration code out
    #    when FLAYR_NO_NETWORK=1. Tune never uses the network.
    #  xml-entity-overread: a heap over-read in the XML parser.
    #  xml-depth-limit: the XML parser fails beyond 64 nesting levels instead of recursing on.
    #  double-exponent-overflow: an int overflow parsing numbers like 1e99999999999.
    #  vst3-private-state: the VST3 wrapper's own state trailer (wild read, stack overflow).
    #  wrapper-state-checks: unchecked types and sizes in the AU and LV2 state paths.
    #  base64-declared-size: base64 decoding allocated whatever size the text claimed.
    PATCH_COMMAND ${CMAKE_COMMAND} "-DPATCHES=${FLAYR_JUCE_PATCHES}" -DPATCH_SET=${FLAYR_JUCE_PATCH_SET}
                  -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyPatch.cmake
)
FetchContent_MakeAvailable(JUCE)
# A JUCE tree from elsewhere (FETCHCONTENT_SOURCE_DIR_JUCE) skips PATCH_COMMAND, and
# without the patches FLAYR_NO_NETWORK would silently do nothing, so check them here,
# by content (see cmake/ApplyPatch.cmake).
execute_process(COMMAND ${CMAKE_COMMAND} "-DPATCHES=${FLAYR_JUCE_PATCHES}" -DCHECK_ONLY=ON
                        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ApplyPatch.cmake
                WORKING_DIRECTORY "${juce_SOURCE_DIR}" RESULT_VARIABLE flayr_patches_missing)
if(flayr_patches_missing)
    message(FATAL_ERROR "JUCE at ${juce_SOURCE_DIR} is missing patches/*.patch; apply them first")
endif()

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

# Leave out libraries nothing in the plug-in calls (JUCE links WebKit and DiscRecording
# on macOS unconditionally) and code nothing references.
if(APPLE)
    target_link_options(FlayrTune INTERFACE -Wl,-dead_strip -Wl,-dead_strip_dylibs)
elseif(MSVC)
    target_link_options(FlayrTune INTERFACE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
elseif(UNIX)
    target_link_options(FlayrTune INTERFACE -Wl,--as-needed)
endif()

target_sources(FlayrTune PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/GraphEditor.cpp)

target_include_directories(FlayrTune PRIVATE Source)

target_compile_definitions(FlayrTune PUBLIC
    FLAYR_NO_NETWORK=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(FlayrTune
    PRIVATE
        juce::juce_audio_processors
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

    # The plug-in wrapper against hostile project state, MIDI and host positions.
    juce_add_console_app(plugin_test PRODUCT_NAME "plugin_test")
    target_sources(plugin_test PRIVATE tests/plugin_test.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/GraphEditor.cpp)
    target_include_directories(plugin_test PRIVATE Source)
    target_compile_definitions(plugin_test PRIVATE
        JucePlugin_Name="Flayr Tune" FLAYR_NO_NETWORK=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(plugin_test PRIVATE juce::juce_audio_processors juce::juce_recommended_config_flags)
    add_test(NAME plugin_test COMMAND plugin_test)

    # The built VST3, loaded as a host would, against hostile wrapper state.
    juce_add_console_app(vst3_host_test PRODUCT_NAME "vst3_host_test")
    target_sources(vst3_host_test PRIVATE tests/vst3_host_test.cpp)
    target_compile_definitions(vst3_host_test PRIVATE
        JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_PLUGINHOST_LV2=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(vst3_host_test PRIVATE juce::juce_audio_processors juce::juce_recommended_config_flags)
    add_dependencies(vst3_host_test FlayrTune_VST3)
    add_test(NAME vst3_host_test
             COMMAND vst3_host_test "${CMAKE_BINARY_DIR}/FlayrTune_artefacts/$<CONFIG>/VST3/Flayr Tune.vst3")

    # cmake/ApplyPatch.cmake judges a JUCE tree by content, even one inside this work tree.
    add_test(NAME juce_patch_check
             COMMAND ${CMAKE_COMMAND} -DJUCE_DIR=${juce_SOURCE_DIR} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
                     -DWORK=${CMAKE_CURRENT_BINARY_DIR} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/patch_check_test.cmake)

    # The release's CI-asset verification (scripts/verify-release-assets.sh), against a fake gh.
    find_program(FLAYR_BASH bash)
    find_program(FLAYR_JQ jq)
    find_program(FLAYR_SHASUM shasum)
    if(NOT WIN32 AND FLAYR_BASH AND FLAYR_JQ AND FLAYR_SHASUM)
        add_test(NAME release_verify_test COMMAND ${FLAYR_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/release_verify_test.sh)
        set_tests_properties(release_verify_test PROPERTIES TIMEOUT 120)
    endif()

    # A hang is a failure too.
    set_tests_properties(engine_test plugin_test vst3_host_test juce_patch_check PROPERTIES TIMEOUT 300)
endif()

if(FLAYR_TUNE_BUILD_TOOLS)
    # UI snapshot without a DAW: FlayrTuneSnapshot out.png [live|graph]
    juce_add_console_app(FlayrTuneSnapshot PRODUCT_NAME "FlayrTuneSnapshot")
    target_sources(FlayrTuneSnapshot PRIVATE tools/snapshot.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/GraphEditor.cpp)
    target_include_directories(FlayrTuneSnapshot PRIVATE Source)
    target_compile_definitions(FlayrTuneSnapshot PRIVATE
        JucePlugin_Name="Flayr Tune" FLAYR_NO_NETWORK=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0)
    target_link_libraries(FlayrTuneSnapshot PRIVATE juce::juce_audio_processors juce::juce_recommended_config_flags)

    # Real-voice render: render in_mono_f32.wav out_prefix
    add_executable(render tools/render.cpp)
    target_include_directories(render PRIVATE Source)
endif()

if(FLAYR_TUNE_BUILD_FUZZERS)
    # See tests/fuzz/README.md. Uses libFuzzer where the compiler has it (Linux clang);
    # otherwise (Apple clang) tests/fuzz/fuzz_driver.cpp, a coverage-guided mutation
    # fuzzer on -fsanitize-coverage=inline-8bit-counters with the same command line.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
    check_cxx_source_compiles("#include <cstddef>
#include <cstdint>
extern \"C\" int LLVMFuzzerTestOneInput (const uint8_t*, size_t) { return 0; }" FLAYR_HAVE_LIBFUZZER)
    unset(CMAKE_REQUIRED_FLAGS)
    set(flayr_sanitizers -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g)

    function(flayr_fuzz_app name)
        juce_add_console_app(${name} PRODUCT_NAME ${name})
        target_sources(${name} PRIVATE ${ARGN} Source/PluginProcessor.cpp Source/PluginEditor.cpp Source/GraphEditor.cpp)
        target_include_directories(${name} PRIVATE Source tests/fuzz)
        target_compile_definitions(${name} PRIVATE
            JucePlugin_Name="Flayr Tune" FLAYR_NO_NETWORK=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
        target_link_libraries(${name} PRIVATE juce::juce_audio_processors juce::juce_recommended_config_flags)
        target_compile_options(${name} PRIVATE ${flayr_sanitizers})
        target_link_options(${name} PRIVATE ${flayr_sanitizers})
    endfunction()

    foreach(target fuzz_state fuzz_host fuzz_vst3)
        if(FLAYR_HAVE_LIBFUZZER)
            flayr_fuzz_app(${target} tests/fuzz/${target}.cpp)
            target_compile_options(${target} PRIVATE -fsanitize=fuzzer-no-link)
            target_link_options(${target} PRIVATE -fsanitize=fuzzer)
        else()
            flayr_fuzz_app(${target} tests/fuzz/${target}.cpp tests/fuzz/fuzz_driver.cpp)
            target_compile_options(${target} PRIVATE -fsanitize-coverage=inline-8bit-counters
                -fsanitize-coverage-ignorelist=${CMAKE_CURRENT_SOURCE_DIR}/tests/fuzz/coverage_ignore.txt)
        endif()
    endforeach()
    if(NOT FLAYR_HAVE_LIBFUZZER)
        # The driver reads the coverage counters; it doesn't need any of its own.
        set_source_files_properties(tests/fuzz/fuzz_driver.cpp TARGET_DIRECTORY fuzz_state fuzz_host fuzz_vst3
            PROPERTIES COMPILE_OPTIONS "-fno-sanitize-coverage=inline-8bit-counters")
    endif()
    flayr_fuzz_app(fuzz_make_corpus tests/fuzz/make_corpus.cpp)
endif()
