add_library(audient_transport STATIC
    TransportFormat.cpp
    TransportLinks.cpp
    VirtualCaptureTransport.cpp)

target_include_directories(audient_transport
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

audient_configure_common_flags(audient_transport)

target_compile_features(audient_transport PUBLIC cxx_std_20)

target_link_libraries(audient_transport PRIVATE audient_engine)