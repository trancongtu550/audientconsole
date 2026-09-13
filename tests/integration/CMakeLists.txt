add_executable(audient_console_integration_tests
    AsioBackendIntegrationTest.cpp
    AsioRegistryDiscoveryTest.cpp
    AsioStreamIntegrationTest.cpp
    AsioStreamTransportIntegrationTest.cpp
    AsioRoutingAdapterIntegrationTest.cpp
    VirtualCaptureTransportIntegrationTest.cpp
    VirtualCaptureEndpointIntegrationTest.cpp
    VirtualMicFeederProtocolIntegrationTest.cpp
    Vst3HostIntegrationTest.cpp
    Vst3ControllerLifecycleIntegrationTest.cpp
    Vst3MultiPluginChainIntegrationTest.cpp
    Vst3LatencyIntegrationTest.cpp
    Vst3RoutingLatencyIntegrationTest.cpp
    Vst3StereoAdapterIntegrationTest.cpp
    Vst3PerChannelChainIntegrationTest.cpp
    Vst3ModuleLoaderBundleTest.cpp
    Vst3RealPluginOfflineTest.cpp
    CaptureMixerIntegrationTest.cpp
    TestPassthroughPlugin.cpp
    SimulatedAsioDriver.cpp)

# Test-only .vst3 module (a real PE DLL exporting GetPluginFactory). Built just
# for the loader/bundle tests; never shipped. OUTPUT_NAME+SUFFIX produce
# "TestPassthrough.vst3" in the same output dir as the integration test exe.
add_library(audient_vst3_test_passthrough_module SHARED
    TestPassthroughModuleDll.cpp
    TestPassthroughPlugin.cpp)
set_target_properties(audient_vst3_test_passthrough_module PROPERTIES
    PREFIX ""
    OUTPUT_NAME "TestPassthrough"
    SUFFIX ".vst3")
target_include_directories(audient_vst3_test_passthrough_module
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(audient_vst3_test_passthrough_module PRIVATE audient_vst3)
audient_configure_common_flags(audient_vst3_test_passthrough_module)

# A second test-only module: a loadable PE that deliberately lacks the
# GetPluginFactory export. Proves the factory-lookup boundary (load succeeds,
# factory lookup fails) without loading any OS DLL from a temp path.
add_library(audient_vst3_test_nonfactory_module SHARED
    TestNonFactoryModuleDll.cpp)
set_target_properties(audient_vst3_test_nonfactory_module PROPERTIES
    PREFIX ""
    OUTPUT_NAME "TestNonFactory"
    SUFFIX ".vst3")
audient_configure_common_flags(audient_vst3_test_nonfactory_module)

if(AUDIENT_ASIO_SDK_DIR)
    target_sources(audient_console_integration_tests PRIVATE AsioNativeProbeTest.cpp)
    target_include_directories(audient_console_integration_tests SYSTEM PRIVATE "${AUDIENT_ASIO_SDK_DIR}")
endif()

target_include_directories(audient_console_integration_tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Expose the built test module file name so the loader tests can locate it next
# to the integration test executable at runtime.
target_compile_definitions(audient_console_integration_tests
    PRIVATE
        TEST_PASSTHROUGH_VST3_MODULE="$<TARGET_FILE_NAME:audient_vst3_test_passthrough_module>"
        TEST_NONFACTORY_VST3_MODULE="$<TARGET_FILE_NAME:audient_vst3_test_nonfactory_module>")

# Phase 4 offline gate: real licensed plug-ins present on this machine are made
# available to the integration tests as configure-time paths. Tests themselves
# SKIP at runtime when the file is missing (no hard dependency on the plug-in).
if(EXISTS "C:/Program Files/Common Files/VST3/Softube/Tube-Tech CL 1B.vst3")
    target_compile_definitions(audient_console_integration_tests
        PRIVATE TEST_REAL_PLUGIN_SOFTUBE_CL1B_PATH="C:/Program Files/Common Files/VST3/Softube/Tube-Tech CL 1B.vst3")
endif()
if(EXISTS "C:/Program Files/Common Files/VST3/AA_Tubechild670.vst3")
    target_compile_definitions(audient_console_integration_tests
        PRIVATE TEST_REAL_PLUGIN_TUBECHILD670_PATH="C:/Program Files/Common Files/VST3/AA_Tubechild670.vst3")
endif()
if(EXISTS "C:/Program Files/Common Files/VST3/FabFilter/FabFilter Pro-Q 4.vst3")
    target_compile_definitions(audient_console_integration_tests
        PRIVATE TEST_REAL_PLUGIN_FABFILTER_Q4_PATH="C:/Program Files/Common Files/VST3/FabFilter/FabFilter Pro-Q 4.vst3")
endif()

audient_configure_common_flags(audient_console_integration_tests)

target_link_libraries(audient_console_integration_tests
    PRIVATE audient_core audient_asio audient_engine audient_transport audient_routing audient_virtual_audio audient_vst3 audient_channel audient_production gtest_main)

include(GoogleTest)
gtest_discover_tests(audient_console_integration_tests)

audient_archive_symbols(audient_console_integration_tests)