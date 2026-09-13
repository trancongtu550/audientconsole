add_executable(audient_console_bench
    benchmark_main.cpp
    AllocationBench.cpp
    CallbackTimingBench.cpp
    RingBufferBench.cpp)

target_include_directories(audient_console_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

audient_configure_common_flags(audient_console_bench)

target_link_libraries(audient_console_bench PRIVATE audient_core audient_engine)

audient_archive_symbols(audient_console_bench)