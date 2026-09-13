add_library(audient_engine STATIC
    AudioRamp.cpp
    SanitizeAudio.cpp
    Meters.cpp
    Counters.cpp
    RingBuffer.cpp
    ChannelControls.cpp
    TestSignal.cpp
    EngineGraph.cpp
    SyntheticDownlink.cpp)

target_include_directories(audient_engine
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/..")

audient_configure_common_flags(audient_engine)

target_compile_features(audient_engine PUBLIC cxx_std_20)