if(AUDIENT_ASIO_SDK_DIR)
    add_executable(audient_console_demo
        demo_main.cpp)

    target_include_directories(audient_console_demo
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.."
        SYSTEM PRIVATE "${AUDIENT_ASIO_SDK_DIR}")

    audient_configure_common_flags(audient_console_demo)

    target_link_libraries(audient_console_demo
        PRIVATE audient_asio audient_engine audient_transport audient_vst3 user32)

    audient_archive_symbols(audient_console_demo)

    # Slice P manual-hardware-test harness: real iD14 mic -> RoutingCore ->
    # input VST3 chain -> processed-mic-monitor render branch -> Output 1/2.
    add_executable(audient_console_routing_demo
        routing_demo_main.cpp)

    target_include_directories(audient_console_routing_demo
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.."
        SYSTEM PRIVATE "${AUDIENT_ASIO_SDK_DIR}")

    audient_configure_common_flags(audient_console_routing_demo)

    target_link_libraries(audient_console_routing_demo
        PRIVATE audient_asio audient_engine audient_routing audient_vst3)

    audient_archive_symbols(audient_console_routing_demo)

    # Live physical-input virtual-mic integration demo: real Audient ASIO input
    # -> RoutingCore/AsioRoutingAdapter -> input VST3 chain -> VirtualMicFeeder ->
    # SharedRingCaptureSink -> Q5-A3B capture control plane -> virtual mic driver.
    # Standalone manual-test exe; reuses the production libraries only.
    add_executable(audient_console_physical_vmic_demo
        physical_vmic_demo_main.cpp
        DiagGui.cpp
        DiagGui.h
        ../virtual_audio/driver-protocol/CaptureControlClient.cpp)

    target_include_directories(audient_console_physical_vmic_demo
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.."
        SYSTEM PRIVATE "${AUDIENT_ASIO_SDK_DIR}")

    audient_configure_common_flags(audient_console_physical_vmic_demo)

    target_link_libraries(audient_console_physical_vmic_demo
        PRIVATE audient_asio audient_engine audient_routing audient_transport
                audient_vst3 audient_virtual_audio audient_hardware user32 setupapi ole32 advapi32
                winmm)

    audient_archive_symbols(audient_console_physical_vmic_demo)

    # Windows resources for the shipped app: final icon + VERSIONINFO. Generated
    # so the numeric file/product versions track the project VERSION file.
    # RC treats backslashes as escapes in a quoted filename, so use forward slashes.
    set(AUDIENT_DAILY_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/audient_console.ico")
    set(AUDIENT_DAILY_RC "${CMAKE_CURRENT_BINARY_DIR}/audient_console_daily.rc")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/audient_console_daily.rc.in"
                   "${AUDIENT_DAILY_RC}" @ONLY)

    add_executable(audient_console_daily
        daily_demo_main.cpp
        DailyMixerState.cpp
        ../virtual_audio/driver-protocol/CaptureControlClient.cpp
        "${AUDIENT_DAILY_RC}")

    target_include_directories(audient_console_daily
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.."
        SYSTEM PRIVATE "${AUDIENT_ASIO_SDK_DIR}")

    audient_configure_common_flags(audient_console_daily)

    target_link_libraries(audient_console_daily
        PRIVATE audient_asio audient_engine audient_transport audient_daily_gui
                audient_vst3 audient_virtual_audio audient_pico audient_preferences
                audient_core audient_hardware user32 setupapi ole32 advapi32 winmm Shcore)

    # Distributed product = Windows GUI subsystem (no console window). The CRT
    # still calls the existing console-style main(); use an explicit diagnostic
    # console via --console when developer output is needed.
    set_target_properties(audient_console_daily PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(audient_console_daily PRIVATE /ENTRY:mainCRTStartup)

    add_custom_command(TARGET audient_console_daily POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:audient_console_daily>/ui"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/src/daily_gui/ui/index.html"
            "${CMAKE_SOURCE_DIR}/src/daily_gui/ui/app.css"
            "${CMAKE_SOURCE_DIR}/src/daily_gui/ui/app.js"
            "$<TARGET_FILE_DIR:audient_console_daily>/ui"
        COMMENT "Staging Daily WebView UI beside audient_console_daily")

    audient_archive_symbols(audient_console_daily)
else()
    message(STATUS "ASIO SDK not configured; product demo targets disabled (set AUDIENT_ASIO_SDK_DIR to enable).")
endif()