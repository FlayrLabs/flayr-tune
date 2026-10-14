# ctest: cmake -DJUCE_DIR=<fetched JUCE> -DSOURCE_DIR=<repo> -DWORK=<dir in the build tree> -P patch_check_test.cmake
#
# cmake/ApplyPatch.cmake must judge a JUCE tree by content. The trap it guards against:
# a JUCE tree with no .git of its own inside another git work tree (here: the build
# directory inside this repository), where `git apply --check --reverse` skips every
# file and reports success. Builds an unpatched copy of exactly the files the patches
# touch from the fetched JUCE's own git history, then checks that
#   1. the unpatched tree fails CHECK_ONLY,
#   2. ApplyPatch applies every patch to it (not "Skipped patch") and CHECK_ONLY passes,
#   3. a tree with one patched file reverted fails as partly applied.
cmake_minimum_required(VERSION 3.22)
find_package(Git REQUIRED)

file(GLOB patches "${SOURCE_DIR}/patches/*.patch")
list(JOIN patches "|" patch_list)
set(tree "${WORK}/juce-unpatched")
file(REMOVE_RECURSE "${tree}")

set(touched)
foreach(patch IN LISTS patches)
    file(STRINGS "${patch}" lines REGEX "^diff --git a/")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^diff --git a/([^ ]+) b/.*$" "\\1" path "${line}")
        list(APPEND touched "${path}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES touched)

# Byte for byte (OUTPUT_FILE; OUTPUT_VARIABLE would turn JUCE's CRLF line ends into LF).
function(upstream_copy path)
    get_filename_component(dir "${tree}/${path}" DIRECTORY)
    file(MAKE_DIRECTORY "${dir}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${JUCE_DIR}" cat-file blob "HEAD:${path}"
                    OUTPUT_FILE "${tree}/${path}" RESULT_VARIABLE missing ERROR_QUIET)
    if(missing) # files the patches add don't exist upstream
        file(REMOVE "${tree}/${path}")
    endif()
endfunction()
foreach(path IN LISTS touched)
    upstream_copy("${path}")
endforeach()
if(EXISTS "${tree}/.git")
    message(FATAL_ERROR "test tree must not be a repository of its own")
endif()

function(run_apply out)
    execute_process(COMMAND ${CMAKE_COMMAND} "-DPATCHES=${patch_list}" ${ARGN} -P "${SOURCE_DIR}/cmake/ApplyPatch.cmake"
                    WORKING_DIRECTORY "${tree}" RESULT_VARIABLE rc OUTPUT_VARIABLE log ERROR_VARIABLE log)
    set(${out} "${rc}" PARENT_SCOPE)
    set(${out}_log "${log}" PARENT_SCOPE)
endfunction()

run_apply(r1 -DCHECK_ONLY=ON)
if(r1 EQUAL 0)
    message(FATAL_ERROR "FAIL: an unpatched tree inside a git work tree passed the check:\n${r1_log}")
endif()
message(STATUS "PASS: unpatched tree refused")

run_apply(r2)
if(NOT r2 EQUAL 0)
    message(FATAL_ERROR "FAIL: could not apply the patches:\n${r2_log}")
endif()
run_apply(r3 -DCHECK_ONLY=ON)
if(NOT r3 EQUAL 0)
    message(FATAL_ERROR "FAIL: freshly patched tree failed the check:\n${r3_log}")
endif()
message(STATUS "PASS: patches applied and verified by content")

# juce-no-network has markers in two files; revert one of them.
upstream_copy("modules/juce_core/juce_core.cpp")
run_apply(r4 -DCHECK_ONLY=ON)
if(r4 EQUAL 0 OR NOT r4_log MATCHES "partly applied")
    message(FATAL_ERROR "FAIL: a partly patched tree was not reported:\n${r4_log}")
endif()
message(STATUS "PASS: partly patched tree refused")
file(REMOVE_RECURSE "${tree}")
