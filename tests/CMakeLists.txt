add_subdirectory(unit)
add_subdirectory(integration)
add_subdirectory(bench)
add_subdirectory(soak)

if(WIN32)
    add_test(NAME Build002.MissingDependencyFailsConfigure
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/verify-build-002.ps1"
            -SourceDir "${CMAKE_SOURCE_DIR}"
            -BuildRoot "${CMAKE_CURRENT_BINARY_DIR}/build002-tmp"
            -Cmake "${CMAKE_COMMAND}")
    set_tests_properties(Build002.MissingDependencyFailsConfigure PROPERTIES TIMEOUT 300)
endif()