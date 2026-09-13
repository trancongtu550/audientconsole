cmake_minimum_required(VERSION 3.25)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" AUDIENT_CONSOLE_VERSION LIMIT_COUNT 1)
string(STRIP "${AUDIENT_CONSOLE_VERSION}" AUDIENT_CONSOLE_VERSION)

project(AudientConsole
    VERSION ${AUDIENT_CONSOLE_VERSION}
    DESCRIPTION "Personal Windows-native low-latency console for Audient iD14 MK1 with virtual capture/render endpoints"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(AUDIENT_ENABLE_ASAN "Enable AddressSanitizer in Debug builds" OFF)
option(AUDIENT_ENABLE_ANALYZE "Enable MSVC static analysis (/analyze) on project targets" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

include(ProjectVersion)
include(CompilerWarnings)
include(FormatTarget)
include(Notices)

audient_configure_version_header("${CMAKE_CURRENT_BINARY_DIR}/generated/include")

add_subdirectory(src/core)
add_subdirectory(src/engine)
add_subdirectory(src/channel)
add_subdirectory(src/preferences)
add_subdirectory(src/routing)
add_subdirectory(src/diagnostics)
add_subdirectory(src/asio)
add_subdirectory(src/transport)
add_subdirectory(src/virtual_audio)
add_subdirectory(src/pico)
add_subdirectory(src/vst3)
add_subdirectory(src/production)
add_subdirectory(src/id14-control)
add_subdirectory(src/hardware)
add_subdirectory(src/daily_gui)
add_subdirectory(src/app)

enable_testing()
add_subdirectory(tests)

message(STATUS "AudientConsole ${AUDIENT_CONSOLE_VERSION}")
message(STATUS "  C++ standard: C++20")
message(STATUS "  Version header: ${AUDIENT_CONSOLE_GIT_SHA} @ ${AUDIENT_CONSOLE_BUILD_TIME}")