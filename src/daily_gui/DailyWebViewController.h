#pragma once
#include "DailyGui.h"
#include <windows.h>
#include <functional>
#include <string>
namespace audient::daily_gui {
struct DailyWebViewControllerConfig {
    DailyGuiConfig model;
    std::wstring htmlDir;
    std::wstring userDataDir;
    // Daily mode: the window X hides to the tray instead of destroying the
    // engine. Diagnostic/--seconds runs leave this false (real close).
    bool hideOnClose = false;
    // Optional dynamic override (e.g. the live "Close to tray" preference).
    // When set it takes precedence over `hideOnClose`.
    std::function<bool()> shouldHideOnClose;
    std::function<void()> onHidden;
};
HWND DailyWebViewController_Create(HINSTANCE inst, const DailyWebViewControllerConfig& cfg);
void DailyWebViewController_Destroy(HWND hwnd);
// Show/restore + focus the console window (tray "Open Console").
void DailyWebViewController_Show(HWND hwnd);
}
