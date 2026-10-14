# Applies each of PATCHES ("|"-separated) to the current directory, a FetchContent
# source tree, exactly once: a patch that is already in is skipped, anything else that
# fails to apply is an error. With CHECK_ONLY, only verifies that every patch is in.
if(NOT PATCHES)
    message(FATAL_ERROR "ApplyPatch.cmake: PATCHES not set")
endif()
find_package(Git REQUIRED)
string(REPLACE "|" ";" PATCHES "${PATCHES}")
foreach(patch IN LISTS PATCHES)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check --reverse --whitespace=nowarn "${patch}"
        RESULT_VARIABLE missing OUTPUT_QUIET ERROR_QUIET)
    if(NOT missing)
        message(STATUS "JUCE patch present: ${patch}")
    elseif(CHECK_ONLY)
        message(FATAL_ERROR "JUCE patch not applied: ${patch}")
    else()
        execute_process(COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${patch}" RESULT_VARIABLE failed)
        if(failed)
            message(FATAL_ERROR "Could not apply ${patch}")
        endif()
        message(STATUS "Applied ${patch}")
    endif()
endforeach()
