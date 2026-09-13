#pragma once
#include <windows.h>
#include <functional>
#include <utility>
#include <optional>
#include <string>
#include <vector>

struct DiagGuiConfig
{
    std::function<bool()> getGlobalMonitorOn;
    std::function<void(bool)> setGlobalMonitorOn;
    std::function<bool()> getMonoOn;
    std::function<void(bool)> setMonoOn;
    std::function<bool(int)> getLocalEnable;
    std::function<void(int, bool)> setLocalEnable;
    std::function<bool(int)> getLocalMute;
    std::function<void(int, bool)> setLocalMute;
    std::function<float(int)> getLocalGainDb;
    std::function<void(int, float)> setLocalGainDb;
    std::function<std::pair<float, float>()> getMetersCh1;
    std::function<std::pair<float, float>()> getMetersCh2;
    std::function<std::vector<std::string>()> getCh1Names;
    std::function<std::vector<std::string>()> getCh2Names;
    std::function<int()> getCh1Sel;
    std::function<int()> getCh2Sel;
    std::function<void(int, int)> setSel;
    std::function<void(int)> onLoad;
    std::function<void(int, int)> onOpenEditor;
    std::function<void(int, int)> onBypassSel;
    std::function<void(int)> onBypassWhole;
    std::function<bool(int)> getWholeBypass;
    std::function<void(int, int)> onRemove;
    std::function<std::string()> getAsioText;
    std::function<std::string()> getXrunsText;
    std::function<std::optional<double>()> getHwMonDb;
    std::function<std::optional<double>()> getHwHpDb;
    std::function<std::string()> getHwMonText;
    std::function<std::string()> getHwHpText;
    std::function<void(bool, double)> onNudgeHw;
    std::function<void(bool, double)> onSetHwDb;
    bool dualMode = false;
};

HWND DiagGui_Create(HINSTANCE hInst, const DiagGuiConfig& cfg);
void DiagGui_Destroy(HWND hwnd);
