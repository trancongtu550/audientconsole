if(AUDIENT_ASIO_SDK_DIR)
    add_executable(audient_console_soak_tests
        AsioSoakTest.cpp)

    target_include_directories(audient_console_soak_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.."
        SYSTEM PRIVATE "${AUDIENT_ASIO_SDK_DIR}")

    audient_configure_common_flags(audient_console_soak_tests)

    target_link_libraries(audient_console_soak_tests
        PRIVATE audient_asio audient_engine gtest_main)

    include(GoogleTest)
    gtest_discover_tests(audient_console_soak_tests)

    audient_archive_symbols(audient_console_soak_tests)
endif()