#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DailyWebViewHost.h"
#include <wrl/client.h>
#include <wrl/event.h>
#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>
#include <cstdio>
#include <string>
#include <atomic>
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;
namespace audient::daily_gui {
namespace {
struct HostState {
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> ctrl;
    ComPtr<ICoreWebView2Controller3> ctrl3;
    ComPtr<ICoreWebView2> web;
    HWND host{nullptr};
    std::wstring htmlDir;
    std::wstring userDataDir;
    std::wstring fatalError;
    std::function<void(const std::wstring&)> onMessage;
    std::function<void(double)> onScale;
    std::atomic<bool> destroyed{false};
    EventRegistrationToken msgToken{};
    EventRegistrationToken scaleToken{};
    bool hasMsgToken{false};
    bool hasScaleToken{false};
};
constexpr wchar_t kHostClass[] = L"AudientDailyWebHost";

// Production fail-safe: never leave a blank white host. Sets a visible error
// message (painted by HostWndProc) and logs the reason. Used for environment,
// controller, and navigation failures only.
void showFatal(HostState* st, const wchar_t* message)
{
    if (st == nullptr)
    {
        return;
    }
    st->fatalError = message != nullptr ? message : L"WebView2 failed to start.";
    std::printf("[webview] FATAL: %ls\n", st->fatalError.c_str());
    if (st->host != nullptr) {
        InvalidateRect(st->host, nullptr, TRUE);
        UpdateWindow(st->host);
    }
}

LRESULT CALLBACK HostWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<HostState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_SIZE) {
        if (st && st->ctrl) {
            RECT rc{}; GetClientRect(h, &rc);
            st->ctrl->put_Bounds(rc);
        }
        return 0;
    }
    if (m == WM_ERASEBKGND) {
        RECT rc{}; GetClientRect(h, &rc);
        FillRect(reinterpret_cast<HDC>(w), &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT rc{}; GetClientRect(h, &rc);
        if (st != nullptr && !st->fatalError.empty()) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(235, 235, 240));
            DrawTextW(dc, st->fatalError.c_str(), -1, &rc,
                      DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) {
        if (st) {
            st->destroyed.store(true, std::memory_order_relaxed);
            if (st->web && st->hasMsgToken) st->web->remove_WebMessageReceived(st->msgToken);
            if (st->ctrl3 && st->hasScaleToken) st->ctrl3->remove_RasterizationScaleChanged(st->scaleToken);
            if (st->ctrl) st->ctrl->Close();
            st->ctrl3.Reset();
            st->ctrl.Reset();
            st->web.Reset();
            st->env.Reset();
            delete st;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
void EnsureClass(HINSTANCE inst) {
    static bool done = false;
    if (done) return;
    WNDCLASSW wc{}; wc.hInstance = inst; wc.lpszClassName = kHostClass; wc.lpfnWndProc = HostWndProc; wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    done = true;
}

// Creates the WebView2 environment/controller for `dataDir`. On environment
// failure, retries once with a sibling fallback user-data folder (handles a
// locked/shared primary folder); if that also fails the host renders a clear
// error instead of a blank white window.
void createEnvironment(HWND host, HostState* st, const std::wstring& dataDir,
                       bool allowFallback) {
    auto opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, dataDir.c_str(), opts.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [host, st, dataDir, allowFallback](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (st->destroyed.load(std::memory_order_relaxed)) return S_OK;
                if (FAILED(result) || !env) {
                    std::printf("[webview] environment creation failed hr=0x%08lX dir=%ls\n",
                                (unsigned long)result, dataDir.c_str());
                    if (allowFallback) {
                        const std::wstring fallback = dataDir + L"-fallback";
                        std::printf("[webview] retrying with fallback user-data dir=%ls\n", fallback.c_str());
                        createEnvironment(host, st, fallback, false);
                    } else {
                        showFatal(st, L"WebView2 runtime failed to start (environment creation failed).\n"
                                      L"Close any other Audient Console instances and retry.");
                    }
                    return S_OK;
                }
                st->env = env;
                env->CreateCoreWebView2Controller(host,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [host, st](HRESULT r2, ICoreWebView2Controller* c) -> HRESULT {
                            if (st->destroyed.load(std::memory_order_relaxed)) return S_OK;
                            if (FAILED(r2) || !c) {
                                std::printf("[webview] controller creation failed hr=0x%08lX\n",
                                            (unsigned long)r2);
                                showFatal(st, L"WebView2 controller failed to start.\nRetry after closing other instances.");
                                return S_OK;
                            }
                            st->ctrl = c;
                            ComPtr<ICoreWebView2> wv;
                            c->get_CoreWebView2(&wv);
                            st->web = wv;
                            if (!wv) return S_OK;
                            RECT rc{}; GetClientRect(host, &rc); c->put_Bounds(rc);
                            c->put_IsVisible(TRUE);
                            ComPtr<ICoreWebView2Controller3> c3;
                            if (SUCCEEDED(c->QueryInterface(IID_PPV_ARGS(&c3))) && c3)
                            {
                                st->ctrl3 = c3;
                                double rasterScale = 1.0;
                                if (SUCCEEDED(c3->get_RasterizationScale(&rasterScale)) && rasterScale > 0.0 && st->onScale)
                                {
                                    st->onScale(rasterScale);
                                }
                                if (st->onScale)
                                {
                                    ComPtr<ICoreWebView2Controller3> c3keep = c3;
                                    c3->add_RasterizationScaleChanged(
                                        Callback<ICoreWebView2RasterizationScaleChangedEventHandler>(
                                            [st, c3keep](ICoreWebView2Controller* sender, IUnknown*) -> HRESULT {
                                                (void)c3keep;
                                                if (st && !st->destroyed.load(std::memory_order_relaxed) && st->onScale)
                                                {
                                                    double s = 1.0;
                                                    if (sender)
                                                    {
                                                        ComPtr<ICoreWebView2Controller3> cc;
                                                        if (SUCCEEDED(sender->QueryInterface(IID_PPV_ARGS(&cc))) && cc &&
                                                            SUCCEEDED(cc->get_RasterizationScale(&s)) && s > 0.0)
                                                        {
                                                            st->onScale(s);
                                                        }
                                                    }
                                                }
                                                return S_OK;
                                            }).Get(), &st->scaleToken);
                                    st->hasScaleToken = true;
                                }
                            }
                            wv->AddScriptToExecuteOnDocumentCreated(L"window.__audientHostReady=true;", nullptr);
                            wv->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                [st](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
                                    if (!st || st->destroyed.load(std::memory_order_relaxed)) return S_OK;
                                    BOOL ok = FALSE;
                                    COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                                    if (a) { a->get_IsSuccess(&ok); a->get_WebErrorStatus(&status); }
                                    if (!ok) {
                                        std::printf("[webview] navigation failed webErrorStatus=%d\n", (int)status);
                                        showFatal(st, L"WebView2 failed to load the UI.\nSee the log for details.");
                                    }
                                    return S_OK;
                                }).Get(), nullptr);
                            wv->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [st](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                    if (!st || st->destroyed.load(std::memory_order_relaxed)) return S_OK;
                                    if (!st->onMessage) return S_OK;
                                    LPWSTR msg = nullptr;
                                    if (SUCCEEDED(args->TryGetWebMessageAsString(&msg)) && msg) {
                                        std::wstring s(msg);
                                        CoTaskMemFree(msg);
                                        st->onMessage(s);
                                    }
                                    return S_OK;
                                }).Get(), &st->msgToken);
                            st->hasMsgToken = true;
                            std::wstring url = L"file:///" + st->htmlDir + L"/index.html";
                            for (auto& ch : url) if (ch == L'\\') ch = L'/';
                            wv->Navigate(url.c_str());
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    (void)hr;
}
}
HWND DailyWebViewHost_Create(HINSTANCE inst, HWND parent, const DailyWebViewHostConfig& cfg) {
    EnsureClass(inst);
    RECT pr{}; if (parent) GetClientRect(parent, &pr);
    HWND host = CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, pr.right - pr.left, pr.bottom - pr.top, parent, nullptr, inst, nullptr);
    if (!host) return nullptr;
    auto* st = new HostState();
    st->host = host;
    st->htmlDir = cfg.htmlDir;
    st->userDataDir = cfg.userDataDir;
    st->onMessage = cfg.onMessage;
    st->onScale = cfg.onRasterizationScale;
    SetWindowLongPtrW(host, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
    createEnvironment(host, st, st->userDataDir, /*allowFallback=*/true);
    return host;
}
void DailyWebViewHost_Destroy(HWND host) {
    if (!host) return;
    DestroyWindow(host);
}
bool DailyWebViewHost_PostJson(HWND host, const std::wstring& json) {
    if (!host) return false;
    auto* st = reinterpret_cast<HostState*>(GetWindowLongPtrW(host, GWLP_USERDATA));
    if (!st || !st->web) return false;
    std::wstring script = L"window.__audientOnSnapshot && window.__audientOnSnapshot(" + json + L")";
    st->web->ExecuteScript(script.c_str(), nullptr);
    return true;
}
bool DailyWebViewHost_Navigate(HWND host, const std::wstring& url) {
    if (!host) return false;
    auto* st = reinterpret_cast<HostState*>(GetWindowLongPtrW(host, GWLP_USERDATA));
    if (!st || !st->web) return false;
    return SUCCEEDED(st->web->Navigate(url.c_str()));
}
bool DailyWebViewHost_SetZoom(HWND host, double zoom) {
    if (!host) return false;
    auto* st = reinterpret_cast<HostState*>(GetWindowLongPtrW(host, GWLP_USERDATA));
    if (!st || !st->ctrl) return false;
    return SUCCEEDED(st->ctrl->put_ZoomFactor(zoom));
}
std::wstring DailyWebViewHost_RuntimeVersion() {
    LPWSTR version = nullptr;
    if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr, &version)) && version != nullptr) {
        std::wstring result(version);
        CoTaskMemFree(version);
        return result;
    }
    return L"--";
}
}
