include(CppDependencies)
audient_add_webview2sdk()
add_library(audient_daily_gui STATIC
    DailyGui.cpp
    DailyTheme.cpp
    DailyWebViewHost.cpp
    DailyWebViewController.cpp
    DailyTray.cpp
)
target_include_directories(audient_daily_gui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(audient_daily_gui PUBLIC user32 gdi32 comdlg32 comctl32 Msimg32 shell32 audient_webview2_sdk)
audient_configure_common_flags(audient_daily_gui)
add_executable(audient_daily_preview WIN32 preview_main.cpp)
target_link_libraries(audient_daily_preview PRIVATE audient_daily_gui)
audient_configure_common_flags(audient_daily_preview)
# Only the built runtime assets are staged; frontend sources and node_modules
# stay out of the deployment directory.
set(AUDIENT_DAILY_UI_FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/app.css"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/app.js")
add_custom_target(audient_daily_ui ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/ui"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AUDIENT_DAILY_UI_FILES} "${CMAKE_BINARY_DIR}/ui"
    COMMENT "Staging Daily WebView UI to ${CMAKE_BINARY_DIR}/ui")
add_custom_command(TARGET audient_daily_preview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:audient_daily_preview>/ui"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${AUDIENT_DAILY_UI_FILES} "$<TARGET_FILE_DIR:audient_daily_preview>/ui"
    COMMENT "Staging Daily WebView UI beside audient_daily_preview")
