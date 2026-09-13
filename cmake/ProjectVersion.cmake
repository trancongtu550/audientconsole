set(AUDIENT_GIT_SHA "nogit")
set(AUDIENT_BUILD_TIME_UTC "")

find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _git_result
        OUTPUT_VARIABLE _git_sha
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_git_result EQUAL 0 AND _git_sha)
        set(AUDIENT_GIT_SHA "${_git_sha}")
    endif()
endif()

string(TIMESTAMP AUDIENT_BUILD_TIME_UTC "%Y-%m-%d %H:%M:%S UTC" UTC)

set(AUDIENT_CONSOLE_GIT_SHA "${AUDIENT_GIT_SHA}")
set(AUDIENT_CONSOLE_BUILD_TIME "${AUDIENT_BUILD_TIME_UTC}")

function(audient_configure_version_header output_dir)
    set(_header_path "${output_dir}/audient_console_version.h")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/audient_console_version.h.in"
        "${_header_path}" @ONLY)
    message(STATUS "Generated version header: ${_header_path}")
endfunction()