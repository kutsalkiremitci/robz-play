# Apply the tracked extension to a build-local copy, leaving the submodule clean.
find_package(Git REQUIRED)
set(_aia_patch "${CMAKE_CURRENT_LIST_DIR}/curl-schannel-aia.patch")
set(CHIAKI_CURL_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/curl-patched-source")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/curl/"
    DESTINATION "${CHIAKI_CURL_SOURCE_DIR}" PATTERN ".git" EXCLUDE)
# Refresh patch inputs even if the previous patched copy has newer timestamps.
foreach(_input include/curl/curl.h lib/vtls/schannel_verify.c)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/curl/${_input}"
        "${CHIAKI_CURL_SOURCE_DIR}/${_input}" COPYONLY)
endforeach()

# Run from inside a subdirectory of a repository, git apply silently skips every path
# outside that subdirectory and still reports success. The build directory is such a
# subdirectory whenever CMake re-runs itself from it (after any CMakeLists change), so the
# refreshed files above were left unpatched and the build failed much later, in
# holepunch.c. Apply from the top of the enclosing repository instead - nothing is skipped
# there - or from here when the sources are not in a repository at all, and check below
# that the patch really landed.
set(_aia_cwd "${CMAKE_CURRENT_SOURCE_DIR}")
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --show-cdup
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE _aia_cdup OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _aia_no_repo ERROR_QUIET)
if(_aia_no_repo EQUAL 0 AND _aia_cdup)
    get_filename_component(_aia_cwd "${CMAKE_CURRENT_SOURCE_DIR}/${_aia_cdup}" ABSOLUTE)
endif()

execute_process(COMMAND "${GIT_EXECUTABLE}" apply --unsafe-paths
    "--directory=${CHIAKI_CURL_SOURCE_DIR}" --check "${_aia_patch}"
    WORKING_DIRECTORY "${_aia_cwd}"
    RESULT_VARIABLE _can_apply)
if(NOT _can_apply EQUAL 0)
    message(FATAL_ERROR "Bundled curl does not match the Schannel AIA patch")
endif()
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --unsafe-paths
    "--directory=${CHIAKI_CURL_SOURCE_DIR}" "${_aia_patch}"
    WORKING_DIRECTORY "${_aia_cwd}"
    RESULT_VARIABLE _apply_result)
if(NOT _apply_result EQUAL 0)
    message(FATAL_ERROR "Failed to apply the Schannel AIA patch")
endif()
file(STRINGS "${CHIAKI_CURL_SOURCE_DIR}/include/curl/curl.h" _aia_applied REGEX "CURLSSLOPT_SCHANNEL_AIA")
if(NOT _aia_applied)
    message(FATAL_ERROR "The Schannel AIA patch was skipped: ${CHIAKI_CURL_SOURCE_DIR} is unpatched")
endif()
