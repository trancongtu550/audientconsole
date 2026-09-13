#pragma once
#include <windows.h>
namespace audient::daily_gui
{
struct Theme
{
    COLORREF bg = RGB(18, 20, 24);
    COLORREF panel = RGB(32, 35, 42);
    COLORREF panelHi = RGB(42, 46, 56);
    COLORREF panelLo = RGB(26, 29, 36);
    COLORREF border = RGB(52, 56, 66);
    COLORREF borderHi = RGB(68, 72, 84);
    COLORREF text = RGB(235, 237, 241);
    COLORREF textDim = RGB(150, 156, 168);
    COLORREF textDim2 = RGB(118, 124, 140);
    COLORREF amber = RGB(228, 162, 48);
    COLORREF amberHi = RGB(255, 195, 88);
    COLORREF amberLo = RGB(160, 112, 32);
    COLORREF green = RGB(68, 212, 120);
    COLORREF yellow = RGB(252, 212, 48);
    COLORREF red = RGB(242, 88, 88);
    COLORREF muteRed = RGB(208, 48, 48);
    COLORREF bypassBlue = RGB(84, 138, 228);
    COLORREF ledOff = RGB(64, 68, 78);
};
inline COLORREF blend(COLORREF a, COLORREF b, int t)
{
    int r = (GetRValue(a)*(100-t)+GetRValue(b)*t)/100;
    int g = (GetGValue(a)*(100-t)+GetGValue(b)*t)/100;
    int bl=(GetBValue(a)*(100-t)+GetBValue(b)*t)/100;
    return RGB(r,g,bl);
}
HFONT createFont(int px, bool bold, const wchar_t* name = L"Segoe UI");
void deleteFont(HFONT f);
}
