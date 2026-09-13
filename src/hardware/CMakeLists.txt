add_library(audient_hardware STATIC
    AudientHardwareControl.cpp)

target_include_directories(audient_hardware
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

audient_configure_common_flags(audient_hardware)

target_compile_features(audient_hardware PUBLIC cxx_std_20)

target_link_libraries(audient_hardware PUBLIC setupapi)
