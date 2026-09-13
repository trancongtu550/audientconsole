add_library(audient_core STATIC
    Version.cpp
    AllocationTracker.cpp)

target_include_directories(audient_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${CMAKE_BINARY_DIR}/generated/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

audient_configure_common_flags(audient_core)

target_compile_features(audient_core PUBLIC cxx_std_20)