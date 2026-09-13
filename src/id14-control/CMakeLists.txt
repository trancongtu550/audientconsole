add_library(audient_id14_control STATIC
    semantic-model/Id14SemanticControl.cpp
    transport/Id14ControlTransport.cpp
    protocol/Id14CommandWhitelist.cpp
    simulator/SimulatedId14Transport.cpp)

target_include_directories(audient_id14_control
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

audient_configure_common_flags(audient_id14_control)

target_compile_features(audient_id14_control PUBLIC cxx_std_20)