add_library(audient_asio STATIC
    AsioTypes.cpp
    AudientDriverMatcher.cpp
    AsioChannelMap.cpp
    AsioDeviceState.cpp
    AsioBackend.cpp
    AsioStreamBridge.cpp
    AsioRoutingAdapter.cpp
    StreamFade.cpp
    RegistryAsioDriverProvider.cpp)

target_include_directories(audient_asio
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

audient_configure_common_flags(audient_asio)

target_compile_features(audient_asio PUBLIC cxx_std_20)

target_link_libraries(audient_asio PRIVATE audient_engine audient_transport audient_routing audient_channel advapi32)

set(AUDIENT_ASIO_SDK_DIR "" CACHE PATH
    "Steinberg ASIO SDK root (ASIOSDK directory). Optional; when set, the native ASIO SDK provider/driver adapter sources under src/asio/sdk are enabled.")

if(AUDIENT_ASIO_SDK_DIR)
    message(STATUS "ASIO SDK configured at ${AUDIENT_ASIO_SDK_DIR}: enabling native adapter (hardware validation still required).")
    target_sources(audient_asio PRIVATE
        sdk/NativeAsioDriver.cpp
        sdk/NativeAsioDriverProvider.cpp)
    target_include_directories(audient_asio SYSTEM PRIVATE "${AUDIENT_ASIO_SDK_DIR}")
    target_compile_definitions(audient_asio PRIVATE AUDIENT_HAVE_NATIVE_ASIO=1)
else()
    message(STATUS "ASIO SDK not configured; native SDK driver disabled (simulated driver + interfaces active). Set AUDIENT_ASIO_SDK_DIR to enable.")
endif()