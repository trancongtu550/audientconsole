#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DailyTray.h"
#include <shellapi.h>
#include <cstdio>

namespace audient::daily_gui
{

namespace
{
constexpr wchar_t kTrayClass[] = L"AudientConsoleTrayClass";
constexpr UINT kTrayCallback = WM_APP + 1;
constexpr UINT kMenuOpen = 1;
constexpr UINT kMenuExit = 2;
constexpr UINT kMenuBufferBase = 100; // + index into supported buffers
// Final application icon embedded in AudientConsole.exe (see
// audient_console_daily.rc.in: IDI_APPICON 101). Loaded from the running module
// so the tray matches Explorer/taskbar/titlebar/Start Menu exactly.
constexpr int kAppIconResourceId = 101;
} // namespace

DailyTray::~DailyTray()
{
    destroy();
}

LRESULT CALLBACK DailyTray::wndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    DailyTray* self = reinterpret_cast<DailyTray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr && self->m_taskbarCreated != 0 && message == self->m_taskbarCreated)
    {
        // Explorer restarted: re-register the icon.
        self->m_iconAdded = false;
        self->addIcon();
        return 0;
    }
    if (message == kTrayCallback && self != nullptr)
    {
        const UINT event = LOWORD(lParam);
        if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP || event == WM_RBUTTONDBLCLK)
        {
            self->showMenu();
        }
        else if (event == NIN_SELECT || event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK)
        {
            if (self->m_cfg.onOpen)
            {
                self->m_cfg.onOpen();
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool DailyTray::create(HINSTANCE inst, const DailyTrayConfig& cfg)
{
    m_cfg = cfg;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &DailyTray::wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kTrayClass;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return false;
    }

    // Hidden TOP-LEVEL window (not message-only) so it receives the
    // "TaskbarCreated" broadcast when Explorer restarts.
    m_hwnd = CreateWindowExW(0, kTrayClass, L"Audient Console Tray", WS_OVERLAPPED,
                             0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (m_hwnd == nullptr)
    {
        return false;
    }
    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    m_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    addIcon();
    return m_iconAdded;
}

void DailyTray::addIcon()
{
    if (m_hwnd == nullptr || m_iconAdded)
    {
        return;
    }
    if (m_icon == nullptr)
    {
        // Prefer the final icon embedded in the executable so the tray matches
        // Explorer/taskbar/titlebar/Start Menu. LoadImage creates an owned
        // handle that is DestroyIcon'd only after NIM_DELETE.
        const int cx = GetSystemMetrics(SM_CXSMICON);
        const int cy = GetSystemMetrics(SM_CYSMICON);
        m_icon = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON,
            cx > 0 ? cx : 16, cy > 0 ? cy : 16, LR_DEFAULTCOLOR));
        m_iconOwned = (m_icon != nullptr);
        if (m_icon == nullptr)
        {
            // Fallback for hosts without the embedded resource (e.g. preview):
            // shared system icon; must NOT be destroyed.
            m_icon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
            m_iconOwned = false;
        }
        std::printf("[tray] icon %s\n", m_iconOwned ? "embedded(app)" : "fallback(system)");
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayCallback;
    nid.hIcon = m_icon;
    const std::wstring tip = m_cfg.title;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    if (Shell_NotifyIconW(NIM_ADD, &nid))
    {
        nid.uVersion = NOTIFYICON_VERSION_4;
        (void)Shell_NotifyIconW(NIM_SETVERSION, &nid);
        m_iconAdded = true;
    }
}

void DailyTray::setTooltip(const std::wstring& text)
{
    if (m_hwnd == nullptr || !m_iconAdded)
    {
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = m_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, text.c_str(), _TRUNCATE);
    (void)Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void DailyTray::showMenu()
{
    if (m_hwnd == nullptr)
    {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr)
    {
        return;
    }
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, m_cfg.title.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuOpen, L"Open Console");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU buffers = CreatePopupMenu();
    const bool rebuilding = m_cfg.isRebuilding ? m_cfg.isRebuilding() : false;
    const long current = m_cfg.getCurrentBuffer ? m_cfg.getCurrentBuffer() : 0;
    const std::vector<long> supported = m_cfg.getSupportedBuffers
        ? m_cfg.getSupportedBuffers()
        : std::vector<long>{};
    if (buffers != nullptr)
    {
        int index = 0;
        for (const long value : supported)
        {
            UINT flags = MF_STRING;
            if (value == current)
            {
                flags |= MF_CHECKED;
            }
            if (rebuilding)
            {
                flags |= MF_GRAYED;
            }
            wchar_t label[16]{};
            swprintf_s(label, L"%ld", value);
            AppendMenuW(buffers, flags, kMenuBufferBase + static_cast<UINT>(index), label);
            ++index;
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(buffers), L"Buffer Size");
    }
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"48 kHz (Locked)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit Audient Console");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(m_hwnd); // required so the menu dismisses correctly
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        pt.x, pt.y, 0, m_hwnd, nullptr);
    PostMessageW(m_hwnd, WM_NULL, 0, 0);

    if (command == kMenuOpen && m_cfg.onOpen)
    {
        m_cfg.onOpen();
    }
    else if (command == kMenuExit && m_cfg.onExit)
    {
        m_cfg.onExit();
    }
    else if (command >= kMenuBufferBase)
    {
        const std::size_t idx = static_cast<std::size_t>(command - kMenuBufferBase);
        if (idx < supported.size() && m_cfg.onSetBuffer)
        {
            m_cfg.onSetBuffer(supported[idx]);
        }
    }

    DestroyMenu(menu); // also destroys the popup submenu
}

void DailyTray::destroy()
{
    if (m_hwnd != nullptr && m_iconAdded)
    {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = m_hwnd;
        nid.uID = 1;
        (void)Shell_NotifyIconW(NIM_DELETE, &nid);
        m_iconAdded = false;
    }
    // Destroy the owned icon only AFTER the tray icon has been removed.
    if (m_icon != nullptr && m_iconOwned)
    {
        DestroyIcon(m_icon);
    }
    m_icon = nullptr;
    m_iconOwned = false;
    if (m_hwnd != nullptr)
    {
        SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

} // namespace audient::daily_gui
