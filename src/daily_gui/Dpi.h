#pragma once
#include <windows.h>
#include <shellscalingapi.h>
#include <cmath>

namespace audient::daily_gui
{

inline int dpiForWindow(HWND hwnd)
{
    UINT d = GetDpiForWindow(hwnd);
    return d ? (int)d : 96;
}
inline int dpiForMonitor(HMONITOR m)
{
    UINT dx = 96, dy = 96;
    if (SUCCEEDED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy))) return (int)dx;
    return 96;
}
inline float scaleForDpi(int dpi) { return (float)dpi / 96.0f; }

struct DpiMetrics
{
    float dpiScale = 1.0f;
    float userScale = 1.0f;
    float total() const { return dpiScale * userScale; }
    int px(int dip) const { return (int)std::lround((float)dip * total()); }
};

inline RECT scaleRect(const RECT& dip, float s)
{
    RECT r;
    r.left = (int)std::lround(dip.left * s);
    r.top = (int)std::lround(dip.top * s);
    r.right = (int)std::lround(dip.right * s);
    r.bottom = (int)std::lround(dip.bottom * s);
    return r;
}

}
