include(FetchContent)

set(FETCHCONTENT_QUIET OFF)
set(GTEST_FORCE_SHARED_CRT OFF CACHE BOOL "" FORCE)

function(audient_add_googletest)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.16.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(googletest)
endfunction()

function(audient_add_vst3sdk)
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Declare(vst3_pluginterfaces
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3_pluginterfaces.git
        GIT_TAG 4f547e8e102b47de4a8b8aaf343c73b700786372
        GIT_SHALLOW TRUE)
    FetchContent_Declare(vst3_base
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3_base.git
        GIT_TAG fcf9da0bd27a16f7f03773a3a39822f28f5c8477
        GIT_SHALLOW TRUE)
    FetchContent_Declare(vst3_public_sdk
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3_public_sdk.git
        GIT_TAG 586dc5e6c8012c3e4b01c79389375cbe96bdb1da
        GIT_SHALLOW TRUE)
    FetchContent_Populate(vst3_pluginterfaces)
    FetchContent_Populate(vst3_base)
    FetchContent_Populate(vst3_public_sdk)

    set(vst3_pluginterfaces_SOURCE_DIR "${vst3_pluginterfaces_SOURCE_DIR}" PARENT_SCOPE)
    set(vst3_pluginterfaces_BINARY_DIR "${vst3_pluginterfaces_BINARY_DIR}" PARENT_SCOPE)
    set(vst3_base_SOURCE_DIR "${vst3_base_SOURCE_DIR}" PARENT_SCOPE)
    set(vst3_base_BINARY_DIR "${vst3_base_BINARY_DIR}" PARENT_SCOPE)
    set(vst3_public_sdk_SOURCE_DIR "${vst3_public_sdk_SOURCE_DIR}" PARENT_SCOPE)
    set(vst3_public_sdk_BINARY_DIR "${vst3_public_sdk_BINARY_DIR}" PARENT_SCOPE)
endfunction()

set(AUDIENT_WEBVIEW2_VERSION "1.0.4191.47" CACHE STRING "Pinned Microsoft.Web.WebView2 SDK version")
set(AUDIENT_WEBVIEW2_NUPKG_URL "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/${AUDIENT_WEBVIEW2_VERSION}/microsoft.web.webview2.${AUDIENT_WEBVIEW2_VERSION}.nupkg" CACHE STRING "Pinned WebView2 nupkg URL")
set(AUDIENT_WEBVIEW2_NUPKG_SHA512 "adf91bda1a71d860c098cd0e426b5a238e187ea73c694832f8fde36809a9165fda8844fc9c2b9fc8a870b0da53cd9eb97ec140d9a2f104a228301ffae841db8c" CACHE STRING "SHA512 of pinned WebView2 nupkg (official nupkg integrity)")

function(audient_add_webview2sdk)
    if(TARGET audient_webview2_sdk)
        return()
    endif()
    if(POLICY CMP0169)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_Declare(webview2sdk
        URL "${AUDIENT_WEBVIEW2_NUPKG_URL}"
        URL_HASH SHA512=${AUDIENT_WEBVIEW2_NUPKG_SHA512})
    FetchContent_Populate(webview2sdk)
    set(_dst "${webview2sdk_SOURCE_DIR}")
    if(NOT EXISTS "${_dst}/build/native/include/WebView2.h")
        message(FATAL_ERROR "WebView2 SDK layout unexpected after extract: ${_dst}")
    endif()
    add_library(audient_webview2_sdk INTERFACE)
    target_include_directories(audient_webview2_sdk SYSTEM INTERFACE "${_dst}/build/native/include")
    if(EXISTS "${_dst}/build/native/x64/WebView2LoaderStatic.lib")
        target_link_libraries(audient_webview2_sdk INTERFACE "${_dst}/build/native/x64/WebView2LoaderStatic.lib")
    endif()
    set(AUDIENT_WEBVIEW2SDK_SRC_DIR "${_dst}" CACHE PATH "WebView2 SDK source dir" FORCE)
endfunction()

function(audient_sync_vst3_layout sync_root)
    file(REMOVE_RECURSE "${sync_root}")
    file(MAKE_DIRECTORY "${sync_root}")
    file(COPY "${vst3_pluginterfaces_SOURCE_DIR}/" DESTINATION "${sync_root}/pluginterfaces")
    file(COPY "${vst3_base_SOURCE_DIR}/" DESTINATION "${sync_root}/base")
    file(COPY "${vst3_public_sdk_SOURCE_DIR}/" DESTINATION "${sync_root}/public.sdk")
endfunction()