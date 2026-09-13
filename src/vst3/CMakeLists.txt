include(CppDependencies)

audient_add_vst3sdk()

set(_vst3_sync "${CMAKE_CURRENT_BINARY_DIR}/vst3-sync")
audient_sync_vst3_layout("${_vst3_sync}")

add_library(audient_vst3_iids OBJECT
    "${_vst3_sync}/public.sdk/source/vst/vstinitiids.cpp"
    "${_vst3_sync}/public.sdk/source/common/commoniids.cpp"
    "${_vst3_sync}/pluginterfaces/base/funknown.cpp"
    "${_vst3_sync}/pluginterfaces/base/ustring.cpp"
    "${_vst3_sync}/pluginterfaces/base/coreiids.cpp")
target_include_directories(audient_vst3_iids PRIVATE "${_vst3_sync}")

add_library(audient_vst3 STATIC
    Vst3Host.cpp
    Vst3HostContext.cpp
    Vst3Processor.cpp
    Vst3ModuleLoader.cpp
    Vst3Editor.cpp
    Vst3Controller.cpp
    Vst3Chain.cpp
    ParameterEditQueue.cpp
    StreamingParameterChanges.cpp
    Vst3StateStream.cpp)

target_sources(audient_vst3 PRIVATE $<TARGET_OBJECTS:audient_vst3_iids>)

target_include_directories(audient_vst3
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${_vst3_sync}")

# Vst3Editor host window uses Win32 user32 (GDI window class registration,
# LoadCursorW, ShowWindow). Made PUBLIC so every consumer (tests, demo) links it.
target_link_libraries(audient_vst3 PUBLIC user32)

audient_configure_common_flags(audient_vst3)

target_compile_features(audient_vst3 PUBLIC cxx_std_20)