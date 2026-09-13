add_library(audient_pico STATIC
    PicoVirtualMicSink.cpp
    PicoUsbWorker.cpp
    WasapiPicoRenderDevice.cpp)

target_include_directories(audient_pico
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")

audient_configure_common_flags(audient_pico)

target_compile_features(audient_pico PUBLIC cxx_std_20)

# WASAPI: ole32 (CoCreateInstance/COM), uuid (device/interface GUIDs), plus the
# Windows multimedia header set in the SDK. No WinUSB/HID/vendor transport.
target_link_libraries(audient_pico
    PRIVATE audient_transport ole32 uuid)
