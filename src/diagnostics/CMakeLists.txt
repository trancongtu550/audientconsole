add_library(audient_diagnostics STATIC
    CrashHandler.cpp
    DiagnosticBundle.cpp)

target_include_directories(audient_diagnostics
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")

audient_configure_common_flags(audient_diagnostics)

target_link_libraries(audient_diagnostics PUBLIC audient_core)

target_compile_features(audient_diagnostics PUBLIC cxx_std_20)