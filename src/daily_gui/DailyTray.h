#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

namespace audient::daily_gui
{

// Native Windows notification-area (system tray) integration for the Daily
// Console. Created and owned by the main STA/UI thread (the same thread that
// pumps the WebView window). It NEVER performs ASIO work: menu actions call the
// caller-provided command callbacks, which funnel into the single validated
// asynchronous reconfiguration path.
struct DailyTrayConfig
{
    std::wstring title = L"Audient Console";
    std::function<void()> onOpen;         // show/restore the main window
    std::function<void()> onExit;         // request process shutdown
    std::function<void(long)> onSetBuffer; // same command used by the WebView
    std::function<long()> getCurrentBuffer;
    std::function<std::vector<long>()> getSupportedBuffers;
    std::function<bool()> isRebuilding;
};

class DailyTray
{
public:
    DailyTray() = default;
    ~DailyTray();

    DailyTray(const DailyTray&) = delete;
    DailyTray& operator=(const DailyTray&) = delete;

    bool create(HINSTANCE inst, const DailyTrayConfig& cfg);
    void setTooltip(const std::wstring& text);
    void destroy();
    HWND hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    void showMenu();
    void addIcon();

    DailyTrayConfig m_cfg;
    HWND m_hwnd = nullptr;
    HICON m_icon = nullptr;
    bool m_iconOwned = false; // true when m_icon must be DestroyIcon'd
    bool m_iconAdded = false;
    UINT m_taskbarCreated = 0;
};

} // namespace audient::daily_gui
