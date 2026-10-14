# Applies PATCH to the current directory (a FetchContent source tree) exactly once.
# Re-running is a no-op when the patch is already in; anything else is an error.
if(NOT PATCH)
    message(FATAL_ERROR "ApplyPatch.cmake: PATCH not set")
endif()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check --reverse --whitespace=nowarn "${PATCH}"
    RESULT_VARIABLE already OUTPUT_QUIET ERROR_QUIET)
if(already EQUAL 0)
    message(STATUS "Patch already applied: ${PATCH}")
    return()
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${PATCH}" RESULT_VARIABLE failed)
if(failed)
    message(FATAL_ERROR "Could not apply ${PATCH}")
endif()
message(STATUS "Applied ${PATCH}")
