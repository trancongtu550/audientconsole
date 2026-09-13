add_library(audient_virtual_audio STATIC
    endpoint/SoftwareCaptureEndpoint.cpp
    driver-protocol/MockDriverCaptureSink.cpp
    driver-protocol/SharedRingCaptureSink.cpp
    driver-protocol/FanoutCaptureSink.cpp
    driver-protocol/VirtualMicFeeder.cpp)

target_include_directories(audient_virtual_audio
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

audient_configure_common_flags(audient_virtual_audio)

target_compile_features(audient_virtual_audio PUBLIC cxx_std_20)

target_link_libraries(audient_virtual_audio PRIVATE audient_transport)