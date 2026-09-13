#include "DailyTheme.h"
HFONT audient::daily_gui::createFont(int px, bool bold, const wchar_t* name)
{
    return CreateFontW(-px, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, name);
}
void audient::daily_gui::deleteFont(HFONT f)
{
    if (f) DeleteObject(f);
}
