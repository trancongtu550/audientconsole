add_library(audient_channel STATIC
    ChannelIdentity.cpp
    ChannelRack.cpp
    InputChannel.cpp
    ChannelCollection.cpp)

target_include_directories(audient_channel
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/..")

audient_configure_common_flags(audient_channel)

target_compile_features(audient_channel PUBLIC cxx_std_20)
