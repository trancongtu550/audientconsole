add_library(audient_production STATIC
    ChannelVstChain.cpp)

target_include_directories(audient_production
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

audient_configure_common_flags(audient_production)

target_compile_features(audient_production PUBLIC cxx_std_20)

target_link_libraries(audient_production PRIVATE audient_channel audient_vst3)
