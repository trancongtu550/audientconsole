include(CppDependencies)

audient_add_googletest()

add_executable(audient_console_unit_tests
    CoreVersionSmokeTest.cpp
    DiagnosticsSmokeTest.cpp
    AllocationTrackerSmokeTest.cpp
    RealtimeRingBufferTest.cpp
    MetersCountersTest.cpp
    ChannelControlsSanitizeTest.cpp
    GraphSnapshotTest.cpp
    TestSignalTest.cpp
    CallbackGuardTest.cpp
    EngineRealtimeInvariantTest.cpp
    AsioBackendUnitTest.cpp
    AsioRecoveryControllerTest.cpp
    ChannelPlanBindingTest.cpp
    EngineGraphTest.cpp
    TransportTest.cpp
    StreamFadeTest.cpp
    VirtualCaptureTransportTest.cpp
    VirtualCaptureEndpointTest.cpp
    VirtualMicFeederProtocolTest.cpp
    PicoVirtualMicTransportTest.cpp
    AppPreferencesTest.cpp
    VirtualMicSourcePolicyTest.cpp
    CaptureRingContractTest.cpp
    CaptureControlPlaneTest.cpp
    Id14ControlTest.cpp
    AudientHardwareControlTest.cpp
    Vst3ModuleLoaderTest.cpp
    Vst3HostContextTest.cpp
    Vst3ChainTest.cpp
    ParameterEditQueueTest.cpp
    StreamingParameterChangesTest.cpp
    Vst3StateStreamTest.cpp
    RoutingCoreTest.cpp
    ChannelModelTest.cpp
    CaptureMixerTest.cpp)

audient_configure_common_flags(audient_console_unit_tests)

target_link_libraries(audient_console_unit_tests
    PRIVATE audient_core audient_engine audient_channel audient_routing audient_diagnostics audient_asio audient_transport audient_virtual_audio audient_pico audient_preferences audient_vst3 audient_id14_control audient_hardware gtest_main)

include(GoogleTest)
gtest_discover_tests(audient_console_unit_tests)

audient_archive_symbols(audient_console_unit_tests)