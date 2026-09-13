// Slice P — ASIO <-> RoutingCore adapter demo (manual hardware test).
//
// Purpose: run the REAL processed path on the target hardware
//   iD14 MK1 mic input -> RoutingCore -> input VST3 chain -> processed mic
//   -> processedMicMonitor render branch -> iD14 Output 1/2
// so the processed path (an EQ/compress effect on the mic chain) is audibly
// verified on the monitor output. The system downlink is muted here (no
// Windows shared-mode mix until Phase 7); the downlink path exists and is
// exercised by the automated simulate-driver tests.
//
// NOT production UI: a thin interactive console harness on top of
// AsioRoutingAdapter, matching the `audient_console_demo` UX (plugin chooser,
// add/remove/bypass, native editor) but on the routing-core render path.
//   --vst3 <path>     one mic-chain VST3 (EQ/Comp/etc) at startup
//   --vst3out <path>  one output-chain VST3 (optional, non-interactive)
//   --vst3dir <dir>   directory scanned for the interactive chooser
//   --seconds N       auto-exit after N seconds (non-interactive)
//   Keys: '+'/'-' output level, 'm' monitor mute, 'p' panic mute,
//         'a' add plugin, 'r' remove last, 'b' bypass last, 'B' whole-chain
//         bypass, 'e' open/close native editor, 'q' quit (graceful fade-out).

#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/AsioRoutingAdapter.h"
#include "asio/sdk/NativeAsioDriverProvider.h"
#include "engine/GraphConfig.h"
#include "vst3/Vst3Chain.h"
#include "vst3/Vst3Editor.h"
#include "vst3/Vst3ModuleLoader.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3Processor.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>

namespace
{

bool containsInsensitive(const std::string& haystack, const char* needle)
{
    std::string lowered = haystack;
    for (char& ch : lowered)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    std::string target = needle;
    for (char& ch : target)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lowered.find(target) != std::string::npos;
}

audient::asio::ChannelPlan planFromCapabilities(const audient::asio::DriverCapabilities& caps)
{
    std::vector<audient::asio::ChannelInfo> inputs;
    std::vector<audient::asio::ChannelInfo> outputs;
    for (const audient::asio::ChannelInfo& channel : caps.channels)
    {
        (channel.isInput ? inputs : outputs).push_back(channel);
    }
    return audient::asio::AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
}

float dBToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

// Owned module bundle for one prepared effect. Everything must stay alive for
// the whole run (the chain holds borrowed processor pointers).
struct PluginSlot
{
    std::unique_ptr<audient::vst3::Vst3ModuleLoader> loader;
    std::unique_ptr<audient::vst3::Vst3Host> host;
    std::unique_ptr<audient::vst3::Vst3Processor> processor;
    std::string name;
    bool bypassStatus = false; // mic-chain slot bypass (status copy for rebuild)
};

// Top-level window hosting a Vst3Editor. The plug-in view is a child of this
// window; Vst3Editor owns that child. WM_CLOSE asks the demo to close the
// editor; the demo calls Vst3Editor::close to detach the plug-in view.
const wchar_t kEditorHostClass[] = L"AudientConsoleRoutingVst3EditorHost";

struct EditorHostState
{
    audient::vst3::Vst3Editor* editor = nullptr;
    bool wantClose = false;
    HWND hwnd = nullptr;
};

LRESULT CALLBACK editorHostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE)
    {
        const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    EditorHostState* state = reinterpret_cast<EditorHostState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message)
    {
    case WM_CLOSE:
        if (state != nullptr)
        {
            state->wantClose = true;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1; // plug-in paints its own child; avoid parent flicker
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

// Registers the editor host window class once (idempotent).
bool ensureEditorHostClass()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &editorHostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kEditorHostClass;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// Reads a line from the console using _getch/_putch (echoed), so interactive
// menus do not compete with the key pump (_getch/_kbhit) nor get overwritten
// by the status thread. Bare Enter returns an empty string.
std::string readLineFromConsole()
{
    std::string line;
    for (;;)
    {
        const int ch = _getch();
        if (ch == '\r' || ch == '\n')
        {
            std::printf("\n");
            std::fflush(stdout);
            break;
        }
        if (ch == '\b')
        {
            if (!line.empty())
            {
                line.pop_back();
                std::printf("\b \b");
                std::fflush(stdout);
            }
            continue;
        }
        if (ch >= 32 && ch < 127)
        {
            line.push_back(static_cast<char>(ch));
            _putch(ch);
            std::fflush(stdout);
        }
        // Ignore function/arrow keys and other control codes.
    }
    return line;
}

// Realtime status line: pins a single row to the bottom of the console.
class StatusLine
{
public:
    void write(const std::string& text) const
    {
        const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (handle == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(handle, &info))
        {
            std::printf("\r%s\n", text.c_str());
            std::fflush(stdout);
            return;
        }
        const SHORT width = info.dwSize.X;
        if (width <= 1)
        {
            return;
        }
        const SHORT row = info.srWindow.Bottom;
        std::string line = text;
        const std::size_t maxColumns = static_cast<std::size_t>(width) - 1;
        if (line.size() > maxColumns)
        {
            line.resize(maxColumns);
        }
        else if (line.size() < maxColumns)
        {
            line.append(maxColumns - line.size(), ' ');
        }
        const COORD statusPos{0, row};
        SetConsoleCursorPosition(handle, statusPos);
        DWORD written = 0;
        FillConsoleOutputCharacterA(handle, ' ', static_cast<DWORD>(width - 1), statusPos, &written);
        WriteConsoleA(handle, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
        SetConsoleCursorPosition(handle, info.dwCursorPosition);
        std::fflush(stdout);
    }
};

} // namespace

int main(int argc, char** argv)
{
    // The native editor is a plug-in window; opt into per-monitor DPI so plug-in
    // views render at physical pixels (project guidelines §15, same as the main demo).
    if (auto* setDpi = reinterpret_cast<BOOL(WINAPI*)(void*)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))
    {
        setDpi(reinterpret_cast<void*>(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    }
    else if (auto* setDpi2 = reinterpret_cast<HRESULT(WINAPI*)(int)>(
                 GetProcAddress(GetModuleHandleW(L"shcore.dll"), "SetProcessDpiAwareness")))
    {
        setDpi2(2); // PROCESS_PER_MONITOR_DPI_AWARE
    }
    else
    {
        SetProcessDPIAware();
    }
    SetConsoleTitleA("Audient Console - ASIO <> RoutingCore adapter demo");

    std::string vst3MicPath;
    std::string vst3OutPath;
    std::string vst3Dir = "C:\\Program Files\\Common Files\\VST3";
    long autoExitSeconds = 0;
    for (int i = 1; i < argc - 1; ++i)
    {
        if (strcmp(argv[i], "--vst3") == 0)
        {
            vst3MicPath = argv[i + 1];
        }
        else if (strcmp(argv[i], "--vst3out") == 0)
        {
            vst3OutPath = argv[i + 1];
        }
        else if (strcmp(argv[i], "--vst3dir") == 0)
        {
            vst3Dir = argv[i + 1];
        }
        else if (strcmp(argv[i], "--seconds") == 0 || strcmp(argv[i], "-s") == 0)
        {
            autoExitSeconds = std::strtol(argv[i + 1], nullptr, 10);
        }
    }

    std::printf("Audient Console routing adapter demo (Slice P)\n");
    std::printf("  prerequisites: Audient ASIO driver + iD14 connected, 48 kHz / 64 samples\n\n");

    audient::asio::NativeAsioDriverProvider provider;
    audient::asio::DriverIdentity match;
    for (const audient::asio::DriverIdentity& candidate : provider.available())
    {
        if (containsInsensitive(candidate.name, "audient"))
        {
            match = candidate;
            break;
        }
    }
    if (match.clsid.empty())
    {
        std::printf("ERROR: no Audient ASIO driver found in the registry.\n");
        return 1;
    }
    std::printf("driver: %s  ({%s})\n", match.name.c_str(), match.clsid.c_str());

    audient::asio::AsioBackend backend(provider);
    std::string error;
    if (!backend.selectAudientDevice(error))
    {
        std::printf("ERROR: selectAudientDevice failed: %s\n", error.c_str());
        return 1;
    }

    const audient::asio::DriverCapabilities caps = backend.driverCapabilities();
    std::printf("CAPS: %ld in / %ld out\n", caps.inputChannels, caps.outputChannels);
    const audient::asio::ChannelPlan plan = planFromCapabilities(caps);
    if (!plan.valid())
    {
        std::printf("ERROR: channel plan invalid (mic input / stereo output mapping failed).\n");
        return 1;
    }

    long buffer = 64;
    if (!backend.configure(48000, buffer, plan, error))
    {
        std::printf("configure(48000,64) failed: %s -- falling back to 128\n", error.c_str());
        buffer = 128;
        if (!backend.configure(48000, buffer, plan, error))
        {
            std::printf("ERROR: configure(48000,128) failed too: %s\n", error.c_str());
            return 1;
        }
    }
    std::printf("CONFIG: 48 kHz / %ld samples / mic-in=%ld outL=%ld outR=%ld\n", buffer, plan.micInput,
                plan.outputLeft, plan.outputRight);

    constexpr std::size_t kMaxBlock = 256;
    const long kMaxBlockLong = static_cast<long>(kMaxBlock);

    // Interactive mic chain (chooser + add/remove/bypass + editor), matching the
    // main demo UX but on the routing-core adapter path. Rebuilds publish a new
    // Vst3Chain snapshot (atomic swap, realtime-safe); the callback only reads
    // the published snapshot.
    std::vector<std::string> vst3Candidates;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(vst3Dir, std::filesystem::directory_options::skip_permission_denied))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".vst3")
        {
            vst3Candidates.push_back(entry.path().string());
        }
    }
    if (vst3Candidates.empty())
    {
        std::printf("[vst3] no .vst3 found under %s\n", vst3Dir.c_str());
    }

    audient::vst3::Vst3Chain micChain;
    micChain.publish({}, false);
    micChain.configure(kMaxBlock);
    audient::vst3::Vst3Chain outChain;
    outChain.publish({}, false);
    outChain.configure(kMaxBlock);

    std::vector<PluginSlot> pluginSlots;   // mic chain, owned here
    std::vector<PluginSlot> retiredSlots;  // destroyed once the chain settles
    bool micChainWholeBypass = false;
    std::atomic<std::size_t> slotCount{0};       // status copy for the UI thread
    std::atomic<bool> chainBypassStatus{false};  // status copy for the UI thread

    const auto rebuildMicChain = [&]() {
        std::vector<audient::vst3::Vst3Chain::Slot> chainSlots;
        for (const PluginSlot& slot : pluginSlots)
        {
            // The chain reads each processor's negotiated layout (VST-006
            // mono<->stereo adapter handles layout-mismatched slots).
            chainSlots.push_back({slot.processor.get(), slot.bypassStatus});
        }
        if (chainSlots.size() > audient::vst3::Vst3Chain::kMaxSlots)
        {
            std::printf("[vst3] WARN: mic chain full (%zu slots)\n", chainSlots.size());
        }
        micChain.publish(std::move(chainSlots), micChainWholeBypass);
        micChain.reap();
        slotCount.store(pluginSlots.size(), std::memory_order_relaxed);
        chainBypassStatus.store(micChainWholeBypass, std::memory_order_relaxed);
    };

    const auto addPluginFromPath = [&](const std::string& path) -> int {
        if (pluginSlots.size() >= audient::vst3::Vst3Chain::kMaxSlots)
        {
            std::printf("[vst3] mic chain full (%zu slots)\n", pluginSlots.size());
            return -1;
        }
        try
        {
            PluginSlot slot;
            slot.loader = std::make_unique<audient::vst3::Vst3ModuleLoader>();
            std::printf("[vst3] loading %s ...\n", path.c_str());
            if (!slot.loader->load(path))
            {
                std::printf("WARN: plugin load failed: %s (slot not added)\n", slot.loader->lastError().c_str());
                return -1;
            }
            slot.host = std::make_unique<audient::vst3::Vst3Host>();
            if (!slot.host->attachFactory(slot.loader->factory()))
            {
                std::printf("WARN: factory attach failed (slot not added)\n");
                return -1;
            }
            const std::vector<audient::vst3::PluginClassInfo> classes = slot.host->classes();
            const audient::vst3::PluginClassInfo* effect = nullptr;
            for (const audient::vst3::PluginClassInfo& cls : classes)
            {
                if (cls.isEffect())
                {
                    effect = &cls;
                    break;
                }
            }
            if (effect == nullptr)
            {
                std::printf("WARN: no Audio Effect class found in module (slot not added)\n");
                return -1;
            }
            slot.processor = slot.host->createEffectProcessor(*effect, 48000.0, kMaxBlockLong,
                                                              audient::vst3::BusLayout::Mono);
            if (!slot.processor || !slot.processor->valid())
            {
                std::printf("WARN: plugin prepare failed: %s (slot not added)\n", slot.host->lastError().c_str());
                return -1;
            }
            slot.name = effect->name;
            slot.bypassStatus = false;
            const int index = static_cast<int>(pluginSlots.size());
            pluginSlots.push_back(std::move(slot));
            std::printf("[vst3] effect \"%s\" added as slot %d (latency %u samples, layout %s)\n", effect->name.c_str(), index,
                        static_cast<unsigned>(pluginSlots[static_cast<std::size_t>(index)].processor->latencySamples()),
                        pluginSlots[static_cast<std::size_t>(index)].processor->layout() == audient::vst3::BusLayout::Stereo
                            ? "stereo"
                            : "mono");
            rebuildMicChain();
            return index;
        }
        catch (const std::exception& exception)
        {
            std::printf("WARN: plugin raised an exception during add: %s (slot not added, VST-018 compatibility record)\n",
                        exception.what());
            return -1;
        }
        catch (...)
        {
            std::printf("WARN: plugin raised an unknown exception during add (slot not added, VST-018 record)\n");
            return -1;
        }
    };

    // Optional output-chain slot (stereo), held alive outside the interactive
    // mic slot list.
    std::unique_ptr<PluginSlot> outSlot;
    const auto addOutPluginFromPath = [&](const std::string& path) -> bool {
        try
        {
            auto slot = std::make_unique<PluginSlot>();
            slot->loader = std::make_unique<audient::vst3::Vst3ModuleLoader>();
            if (!slot->loader->load(path))
            {
                std::printf("[out] WARN: plugin load failed: %s\n", slot->loader->lastError().c_str());
                return false;
            }
            slot->host = std::make_unique<audient::vst3::Vst3Host>();
            if (!slot->host->attachFactory(slot->loader->factory()))
            {
                std::printf("[out] WARN: factory attach failed\n");
                return false;
            }
            const audient::vst3::PluginClassInfo* effect = nullptr;
            for (const audient::vst3::PluginClassInfo& cls : slot->host->classes())
            {
                if (cls.isEffect())
                {
                    effect = &cls;
                    break;
                }
            }
            if (effect == nullptr)
            {
                std::printf("[out] WARN: no Audio Effect class found in module\n");
                return false;
            }
            slot->processor = slot->host->createEffectProcessor(*effect, 48000.0, kMaxBlockLong,
                                                                audient::vst3::BusLayout::Stereo);
            if (!slot->processor || !slot->processor->valid())
            {
                std::printf("[out] WARN: plugin prepare failed: %s\n", slot->host->lastError().c_str());
                return false;
            }
            outChain.publish({{slot->processor.get(), false}}, false);
            std::printf("[out] effect \"%s\" active (latency %u samples)\n", effect->name.c_str(),
                        static_cast<unsigned>(slot->processor->latencySamples()));
            outSlot = std::move(slot);
            return true;
        }
        catch (const std::exception& exception)
        {
            std::printf("[out] WARN: plugin raised an exception: %s (not added, VST-018 compatibility record)\n",
                        exception.what());
            return false;
        }
        catch (...)
        {
            std::printf("[out] WARN: plugin raised an unknown exception (not added, VST-018 record)\n");
            return false;
        }
    };

    // Startup plugin selection: explicit --vst3 wins; otherwise an interactive
    // chooser over --vst3dir (skipped for non-interactive --seconds runs).
    if (vst3MicPath.empty() && autoExitSeconds == 0 && !vst3Candidates.empty())
    {
        std::printf("[vst3] choose initial plug-in (0..%zu, Ctrl+C to skip):\n", vst3Candidates.size() - 1);
        for (std::size_t i = 0; i < vst3Candidates.size(); ++i)
        {
            std::printf("  [%3zu] %s\n", i, vst3Candidates[i].c_str());
        }
        std::printf("> ");
        std::fflush(stdout);
        std::string line;
        std::getline(std::cin, line);
        const std::size_t choice = static_cast<std::size_t>(std::strtoul(line.c_str(), nullptr, 10));
        if (choice < vst3Candidates.size())
        {
            vst3MicPath = vst3Candidates[choice];
            std::printf("[vst3] picked %s\n", vst3MicPath.c_str());
        }
        else
        {
            std::printf("[vst3] invalid choice, continuing dry\n");
        }
    }

    if (!vst3MicPath.empty() && addPluginFromPath(vst3MicPath) < 0)
    {
        std::printf("[vst3] initial plug-in not loaded; continuing with an empty mic chain\n");
    }
    if (!vst3OutPath.empty() && !addOutPluginFromPath(vst3OutPath))
    {
        std::printf("[out] initial plug-in not loaded; continuing with an empty output chain\n");
    }

    audient::asio::AsioRoutingAdapter adapter(backend, kMaxBlock);
    adapter.setMicChain(&audient::vst3::Vst3Chain::processMono, &micChain);
    adapter.setInputChainLatency(&audient::vst3::Vst3Chain::chainLatencyQuery, &micChain);
    adapter.setOutputChain(&audient::vst3::Vst3Chain::processStereo, &outChain);
    adapter.setOutputChainLatency(&audient::vst3::Vst3Chain::chainLatencyQuery, &outChain);
    adapter.setSourceLatency(audient::routing::BusId::PhysicalInput1,
                             static_cast<std::uint32_t>(caps.latencies.input > 0 ? caps.latencies.input : 0));
    adapter.setSourceLatency(audient::routing::BusId::PlaybackBus,
                             static_cast<std::uint32_t>(caps.latencies.output > 0 ? caps.latencies.output : 0));

    audient::asio::AsioRoutingAdapter::RenderConfig cfg;
    cfg.monitorGain = dBToLinear(-6.0f); // processed mic monitor at a moderate level
    cfg.monitorMute = false;
    cfg.downlinkGain = 1.0f;
    cfg.downlinkMute = true; // no Windows system mix yet in this demo
    cfg.physicalOutputGain = dBToLinear(0.0f);
    cfg.physicalOutputMute = false;
    cfg.panicMute = false;
    cfg.revision = 1;
    adapter.publishRenderConfig(cfg);

    adapter.attach();
    adapter.requestFadeIn();
    if (!backend.start(error))
    {
        std::printf("ERROR: start failed: %s\n", error.c_str());
        return 1;
    }

    const audient::routing::LatencySnapshot latency = adapter.latencySnapshot();
    std::printf("STREAM STARTED at 48 kHz / %ld samples.\n", buffer);
    std::printf("  latency model (samples): micRaw=%u micProcessed=%u micUplink=%u outputProcessed=%u physicalOut=%u\n",
                latency.micRaw, latency.micProcessed, latency.micUplink, latency.outputProcessed, latency.physicalOutput);
    std::printf("Keys:\n");
    std::printf("  + / -   physical output level         m   monitor mute toggle\n");
    std::printf("  p       panic mute (zeros output)     e   open/close VST3 editor\n");
    std::printf("  a       add plug-in (menu)            r   remove last slot\n");
    std::printf("  b       bypass last slot              B   bypass whole mic chain\n");
    std::printf("  q       quit (graceful fade-out)\n\n");

    // Native editor hosting (VST-010 demo path), same lifecycle as the main
    // demo but wired to the routing adapter's mic chain.
    audient::vst3::Vst3Editor pluginEditor;
    EditorHostState editorHostState{};
    HWND editorHostWindow = nullptr;
    audient::vst3::Vst3Host* editorSinkHost = nullptr;

    const auto clearParameterEditSink = [&]() {
        if (editorSinkHost != nullptr)
        {
            editorSinkHost->setParameterEditSink(nullptr);
        }
        editorSinkHost = nullptr;
    };
    const auto closeEditor = [&]() {
        if (pluginEditor.isOpen())
        {
            pluginEditor.close();
        }
        if (editorHostWindow != nullptr)
        {
            DestroyWindow(editorHostWindow);
            editorHostWindow = nullptr;
        }
        clearParameterEditSink();
    };

    std::atomic<bool> running{true};
    std::atomic<bool> promptActive{false}; // status thread pauses while an interactive menu is up
    std::thread uiThread([&]() {
        StatusLine statusLine;
        while (running.load(std::memory_order_relaxed))
        {
            if (promptActive.load(std::memory_order_relaxed))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            const float micPeak = adapter.micUplinkPeak();
            const float micDb = micPeak > 0.0001f ? 20.0f * std::log10(micPeak) : -120.0f;
            const std::size_t slots = slotCount.load(std::memory_order_relaxed);
            const bool bypass = chainBypassStatus.load(std::memory_order_relaxed);
            char buffer[256]{};
            std::snprintf(buffer, sizeof(buffer),
                          "state=%-11s callbacks=%-9llu xruns=%-5llu overloads=%-5llu mic=%6.1fdBFS fade=%s monitor=%s slots=%zu/bypass=%d",
                          backend.state() == audient::asio::DeviceState::Streaming ? "Streaming"
                          : backend.state() == audient::asio::DeviceState::Ready    ? "Ready"
                          : backend.state() == audient::asio::DeviceState::Reconnecting ? "Reconnecting"
                          : backend.state() == audient::asio::DeviceState::NoDevice ? "NoDevice"
                                                                                     : "Other",
                          static_cast<unsigned long long>(backend.callbackCount()),
                          static_cast<unsigned long long>(backend.xrunCount()),
                          static_cast<unsigned long long>(backend.overloadCount()),
                          static_cast<double>(micDb),
                          adapter.fadeIsFull() ? "full" : adapter.fadeIsMuted() ? "muted" : "mid",
                          cfg.monitorMute ? "OFF" : "ON",
                          slots, bypass ? 1 : 0);
            statusLine.write(buffer);
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });

    const auto autoExitDeadline = [&]() {
        return autoExitSeconds > 0 ? std::chrono::steady_clock::now() + std::chrono::seconds(autoExitSeconds)
                                   : std::chrono::steady_clock::time_point::max();
    }();

    bool quit = false;
    while (!quit)
    {
        if (autoExitSeconds > 0 && std::chrono::steady_clock::now() >= autoExitDeadline)
        {
            quit = true;
        }
        // Pump messages for the open editor window (the plug-in GUI needs them).
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (editorHostState.wantClose)
        {
            editorHostState.wantClose = false;
            closeEditor();
            std::printf("\neditor closed\n");
        }
        // Reap retired chain snapshots and destroy removed processors only once
        // the chain has settled (no callback can still be touching the old
        // snapshot that referenced them).
        micChain.reap();
        if (micChain.settled() && !retiredSlots.empty())
        {
            retiredSlots.clear();
            std::printf("\nslot removed and reclaimed (safe to unload)\n");
        }
        if (_kbhit())
        {
            const int key = std::tolower(_getch());
            switch (key)
            {
            case '+':
            case '=':
            {
                audient::asio::AsioRoutingAdapter::RenderConfig next = adapter.renderConfig();
                const float currentDb = 20.0f * std::log10(std::max(next.physicalOutputGain, 1e-6f));
                next.physicalOutputGain = dBToLinear(std::min(12.0f, currentDb + 1.0f));
                next.revision = next.revision + 1u;
                adapter.publishRenderConfig(next);
                std::printf("\noutput +1 dB -> %.1f dB\n", std::min(12.0f, currentDb + 1.0f));
                break;
            }
            case '-':
            case '_':
            {
                audient::asio::AsioRoutingAdapter::RenderConfig next = adapter.renderConfig();
                const float currentDb = 20.0f * std::log10(std::max(next.physicalOutputGain, 1e-6f));
                next.physicalOutputGain = dBToLinear(std::max(-60.0f, currentDb - 1.0f));
                next.revision = next.revision + 1u;
                adapter.publishRenderConfig(next);
                std::printf("\noutput -1 dB -> %.1f dB\n", std::max(-60.0f, currentDb - 1.0f));
                break;
            }
            case 'm':
            {
                audient::asio::AsioRoutingAdapter::RenderConfig next = adapter.renderConfig();
                next.monitorMute = !next.monitorMute;
                next.revision = next.revision + 1u;
                adapter.publishRenderConfig(next);
                cfg.monitorMute = next.monitorMute;
                std::printf("\nmonitor %s\n", next.monitorMute ? "MUTED" : "UNMUTED");
                break;
            }
            case 'p':
                backend.panic();
                backend.clearPanic();
                std::printf("\npanic: momentary mute applied (output zeroed)\n");
                break;
            case 'a':
            {
                if (pluginSlots.size() >= audient::vst3::Vst3Chain::kMaxSlots)
                {
                    std::printf("\nmic chain already full (%zu/%zu slots)\n", pluginSlots.size(),
                                static_cast<std::size_t>(audient::vst3::Vst3Chain::kMaxSlots));
                    break;
                }
                if (vst3Candidates.empty())
                {
                    std::printf("\nno candidate .vst3 under %s\n", vst3Dir.c_str());
                    break;
                }
                promptActive.store(true, std::memory_order_relaxed);
                std::printf("\nadd plug-in (0..%zu, empty = cancel):\n", vst3Candidates.size() - 1);
                for (std::size_t i = 0; i < vst3Candidates.size(); ++i)
                {
                    std::printf("  [%3zu] %s\n", i, vst3Candidates[i].c_str());
                }
                std::printf("> ");
                std::fflush(stdout);
                const std::string line = readLineFromConsole();
                promptActive.store(false, std::memory_order_relaxed);
                if (line.empty())
                {
                    std::printf("add cancelled\n");
                    break;
                }
                const std::size_t choice = static_cast<std::size_t>(std::strtoul(line.c_str(), nullptr, 10));
                if (choice >= vst3Candidates.size())
                {
                    std::printf("invalid choice, add cancelled\n");
                    break;
                }
                if (addPluginFromPath(vst3Candidates[choice]) < 0)
                {
                    std::printf("add failed; chain unchanged\n");
                }
                break;
            }
            case 'r':
                if (pluginSlots.empty())
                {
                    std::printf("\nno slot to remove\n");
                    break;
                }
                {
                    closeEditor();
                    PluginSlot removed = std::move(pluginSlots.back());
                    pluginSlots.pop_back();
                    retiredSlots.push_back(std::move(removed));
                    std::printf("\nremoved slot %zu; chain republished\n", pluginSlots.size());
                    rebuildMicChain();
                }
                break;
            case 'b':
                if (pluginSlots.empty())
                {
                    std::printf("\nno slot to bypass\n");
                    break;
                }
                {
                    pluginSlots.back().bypassStatus = !pluginSlots.back().bypassStatus;
                    std::printf("\nslot %zu bypass %s\n", pluginSlots.size() - 1,
                                pluginSlots.back().bypassStatus ? "ON" : "OFF");
                    rebuildMicChain();
                }
                break;
            case 'B':
                micChainWholeBypass = !micChainWholeBypass;
                std::printf("\nwhole mic chain bypass %s\n", micChainWholeBypass ? "ON" : "OFF");
                rebuildMicChain();
                break;
            case 'e':
                if (pluginEditor.isOpen())
                {
                    closeEditor();
                    std::printf("\neditor closed\n");
                    break;
                }
                if (pluginSlots.empty())
                {
                    std::printf("\neditor unavailable (no prepared plugin)\n");
                    break;
                }
                {
                    std::size_t targetIndex = pluginSlots.size() - 1;
                    if (pluginSlots.size() > 1)
                    {
                        promptActive.store(true, std::memory_order_relaxed);
                        std::printf("\nchoose editor for plug-in (0..%zu, empty = last):\n", pluginSlots.size() - 1);
                        for (std::size_t i = 0; i < pluginSlots.size(); ++i)
                        {
                            std::printf("  [%3zu] %s%s%s\n", i, pluginSlots[i].name.c_str(),
                                        pluginSlots[i].bypassStatus ? "  (bypassed)" : "",
                                        i == pluginSlots.size() - 1 ? "  <- last" : "");
                        }
                        std::printf("> ");
                        std::fflush(stdout);
                        const std::string line = readLineFromConsole();
                        promptActive.store(false, std::memory_order_relaxed);
                        if (!line.empty())
                        {
                            const std::size_t pick = static_cast<std::size_t>(std::strtoul(line.c_str(), nullptr, 10));
                            if (pick < pluginSlots.size())
                            {
                                targetIndex = pick;
                            }
                            else
                            {
                                std::printf("invalid slot index; using last\n");
                            }
                        }
                    }
                    PluginSlot& target = pluginSlots[targetIndex];
                    if (target.processor == nullptr || !target.processor->valid() || target.host == nullptr ||
                        target.loader == nullptr)
                    {
                        std::printf("\neditor unavailable for slot %zu (not prepared)\n", targetIndex);
                        break;
                    }
                    if (!ensureEditorHostClass())
                    {
                        std::printf("\neditor host window class registration failed\n");
                        break;
                    }
                    editorHostState = EditorHostState{};
                    editorHostState.editor = &pluginEditor;
                    editorHostWindow = CreateWindowExW(0, kEditorHostClass, L"VST3 Editor", WS_OVERLAPPEDWINDOW,
                                                       CW_USEDEFAULT, CW_USEDEFAULT, 860, 660, nullptr, nullptr,
                                                       GetModuleHandleW(nullptr), &editorHostState);
                    if (editorHostWindow == nullptr)
                    {
                        std::printf("\neditor host window creation failed (%lu)\n", GetLastError());
                        break;
                    }
                    if (!pluginEditor.open(target.processor->component(), target.loader->factory(), editorHostWindow,
                                           target.host->hostContext()))
                    {
                        std::printf("\neditor open failed: %s\n", pluginEditor.lastError().c_str());
                        DestroyWindow(editorHostWindow);
                        editorHostWindow = nullptr;
                        break;
                    }
                    // VST-009: route controller edits (performEdit) from the
                    // slot's host context into this processor's bounded queue.
                    target.host->setParameterEditSink(target.processor.get());
                    editorSinkHost = target.host.get();
                    ShowWindow(editorHostWindow, SW_SHOW);
                    UpdateWindow(editorHostWindow);
                    std::printf("\neditor open for slot %zu \"%s\" (press e to close)\n", targetIndex,
                                target.name.c_str());
                }
                break;
            case 'q':
                quit = true;
                break;
            default:
                std::printf("\nkey '%c' ignored (use + - m p e a r b B q)\n", key);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    closeEditor();

    running.store(false, std::memory_order_relaxed);
    if (uiThread.joinable())
    {
        uiThread.join();
    }

    std::printf("\ngraceful fade-out and stop...\n");
    adapter.requestFadeOut();
    const auto mutedDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
    while (std::chrono::steady_clock::now() < mutedDeadline && !adapter.fadeIsMuted())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!backend.stop(error))
    {
        std::printf("WARN: stop failed: %s\n", error.c_str());
    }
    adapter.detach();

    std::printf("\nfinished. callbacks=%llu xruns=%llu overloads=%llu\n",
                static_cast<unsigned long long>(backend.callbackCount()),
                static_cast<unsigned long long>(backend.xrunCount()),
                static_cast<unsigned long long>(backend.overloadCount()));
    return 0;
}