#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DailyWebViewController.h"
#include "DailyWebViewHost.h"
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <cmath>
namespace audient::daily_gui {
namespace {
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
constexpr UINT kTimerId = 9102;
constexpr UINT kTimerMs = 33; // ~30 Hz snapshot cadence
constexpr wchar_t kClass[] = L"AudientDailyWebContainer";
// Fixed logical (DIP) client sizes per presentation mode. The window is NOT
// user-resizable: it has no thick frame and no maximize box, and WM_GETMINMAXINFO
// locks the size to the active mode. Physical size = DIP * (monitor_dpi/96).
constexpr int kMainClientW = 750;
constexpr int kMainBaseH = 372;   // Main height for 1 shared insert row (incl. bottom breathing)
constexpr int kMainRowDelta = 43; // extra DIP per additional visible insert row
constexpr int kMiniClientW = 148;
constexpr int kMiniClientH = 375;
// WS_OVERLAPPEDWINDOW minus WS_THICKFRAME (no drag-resize) and WS_MAXIMIZEBOX.
constexpr DWORD kFixedFrameStyle =
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
struct CtrlState {
    DailyGuiConfig model;
    std::wstring htmlDir;
    std::wstring userDataDir;
    HWND container{nullptr};
    HWND webHost{nullptr};
    double rasterScale{0.0}; // device pixels per CSS px (from WebView2)
    double zoomFactor{1.0};  // proportional down-scale when clamped to work area
    RECT appliedClient{0,0,0,0};
    int uiMode{0};           // 0 = main mixer, 1 = mini monitor
    int mainRows{1};         // shared visible insert-row count (Main height class)
    bool destroyed{false};
    bool hideOnClose{false};
    std::function<bool()> shouldHideOnClose;
    std::function<void()> onHidden;
    // UI-thread heartbeat: detects a stalled STA message pump (>150 ms).
    std::chrono::steady_clock::time_point lastTick{};
    bool hasLastTick{false};
};
int ModeClientWidth(int mode) { return mode == 1 ? kMiniClientW : kMainClientW; }
int ModeClientHeight(int mode, int mainRows) {
    if (mode == 1) return kMiniClientH;
    const int rows = mainRows < 1 ? 1 : (mainRows > 3 ? 3 : mainRows);
    return kMainBaseH + (rows - 1) * kMainRowDelta;
}
UINT WindowDpi(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    if (dpi == 0) dpi = GetDpiForSystem();
    if (dpi == 0) dpi = 96;
    return dpi;
}
// Resize the fixed-size frame to the active mode's DIP size at the current DPI.
// Also clamp the physical size to ~85% of the monitor work area: if the
// DPI-scaled size would overflow the desktop, scale the WebView down
// proportionally (zoom) instead of overflowing (still no user resize).
void ApplyModeSize(CtrlState* st) {
    if (st == nullptr || st->container == nullptr) return;
    const double scale = st->rasterScale > 0.0
        ? st->rasterScale
        : (static_cast<double>(WindowDpi(st->container)) / 96.0);
    const int wDip = ModeClientWidth(st->uiMode);
    const int hDip = ModeClientHeight(st->uiMode, st->mainRows);
    double targetW = std::lround(wDip * scale);
    double targetH = std::lround(hDip * scale);
    // Clamp to the work area of the monitor under the window.
    double fit = 1.0;
    HMONITOR mon = MonitorFromWindow(st->container, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon != nullptr && GetMonitorInfoW(mon, &mi)) {
        const double workW = static_cast<double>(mi.rcWork.right - mi.rcWork.left);
        const double workH = static_cast<double>(mi.rcWork.bottom - mi.rcWork.top);
        const double maxW = workW * 0.85;
        const double maxH = workH * 0.85;
        if (targetW > maxW && targetW > 0.0) fit = std::min(fit, maxW / targetW);
        if (targetH > maxH && targetH > 0.0) fit = std::min(fit, maxH / targetH);
    }
    targetW = std::lround(targetW * fit);
    targetH = std::lround(targetH * fit);
    RECT rc{0, 0, (LONG)targetW, (LONG)targetH};
    const DWORD style = (DWORD)GetWindowLongPtrW(st->container, GWL_STYLE);
    const DWORD ex = (DWORD)GetWindowLongPtrW(st->container, GWL_EXSTYLE);
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    st->appliedClient = {0, 0, (LONG)targetW, (LONG)targetH};
    SetWindowPos(st->container, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (st->webHost != nullptr && std::abs(fit - st->zoomFactor) > 0.001) {
        st->zoomFactor = fit;
        DailyWebViewHost_SetZoom(st->webHost, fit);
    }
}
void ApplyDarkTitleBar(HWND hwnd) {
    if (hwnd == nullptr) return;
    BOOL dark = TRUE;
    using PfnDwmSetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr) return;
    auto setAttr = reinterpret_cast<PfnDwmSetWindowAttribute>(
        GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (setAttr != nullptr) {
        // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (current); 19 = pre-20H1 value.
        if (FAILED(setAttr(hwnd, 20, &dark, sizeof(dark)))) {
            (void)setAttr(hwnd, 19, &dark, sizeof(dark));
        }
    }
    FreeLibrary(dwm);
}
std::wstring escJson(const std::string& s) {
    std::wstring w; w.reserve(s.size()+8);
    for (unsigned char c : s) {
        if (c=='\\' || c=='"') { w.push_back(L'\\'); w.push_back((wchar_t)c); }
        else if (c=='\n') { w += L"\\n"; }
        else if (c=='\r') { w += L"\\r"; }
        else if (c<0x20) { wchar_t b[7]; swprintf(b,7,L"\\u%04x",(int)c); w+=b; }
        else w.push_back((wchar_t)c);
    }
    return w;
}
std::wstring buildSnapshotJson(const DailyGuiConfig& m) {
    std::wstringstream ss; ss << std::fixed << std::setprecision(2);
    auto b2s=[&](bool b){ return b ? L"true" : L"false"; };
    bool gMon = m.getGlobalMonitorOn ? m.getGlobalMonitorOn() : false;
    bool mono = m.getMonoOn ? m.getMonoOn() : false;
    bool loc0 = m.getLocalEnable ? m.getLocalEnable(0) : false;
    bool loc1 = m.getLocalEnable ? m.getLocalEnable(1) : false;
    bool mute0 = m.getLocalMute ? m.getLocalMute(0) : false;
    bool mute1 = m.getLocalMute ? m.getLocalMute(1) : false;
    float gain0 = m.getLocalGainDb ? m.getLocalGainDb(0) : 0.f;
    float gain1 = m.getLocalGainDb ? m.getLocalGainDb(1) : 0.f;
    auto pr0 = m.getMetersCh1 ? m.getMetersCh1() : std::make_pair(0.f,0.f);
    auto pr1 = m.getMetersCh2 ? m.getMetersCh2() : std::make_pair(0.f,0.f);
    std::vector<std::string> n0 = m.getCh1Names ? m.getCh1Names() : std::vector<std::string>{};
    std::vector<std::string> n1 = m.getCh2Names ? m.getCh2Names() : std::vector<std::string>{};
    int sel0 = m.getCh1Sel ? m.getCh1Sel() : -1;
    int sel1 = m.getCh2Sel ? m.getCh2Sel() : -1;
    bool wb0 = m.getWholeBypass ? m.getWholeBypass(0) : false;
    bool wb1 = m.getWholeBypass ? m.getWholeBypass(1) : false;
    std::string asio = m.getAsioText ? m.getAsioText() : std::string{};
    std::string xruns = m.getXrunsText ? m.getXrunsText() : std::string{};
    bool picoConnected = m.getPicoConnected ? m.getPicoConnected() : false;
    std::string picoText = m.getPicoText ? m.getPicoText() : std::string{};
    int virtualMicSource = m.getVirtualMicSource ? m.getVirtualMicSource() : 0;
    bool closeToTray = m.getCloseToTray ? m.getCloseToTray() : false;
    bool startMinimized = m.getStartMinimized ? m.getStartMinimized() : false;
    std::string appVersion = m.getAppVersion ? m.getAppVersion() : std::string{};
    std::string audioDevice = m.getAudioDeviceStatus ? m.getAudioDeviceStatus() : std::string{};
    std::string hardwareControl = m.getHardwareControlStatus ? m.getHardwareControlStatus() : std::string{};
    const std::wstring wvWide = DailyWebViewHost_RuntimeVersion();
    std::string webview2Version;
    webview2Version.reserve(wvWide.size());
    for (const wchar_t c : wvWide) webview2Version.push_back(c < 128 ? (char)c : '?');
    std::optional<double> monDb = m.getHwMonDb ? m.getHwMonDb() : std::nullopt;
    std::optional<double> hpDb = m.getHwHpDb ? m.getHwHpDb() : std::nullopt;
    std::string monText = m.getHwMonText ? m.getHwMonText() : std::string{};
    std::string hpText = m.getHwHpText ? m.getHwHpText() : std::string{};
    long curRate = m.getAudioCurrentRate ? m.getAudioCurrentRate() : 48000;
    long actRate = m.getAudioActualRate ? m.getAudioActualRate() : 0;
    long curBuf = m.getAudioCurrentBuffer ? m.getAudioCurrentBuffer() : 0;
    long reqBuf = m.getAudioRequestedBuffer ? m.getAudioRequestedBuffer() : 0;
    std::vector<long> supRates = m.getAudioSupportedRates ? m.getAudioSupportedRates() : std::vector<long>{};
    std::vector<long> supBufs = m.getAudioSupportedBuffers ? m.getAudioSupportedBuffers() : std::vector<long>{};
    bool rebuilding = m.getAudioRebuilding ? m.getAudioRebuilding() : false;
    int uiMode = m.getUiMode ? m.getUiMode() : 0;
    float sysPeakL = m.getSystemPeakL ? m.getSystemPeakL() : -1.0f;
    float sysPeakR = m.getSystemPeakR ? m.getSystemPeakR() : -1.0f;
    std::optional<bool> sysMuted = m.getSystemMuted ? m.getSystemMuted() : std::nullopt;
    ss << L"{\"type\":\"snapshot\"";
    ss << L",\"dual\":" << b2s(m.dualMode);
    ss << L",\"globalMonitor\":" << b2s(gMon);
    ss << L",\"mono\":" << b2s(mono);
    ss << L",\"ch\":[";
    ss << L"{\"local\":" << b2s(loc0) << L",\"mute\":" << b2s(mute0) << L",\"gainDb\":" << gain0 << L",\"raw\":" << pr0.first << L",\"post\":" << pr0.second << L",\"sel\":" << sel0 << L",\"wholeBypass\":" << b2s(wb0) << L",\"names\":[";
    for(size_t i=0;i<n0.size();++i){ if(i) ss<<L","; ss<<L"\""<<escJson(n0[i])<<L"\""; }
    ss<<L"]},";
    ss << L"{\"local\":" << b2s(loc1) << L",\"mute\":" << b2s(mute1) << L",\"gainDb\":" << gain1 << L",\"raw\":" << pr1.first << L",\"post\":" << pr1.second << L",\"sel\":" << sel1 << L",\"wholeBypass\":" << b2s(wb1) << L",\"names\":[";
    for(size_t i=0;i<n1.size();++i){ if(i) ss<<L","; ss<<L"\""<<escJson(n1[i])<<L"\""; }
    ss<<L"]}";
    ss<<L"]";
    ss<<L",\"asioText\":\""<<escJson(asio)<<L"\"";
    ss<<L",\"xrunsText\":\""<<escJson(xruns)<<L"\"";
    ss<<L",\"picoConnected\":"<<b2s(picoConnected);
    ss<<L",\"picoText\":\""<<escJson(picoText)<<L"\"";
    ss<<L",\"virtualMicSource\":"<<virtualMicSource;
    ss<<L",\"closeToTray\":"<<b2s(closeToTray);
    ss<<L",\"startMinimized\":"<<b2s(startMinimized);
    ss<<L",\"appVersion\":\""<<escJson(appVersion)<<L"\"";
    ss<<L",\"audioDevice\":\""<<escJson(audioDevice)<<L"\"";
    ss<<L",\"hardwareControl\":\""<<escJson(hardwareControl)<<L"\"";
    ss<<L",\"webview2Version\":\""<<escJson(webview2Version)<<L"\"";
    ss<<L",\"uiMode\":"<<uiMode;
    ss<<L",\"systemPeakL\":"<<sysPeakL;
    ss<<L",\"systemPeakR\":"<<sysPeakR;
    ss<<L",\"systemMuted\":"; if(sysMuted) ss<<b2s(*sysMuted); else ss<<L"null";
    ss<<L",\"hwMonDb\":"; if(monDb) ss<<*monDb; else ss<<L"null";
    ss<<L",\"hwHpDb\":"; if(hpDb) ss<<*hpDb; else ss<<L"null";
    ss<<L",\"hwMonText\":\""<<escJson(monText)<<L"\"";
    ss<<L",\"hwHpText\":\""<<escJson(hpText)<<L"\"";
    ss<<L",\"audio\":{\"currentRate\":"<<curRate<<L",\"actualRate\":"<<actRate
      <<L",\"currentBuffer\":"<<curBuf<<L",\"requestedBuffer\":"<<reqBuf
      <<L",\"rebuilding\":"<<b2s(rebuilding)<<L",\"supportedRates\":[";
    for(size_t i=0;i<supRates.size();++i){ if(i) ss<<L","; ss<<supRates[i]; }
    ss<<L"],\"supportedBuffers\":[";
    for(size_t i=0;i<supBufs.size();++i){ if(i) ss<<L","; ss<<supBufs[i]; }
    ss<<L"]}";
    ss<<L"}";
    return ss.str();
}
void handleMessage(CtrlState* st, const std::wstring& raw) {
    if (!st) return;
    auto& m = st->model;
    std::string s; s.reserve(raw.size()); for (wchar_t c: raw) s.push_back((char)(c<128?c:'?'));
    auto has=[&](const char* k){ return s.find(k)!=std::string::npos; };
    auto boolVal=[&](const char* key)->bool{ auto p=s.find(key); if(p==std::string::npos) return false; auto q=s.find("true",p); auto f=s.find("false",p); if(q!=std::string::npos && (f==std::string::npos||q<f)) return true; return false; };
    auto intVal=[&](const char* key, int def)->int{ auto p=s.find(key); if(p==std::string::npos) return def; auto q=s.find(":",p); if(q==std::string::npos) return def; int v=def; (void)sscanf_s(s.c_str()+q+1, "%d", &v); return v; };
    auto floatVal=[&](const char* key, float def)->float{ auto p=s.find(key); if(p==std::string::npos) return def; auto q=s.find(":",p); if(q==std::string::npos) return def; float v=def; (void)sscanf_s(s.c_str()+q+1, "%f", &v); return v; };
    auto doubleVal=[&](const char* key, double def)->double{ auto p=s.find(key); if(p==std::string::npos) return def; auto q=s.find(":",p); if(q==std::string::npos) return def; double v=def; (void)sscanf_s(s.c_str()+q+1, "%lf", &v); return v; };
    std::string cmd;
    { auto p=s.find("\"cmd\""); if(p!=std::string::npos){ auto q=s.find(":",p); if(q!=std::string::npos){ auto a=s.find("\"",q); auto b=s.find("\"",a+1); if(a!=std::string::npos&&b!=std::string::npos) cmd=s.substr(a+1,b-a-1); } } }
    if (cmd=="toggleGlobal") { if(m.setGlobalMonitorOn) m.setGlobalMonitorOn(boolVal("\"on\"")); }
    else if (cmd=="toggleMono") { if(m.setMonoOn) m.setMonoOn(boolVal("\"on\"")); }
    else if (cmd=="setLocal") { int ch=intVal("\"ch\"",0); bool on=boolVal("\"on\""); if(m.setLocalEnable) m.setLocalEnable(ch,on); }
    else if (cmd=="setMute") { int ch=intVal("\"ch\"",0); bool on=boolVal("\"on\""); if(m.setLocalMute) m.setLocalMute(ch,on); }
    else if (cmd=="setGain") { int ch=intVal("\"ch\"",0); float db=floatVal("\"db\"",0.f); db=std::max(-60.f,std::min(0.f,db)); if(m.setLocalGainDb) m.setLocalGainDb(ch,db); }
    else if (cmd=="setSel") { int ch=intVal("\"ch\"",0); int idx=intVal("\"idx\"",-1); if(m.setSel && idx>=0) m.setSel(ch,idx); }
    else if (cmd=="load") { int ch=intVal("\"ch\"",0); if(m.onLoad) m.onLoad(ch); }
    else if (cmd=="openEditor") { int ch=intVal("\"ch\"",0); int idx=intVal("\"idx\"",0); if(m.onOpenEditor) m.onOpenEditor(ch,idx); }
    else if (cmd=="bypassSel") { int ch=intVal("\"ch\"",0); int idx=intVal("\"idx\"",0); if(m.onBypassSel) m.onBypassSel(ch,idx); }
    else if (cmd=="bypassWhole") { int ch=intVal("\"ch\"",0); if(m.onBypassWhole) m.onBypassWhole(ch); }
    else if (cmd=="remove") { int ch=intVal("\"ch\"",0); int idx=intVal("\"idx\"",0); if(m.onRemove) m.onRemove(ch,idx); }
    else if (cmd=="nudgeHw") { bool hp=boolVal("\"hp\""); double d=doubleVal("\"delta\"",0); if(m.onNudgeHw) m.onNudgeHw(hp,d); }
    else if (cmd=="setHwDb") { bool hp=boolVal("\"hp\""); double db=doubleVal("\"db\"",-18); if(m.onSetHwDb) m.onSetHwDb(hp,db); }
    else if (cmd=="setBufferSize") {
        int value=intVal("\"value\"",0);
        if(value>0 && m.onSetBufferSize) {
            const bool accepted = m.onSetBufferSize((long)value);
            wprintf(L"[bridge] setBufferSize %d %s\n", value, accepted ? L"accepted" : L"ignored");
        } else {
            wprintf(L"[bridge] setBufferSize %d rejected (invalid)\n", value);
        }
    }
    else if (cmd=="ping") { }
    else if (cmd=="setVirtualMicSource") { int ch=intVal("\"ch\"",0); if(m.setVirtualMicSource) m.setVirtualMicSource(ch); }
    else if (cmd=="setSystemMute") {
        bool on=boolVal("\"on\"");
        if(m.setSystemMute) m.setSystemMute(on);
        wprintf(L"[bridge] setSystemMute %s\n", on ? L"on" : L"off");
    }
    else if (cmd=="setCloseToTray") { bool on=boolVal("\"on\""); if(m.setCloseToTray) m.setCloseToTray(on); }
    else if (cmd=="setStartMinimized") { bool on=boolVal("\"on\""); if(m.setStartMinimized) m.setStartMinimized(on); }
    else if (cmd=="setUiMode") {
        int mode=intVal("\"mode\"",0);
        if(mode != 0 && mode != 1) mode = 0;
        if(st->uiMode != mode){
            st->uiMode = mode;
            if(m.setUiMode) m.setUiMode(mode);
            ApplyModeSize(st);
        }
    }
    else if (cmd=="setMainRows") {
        int rows=intVal("\"rows\"",1);
        if(rows < 1) rows = 1;
        if(rows > 3) rows = 3;
        if(st->mainRows != rows){
            st->mainRows = rows;
            if(st->uiMode == 0) ApplyModeSize(st); // Main hugs the shared insert rows
        }
    }
    (void)st;
}
LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<CtrlState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch(m){
    case WM_CREATE: return 0;
    case WM_GETMINMAXINFO: {
        // Fixed-size console: lock track size to the active mode (no user resize,
        // no maximize) while still scaling with monitor DPI.
        auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
        const int mode = st ? st->uiMode : 0;
        const double scale = (st && st->rasterScale > 0.0)
            ? st->rasterScale
            : (static_cast<double>(WindowDpi(h)) / 96.0);
        RECT rc{0,0,(LONG)std::lround(ModeClientWidth(mode) * scale),
                    (LONG)std::lround(ModeClientHeight(mode, st ? st->mainRows : 1) * scale)};
        const DWORD style = (DWORD)GetWindowLongPtrW(h, GWL_STYLE);
        const DWORD ex = (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE);
        AdjustWindowRectEx(&rc, style, FALSE, ex);
        mmi->ptMinTrackSize.x = rc.right - rc.left;
        mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        mmi->ptMaxTrackSize.x = rc.right - rc.left;
        mmi->ptMaxTrackSize.y = rc.bottom - rc.top;
        return 0;
    }
    case WM_SYSCOMMAND: {
        // Reject maximize and user size gestures even if a style bit slips in.
        const UINT sc = (UINT)w & 0xFFF0u;
        if (sc == SC_MAXIMIZE || sc == SC_SIZE || sc == SC_RESTORE) return 0;
        break;
    }
    case WM_SIZE: {
        if (st && st->webHost) {
            RECT rc{}; GetClientRect(h,&rc);
            SetWindowPos(st->webHost,nullptr,0,0,rc.right-rc.left,rc.bottom-rc.top,SWP_NOZORDER|SWP_NOACTIVATE);
        }
        return 0;
    }
    case WM_TIMER:
        if (w==kTimerId && st && !st->destroyed) {
            const auto now = std::chrono::steady_clock::now();
            if (st->hasLastTick) {
                const double gapMs = std::chrono::duration<double, std::milli>(now - st->lastTick).count();
                if (gapMs > 150.0) {
                    wprintf(L"[ui] heartbeat stall %.0f ms\n", gapMs);
                }
            }
            st->lastTick = now;
            st->hasLastTick = true;
            auto js = buildSnapshotJson(st->model);
            DailyWebViewHost_PostJson(st->webHost, js);
        }
        return 0;
    case WM_CLOSE:
        // Daily mode: hide to the tray; the audio engine keeps running. The
        // "Close to tray" preference is read live so toggling it takes effect
        // without a restart.
        if (st && (st->shouldHideOnClose ? st->shouldHideOnClose() : st->hideOnClose)) {
            ShowWindow(h, SW_HIDE);
            if (st->onHidden) st->onHidden();
            return 0;
        }
        break; // close-to-tray off (or diagnostic/--seconds): normal close -> WM_DESTROY
    case WM_DESTROY:
        if (st) {
            KillTimer(h,kTimerId);
            if (st->webHost) DailyWebViewHost_Destroy(st->webHost);
            st->webHost=nullptr;
            st->destroyed=true;
            delete st;
            SetWindowLongPtrW(h,GWLP_USERDATA,0);
        }
        return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
void EnsureContainerClass(HINSTANCE inst){
    static bool done=false; if(done) return;
    WNDCLASSW wc{}; wc.hInstance=inst; wc.lpszClassName=kClass; wc.lpfnWndProc=WndProc; wc.hCursor=LoadCursorW(nullptr,(LPCWSTR)IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    RegisterClassW(&wc); done=true;
}
}
HWND DailyWebViewController_Create(HINSTANCE inst, const DailyWebViewControllerConfig& cfg){
    // Presentation layer. The host window is Per-Monitor DPI aware and FIXED size
    // (no drag-resize, no maximize); each view mode has its own logical DIP size
    // and physical size = DIP * (monitor_dpi/96).
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    EnsureContainerClass(inst);
    const int initialMode = cfg.model.getUiMode ? cfg.model.getUiMode() : 0;
    const UINT dpi = GetDpiForSystem() ? GetDpiForSystem() : 96;
    const double dpiScale = static_cast<double>(dpi) / 96.0;
    RECT frame{0,0,(LONG)std::lround(ModeClientWidth(initialMode) * dpiScale),
                   (LONG)std::lround(ModeClientHeight(initialMode, 1) * dpiScale)};
    AdjustWindowRectEx(&frame, kFixedFrameStyle, FALSE, 0);
    HWND c = CreateWindowExW(0,kClass,L"",kFixedFrameStyle,
                             80,80,frame.right-frame.left,frame.bottom-frame.top,
                             nullptr,nullptr,inst,nullptr);
    if(!c) return nullptr;
    ApplyDarkTitleBar(c);
    auto* st=new CtrlState();
    st->model=cfg.model;
    st->htmlDir=cfg.htmlDir;
    st->userDataDir=cfg.userDataDir;
    st->container=c;
    st->uiMode=initialMode;
    st->rasterScale=dpiScale; // provisional until WebView2 reports its scale
    st->hideOnClose=cfg.hideOnClose;
    st->shouldHideOnClose=cfg.shouldHideOnClose;
    st->onHidden=cfg.onHidden;
    SetWindowLongPtrW(c,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(st));
    DailyWebViewHostConfig hc{}; hc.htmlDir=cfg.htmlDir; hc.userDataDir=cfg.userDataDir;
    hc.onMessage=[st](const std::wstring& msg){ handleMessage(st, msg); };
    // Track DPI changes: re-apply the fixed size for the active mode (the layout
    // is not resizable and must not be bitmap-stretched).
    hc.onRasterizationScale=[st](double s){
        if (st == nullptr || s <= 0.0 || st->container == nullptr) return;
        st->rasterScale = s;
        ApplyModeSize(st);
    };
    st->webHost = DailyWebViewHost_Create(inst, c, hc);
    RECT rc{}; GetClientRect(c,&rc);
    if(st->webHost) SetWindowPos(st->webHost,nullptr,0,0,rc.right-rc.left,rc.bottom-rc.top,SWP_NOZORDER|SWP_NOACTIVATE);
    SetTimer(c,kTimerId,kTimerMs,nullptr);
    return c;
}
void DailyWebViewController_Destroy(HWND hwnd){ if(hwnd) DestroyWindow(hwnd); }
void DailyWebViewController_Show(HWND hwnd){
    if(!hwnd) return;
    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}
}
