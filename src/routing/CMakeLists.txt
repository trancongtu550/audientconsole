add_library(audient_routing STATIC
    CaptureMixer.cpp
    RoutingCore.cpp)

target_include_directories(audient_routing
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/..")

audient_configure_common_flags(audient_routing)

target_compile_features(audient_routing PUBLIC cxx_std_20)