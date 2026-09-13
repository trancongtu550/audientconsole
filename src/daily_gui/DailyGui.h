#pragma once
#include <windows.h>
#include <functional>
#include <string>
#include <vector>
#include <optional>
#include <utility>

namespace audient::daily_gui
{

struct DailyGuiConfig
{
    std::function<bool()> getGlobalMonitorOn;
    std::function<void(bool)> setGlobalMonitorOn;
    std::function<bool()> getMonoOn;
    std::function<void(bool)> setMonoOn;
    std::function<bool(int)> getLocalEnable;
    std::function<void(int,bool)> setLocalEnable;
    std::function<bool(int)> getLocalMute;
    std::function<void(int,bool)> setLocalMute;
    std::function<float(int)> getLocalGainDb;
    std::function<void(int,float)> setLocalGainDb;
    std::function<std::pair<float,float>()> getMetersCh1;
    std::function<std::pair<float,float>()> getMetersCh2;
    std::function<std::vector<std::string>()> getCh1Names;
    std::function<std::vector<std::string>()> getCh2Names;
    std::function<int()> getCh1Sel;
    std::function<int()> getCh2Sel;
    std::function<void(int,int)> setSel;
    std::function<void(int)> onLoad;
    std::function<void(int,int)> onOpenEditor;
    std::function<void(int,int)> onBypassSel;
    std::function<void(int)> onBypassWhole;
    std::function<bool(int)> getWholeBypass;
    std::function<void(int,int)> onRemove;
    std::function<std::string()> getAsioText;
    std::function<std::string()> getXrunsText;
    // Pico v0.1.1 virtual-mic transport status (published cached state; the UI
    // never blocks on USB/WASAPI work).
    std::function<bool()> getPicoConnected;
    std::function<std::string()> getPicoText;
    // ADR-011 selectable single-source virtual microphone (0 = Input 1, 1 = Input 2).
    std::function<int()> getVirtualMicSource;
    std::function<void(int)> setVirtualMicSource;
    // User preferences (persisted; presentation/behavior only).
    std::function<bool()> getCloseToTray;
    std::function<void(bool)> setCloseToTray;
    std::function<bool()> getStartMinimized;
    std::function<void(bool)> setStartMinimized;
    // About / System Info.
    std::function<std::string()> getAppVersion;
    // User-facing audio-device status (from the active ASIO session / selected
    // Audient device) and the SEPARATE hardware-control channel status.
    std::function<std::string()> getAudioDeviceStatus;
    std::function<std::string()> getHardwareControlStatus;
    // Audio settings: sample-rate is driver-controlled (reflected read-only),
    // buffer size is user-selectable within the driver's actual range.
    std::function<long()> getAudioCurrentRate;
    std::function<long()> getAudioActualRate;
    std::function<long()> getAudioCurrentBuffer;
    std::function<long()> getAudioRequestedBuffer;
    std::function<std::vector<long>()> getAudioSupportedRates;
    std::function<std::vector<long>()> getAudioSupportedBuffers;
    std::function<bool()> getAudioRebuilding;
    std::function<bool(long)> onSetBufferSize;
    std::function<std::optional<double>()> getHwMonDb;
    std::function<std::optional<double>()> getHwHpDb;
    std::function<std::string()> getHwMonText;
    std::function<std::string()> getHwHpText;
    std::function<void(bool,double)> onNudgeHw;
    std::function<void(bool,double)> onSetHwDb;
    // Presentation-only view mode (0 = Main Mixer, 1 = Mini Monitor). Persisted
    // by the app; the host also uses it to size the fixed window.
    std::function<int()> getUiMode;
    std::function<void(int)> setUiMode;
    // Stereo SYSTEM output peak (linear 0..1) for the Mini Monitor: independent
    // L/R from the active iD14 render endpoint's per-channel peaks. -1 = endpoint
    // unavailable (UI shows "--"). Presentation only; never from the ASIO callback.
    std::function<float()> getSystemPeakL;
    std::function<float()> getSystemPeakR;
    // Windows SYSTEM render-endpoint mute (IAudioEndpointVolume, endpoint-wide -
    // NOT independent Speaker/Headphone mute). nullopt = endpoint/mute API
    // unavailable. The published state is read from Windows; the setter only
    // requests a write. Runtime endpoint state only - never persisted.
    std::function<std::optional<bool>()> getSystemMuted;
    std::function<void(bool)> setSystemMute;
    bool dualMode = false;
    float userScale = 1.0f;
    std::function<void(float)> onUserScaleChanged;
};

HWND Create(HINSTANCE hInst, const DailyGuiConfig& cfg);
void Destroy(HWND hwnd);

}
