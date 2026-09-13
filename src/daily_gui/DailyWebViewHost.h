#pragma once
#include <windows.h>
#include <functional>
#include <string>
namespace audient::daily_gui {
struct DailyWebViewHostConfig {
    std::wstring htmlDir;
    std::wstring userDataDir;
    std::function<void(const std::wstring&)> onMessage;
    // Reports the WebView2 rasterization scale (device pixels per CSS px) so the
    // host can size the window for the 1440x800 logical design canvas.
    std::function<void(double)> onRasterizationScale;
};
HWND DailyWebViewHost_Create(HINSTANCE inst, HWND parent, const DailyWebViewHostConfig& cfg);
void DailyWebViewHost_Destroy(HWND host);
bool DailyWebViewHost_PostJson(HWND host, const std::wstring& json);
bool DailyWebViewHost_Navigate(HWND host, const std::wstring& url);
// Proportional down-scale (1.0 = native DPI). Used only when the fixed DIP
// window would otherwise overflow the monitor work area.
bool DailyWebViewHost_SetZoom(HWND host, double zoom);
// Installed WebView2 Evergreen runtime version (L"--" when unavailable).
std::wstring DailyWebViewHost_RuntimeVersion();
}
