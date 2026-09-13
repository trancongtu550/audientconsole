// physical_vmic_demo_main.cpp - live physical-input integration demo.
//
// Manual test harness that runs the REAL production path end to end on the app
// side and exposes the virtual-mic driver link:
//
//   Audient ASIO input (channel plan, selectable)
//       -> AsioRoutingAdapter / RoutingCore (production adapter)
//       -> input VST3 mic chain if enabled (--vst3)
//       -> processed mic uplink -> VirtualCaptureTransport (realtime publish)
//       -> SoftwareCaptureEndpoint -> VirtualMicFeeder (NORMAL worker)
//       -> SharedRingCaptureSink -> capture ring -> CaptureControlClient
//       -> \\.\AudientConsoleControl (Q5-A3B driver control plane)
//       -> virtual mic driver DMA fill -> Windows capture endpoint
//
// Hard rules (AGENTS §7):
//   - The ASIO callback path does ONLY the production adapter's realtime work
//     (buffer binds, prepared chain hooks, ramps, one bounded ring write + the
//     optional diagnostic taps). No DeviceIoControl, no mapping, no allocation,
//     no locks, no driver calls in the callback.
//   - The feeder (VirtualMicFeeder) remains the boundary to SharedRingCaptureSink.
//     When the virtual-mic driver is not installed on this machine, the demo
//     falls back to a LOCAL ring (app-only mode); --require-driver is a hard
//     error instead.
//
// REALTIME PACING (root cause of the "voice -> buzz" failure, fixed here):
//   VirtualMicFeeder drains the realtime 48 kHz stream (750 blocks/s). A 1 ms
//   poll alone cannot keep up because Windows coalesces short sleeps to the
//   scheduler tick (~15.6 ms unless the timer resolution is raised), waking the
//   worker only ~60-70x/s; the bounded drop-new transport then discards most of
//   the audio and the virtual mic hears bursts + gaps = "constant buzzing/noise,
//   no intelligible voice". The synthetic tone demo did not use this
//   transport/feeder chain (it wrote straight into the ring), which is why it
//   stayed clean. Fix: timeBeginPeriod(1) at startup + sub-millisecond feeder
//   poll, verified by a deterministic SPSC-chain probe (751.0 vs 750.5 blk/s,
//   0.1% deficit, zero transport overflow).
//
// Usage:
//   physical_vmic_demo [--channel <n>] [--vst3 <path>]... [--buffer <frames>]
//                      [--seconds <N>] [--monitor-off|--monitor-on] [--require-driver]
//                      [--record <prefix>] [--record-seconds <N>]
//                      [--probe-latency] [--simulate-loss-after <ms>]
//                      [--editor] [--vst3dir <dir>]
//
//   --channel <n>        physical ASIO input channel index for the mic uplink
//   --vst3 <path>        one mic-chain VST3 effect; REPEATABLE (order = chain
//                        order, max 8). When omitted on an interactive console, a
//                        native file picker lets you choose the .vst3 yourself.
//   --buffer <frames>    ASIO buffer size within the driver's [min,max]
//                        (default 64, fallback 128, then driver preferred)
//   --seconds <n>        run N seconds then exit cleanly (default: Ctrl+C)
//   --monitor-off        mute the processed-mic monitor on physical Output 1/2
//                        (DEFAULT: local monitoring is OFF for this virtual-mic
//                        workflow; it never affects the virtual-mic path)
//   --monitor-on         enable the local processed-mic monitor on Output 1/2
//                        (also toggled live with the 'm' key)
//   --require-driver     exit if the driver control plane cannot connect
//   --record <prefix>    diagnostic mode: write 48 kHz mono PCM16 WAVs at three
//                        points (A decoded ASIO input; B RoutingCore mic output
//                        before the feeder; C data before SharedRingCaptureSink)
//   --record-seconds <n> recording length (default 8 s; capped at 30 s)
//   --probe-latency      deterministic latency probe: injects impulse markers at
//                        the physical ASIO input and at the capture-ring feeder,
//                        captures the virtual-mic endpoint concurrently, and
//                        prints producer -> WASAPI-visible latency
//                        (also prints transport/capture-ring backlogs each second)
//   --editor             open the FIRST slot's native VST3 editor at startup
//   --vst3dir <dir>      starting folder for the native plug-in file picker
//                        (default C:\Program Files\Common Files\VST3)
//   --identify-output    safe channel identification: play a LOW-level (-30 dBFS)
//                        1.5 kHz tone (100 ms on/off) on ONE output pair so you
//                        can name which physical speakers/headphones it drives.
//                        0 = Output 1/2 (Monitor), 1 = Output 3/4 (Headphones).
//
// Interactive console (no --seconds / stdout is a real console):
//   - A fixed status pane is drawn at the bottom of the console (in-place
//     updates, no scrolling [status] spam). The pane shows the whole VST chain
//     in processing order ([1] first) with per-slot ACTIVE/BYPASSED + reported
//     latency and the chain total. Event lines ([vst3], [device], ...) still log
//     above it. Chain mutations republish an immutable snapshot and never touch
//     the audio callback.
//   - Keys:  1-9 / Up-Down  select a slot (target for editor/bypass/remove/
//                           reorder). Selecting NEVER stops or quits the stream;
//                           a missing slot number just logs and continues.
//            a    add a plug-in (native file picker, appended last)
//            e    open/close the SELECTED slot's native editor
//            s    toggle bypass of the SELECTED slot only
//            x    remove the SELECTED slot (retired after the chain's grace)
//            ,/< or Left/Right  move the SELECTED slot one place earlier/later
//            m    toggle the LOCAL MONITOR OFF/ON: processed VST mic -> ASIO
//                 Output 1/2 (never affects Mic -> VST chain -> virtual mic).
//                 The iD14 natively sends Output 1/2 to BOTH its physical
//                 speakers and headphones.
//            + / -  adjust the LOCAL MONITOR software gain (feed level).
//            b    toggle host-side WHOLE-chain bypass (ACTIVE/BYPASSED)
//            h    open the HARDWARE VOLUME CONTROL mode (iD14 PHYSICAL device
//                 volume via the official Audient API): 1 iD14 Monitor,
//                 2 iD14 Headphones, +/- or Up/Down +/-1 dB, 'h' returns.
//                 Independent of the LOCAL MONITOR software gain; no ASIO 3/4 or
//                 Cue routing is used in v1.
//            q    quit cleanly (the ONLY normal quit key; Ctrl+C also quits)
//   - Local Monitor is OFF by default (see --monitor-on).
//   - If --vst3 was not given, a native file picker lets you select the plug-in.
//
// Automation / redirected stdout (--seconds, ssh, tests): one [status] line per
// second is appended as before, so captured logs keep their exact format.
//
// Standalone: built by CMake (AUDIENT_ASIO_SDK_DIR) reusing the production
// libraries; not part of src/app/demo_main.cpp.

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "asio/AsioBackend.h"
#include "asio/AsioChannelMap.h"
#include "asio/AsioRecoveryController.h"
#include "asio/AsioRoutingAdapter.h"
#include "asio/sdk/NativeAsioDriverProvider.h"
#include "channel/VirtualMicSource.h"
#include "core/Version.h"
#include "engine/GraphConfig.h"
#include "hardware/AudientHardwareControl.h"
#include "preferences/AppPreferences.h"
#include "transport/VirtualCaptureTransport.h"
#include "pico/PicoUsbWorker.h"
#include "pico/PicoVirtualMicSink.h"
#include "pico/WasapiPicoRenderDevice.h"
#include "virtual_audio/driver-protocol/CaptureControlClient.h"
#include "virtual_audio/driver-protocol/FanoutCaptureSink.h"
#include "virtual_audio/driver-protocol/IDriverCaptureSink.h"
#include "virtual_audio/driver-protocol/SharedRingCaptureSink.h"
#include "virtual_audio/driver-protocol/VirtualMicFeeder.h"
#include "virtual_audio/endpoint/SoftwareCaptureEndpoint.h"
#include "vst3/Vst3Chain.h"
#include "vst3/Vst3Controller.h"
#include "vst3/Vst3Editor.h"
#include "vst3/Vst3Host.h"
#include "vst3/Vst3ModuleLoader.h"
#include "vst3/Vst3Processor.h"
#include "engine/SyntheticDownlink.h"
#include "daily_gui/DailyGui.h"
#include "daily_gui/DailyTray.h"
#include "daily_gui/DailyWebViewController.h"
#include "DailyMixerState.h"

#include <commctrl.h>
#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <commdlg.h>
#include <conio.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace
{

constexpr std::size_t kMaxBlock = 1024;
constexpr std::size_t kCaptureBlockFrames = 64;

// A4 automatic device recovery: watchdog timing (see AsioRecoveryController.h).
// Nominal ASIO callback period = 64/48000 = 1.333 ms.
constexpr std::chrono::milliseconds kWatchdogStallMs(150);   // ~112 nominal periods; >10x the 10 ms engine period
constexpr std::chrono::milliseconds kRecoveryRetryMs(750);   // reconnect retry cadence (no busy-spin)
constexpr std::chrono::milliseconds kWatchdogTickMs(20);     // control-thread poll cadence

// VST3 diagnostic: apply --vst3-param edits shortly after stream start (after
// the latency probe and a short flat buffer so the recorder taps A/B also
// capture the pre-edit path), then a grace before --vst3-save snapshots state.
constexpr std::chrono::milliseconds kParamApplyDelayMs(1500);
constexpr std::chrono::milliseconds kParamSaveGraceMs(500);

std::atomic<bool> g_stop{false};
std::atomic<bool> g_teardownStarted{false};

BOOL WINAPI consoleBreakHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT ||
        ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT)
    {
        g_stop.store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

bool containsInsensitive(const std::string& haystack, const char* needle)
{
    std::string lowered = haystack;
    for (char& ch : lowered)
    {
        ch = static_cast<char>(static_cast<unsigned char>(std::tolower(ch)));
    }
    return lowered.find(needle) != std::string::npos;
}

// VST3 diagnostic: convert a VstString (TChar = UTF-16 on Windows) to UTF-8 for
// the --vst3-enum parameter listing. Control/UI thread only.
std::string narrowUtf16(const Steinberg::Vst::TChar* text)
{
    if (text == nullptr)
    {
        return {};
    }
    std::size_t length = 0;
    while (text[length] != 0)
    {
        ++length;
    }
    if (length == 0)
    {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(text),
                                           static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
    {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(text), static_cast<int>(length),
                              &out[0], needed, nullptr, nullptr);
    return out;
}

float dBToLinear(float db)
{
    return std::pow(10.0f, db / 20.0f);
}

const char* stateName(audient::asio::DeviceState state)
{
    switch (state)
    {
    case audient::asio::DeviceState::Streaming: return "Streaming";
    case audient::asio::DeviceState::Ready: return "Ready";
    case audient::asio::DeviceState::Reconnecting: return "Reconnecting";
    case audient::asio::DeviceState::NoDevice: return "NoDevice";
    case audient::asio::DeviceState::Error: return "Error";
    default: return "Other";
    }
}

const char* sampleFormatName(audient::asio::SampleFormat format)
{
    switch (format)
    {
    case audient::asio::SampleFormat::Int16LE: return "Int16LSB";
    case audient::asio::SampleFormat::Int24LE: return "Int24LSB";
    case audient::asio::SampleFormat::Int32LE: return "Int32LSB";
    case audient::asio::SampleFormat::Float32LE: return "Float32LSB";
    case audient::asio::SampleFormat::Float64LE: return "Float64LSB";
    default: return "unknown";
    }
}

bool writePcm16MonoWav(const std::string& path, const std::vector<std::int16_t>& samples, unsigned rate)
{
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(samples.size() * sizeof(std::int16_t));
    const std::uint32_t riffSize = 36u + dataBytes;
    std::vector<unsigned char> body;
    body.reserve(44 + samples.size() * 2);
    const auto put = [&body](const void* src, std::size_t n) {
        const unsigned char* p = static_cast<const unsigned char*>(src);
        body.insert(body.end(), p, p + n);
    };
    const auto putTag = [&body](const char tag[4]) { body.insert(body.end(), tag, tag + 4); };
    const auto put32 = [&](std::uint32_t v) { put(&v, 4); };
    const auto put16 = [&](std::uint16_t v) { put(&v, 2); };

    putTag("RIFF");
    put32(riffSize);
    putTag("WAVE");
    putTag("fmt ");
    put32(16);                 // PCM chunk size
    put16(1);                  // PCM
    put16(1);                  // mono
    put32(rate);               // sample rate
    put32(rate * 2u);          // byte rate
    put16(2);                  // block align
    put16(16);                 // bits
    putTag("data");
    put32(dataBytes);
    body.insert(body.end(), reinterpret_cast<const unsigned char*>(samples.data()),
                reinterpret_cast<const unsigned char*>(samples.data()) + samples.size() * 2);

    FILE* file = nullptr;
    const errno_t err = fopen_s(&file, path.c_str(), "wb");
    if (err != 0 || file == nullptr)
    {
        return false;
    }
    const bool ok = std::fwrite(body.data(), 1, body.size(), file) == body.size();
    std::fclose(file);
    return ok;
}

// ---------------------------------------------------------------------------
// Bounded SPSC float32 sample buffer (realtime-clean push; drop-new).
// ---------------------------------------------------------------------------
class SpscFloatSample
{
public:
    explicit SpscFloatSample(std::size_t capacity) : m_mask(capacity - 1), m_data(capacity, 0.0f) {}

    bool push(const float* src, std::size_t frames)
    {
        const std::size_t write = m_write.load(std::memory_order_acquire);
        const std::size_t read = m_read.load(std::memory_order_acquire);
        if (write - read + frames > m_data.size())
        {
            m_dropped += frames;
            return false;
        }
        for (std::size_t i = 0; i < frames; ++i)
        {
            m_data[(write + i) & m_mask] = src[i];
        }
        _ReadWriteBarrier();
        m_write.store(write + frames, std::memory_order_release);
        m_accepted += frames;
        return true;
    }

    std::size_t pop(float* dst, std::size_t maxFrames)
    {
        const std::size_t read = m_read.load(std::memory_order_acquire);
        const std::size_t write = m_write.load(std::memory_order_acquire);
        const std::size_t available = write - read;
        if (available == 0)
        {
            return 0;
        }
        const std::size_t take = available < maxFrames ? available : maxFrames;
        for (std::size_t i = 0; i < take; ++i)
        {
            dst[i] = m_data[(read + i) & m_mask];
        }
        _ReadWriteBarrier();
        m_read.store(read + take, std::memory_order_release);
        m_recorded += take;
        return take;
    }

    std::uint64_t accepted() const { return m_accepted; }
    std::uint64_t dropped() const { return m_dropped; }
    std::uint64_t recorded() const { return m_recorded; }

private:
    std::size_t m_mask = 0;
    std::vector<float> m_data;
    std::atomic<std::size_t> m_write{0};
    std::atomic<std::size_t> m_read{0};
    std::atomic<std::uint64_t> m_accepted{0};
    std::atomic<std::uint64_t> m_dropped{0};
    std::atomic<std::uint64_t> m_recorded{0};
};

// ---------------------------------------------------------------------------
// MIC RAW meter + optional recorder-A tap (folded into ONE adapter mic-input
// tap). Realtime-clean: per-block peak into an atomic (overwrite per block, so
// reads reflect the LAST processed block) + a drop-new push to the recorder.
// ---------------------------------------------------------------------------
struct MicInputTapCtx
{
    SpscFloatSample* recA = nullptr;
    std::atomic<float> blockPeak{0.0f};
};

void micInputTapHook(const float* mono, std::size_t frames, void* opaque)
{
    MicInputTapCtx* ctx = static_cast<MicInputTapCtx*>(opaque);
    if (ctx == nullptr || mono == nullptr || frames == 0)
    {
        return;
    }
    float peak = 0.0f;
    for (std::size_t i = 0; i < frames; ++i)
    {
        const float v = mono[i] >= 0.0f ? mono[i] : -mono[i];
        if (v > peak)
        {
            peak = v;
        }
    }
    ctx->blockPeak.store(peak, std::memory_order_relaxed);
    if (ctx->recA != nullptr)
    {
        (void)ctx->recA->push(mono, frames);
    }
}


// ---------------------------------------------------------------------------
// Interactive console UI (manual harness). When stdout is a REAL console the
// bottom of the window is a fixed status pane; event lines still log above it.
// Redirection/automation (stdout is a pipe) leaves normal line output alone.
// ---------------------------------------------------------------------------
class ConsoleUi
{
public:
    // Pane row count is dynamic: the demo grows it when the VST chain has more
    // plug-ins (default matches the original 9-row pane with zero/one slot).
    static std::size_t defaultPaneRows() { return 9; }

    static bool available()
    {
        const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        return handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) != 0;
    }

    void setPaneRows(std::size_t rows)
    {
        m_paneRows = rows < 4 ? 4 : (rows > 24 ? 24 : rows);
    }
    std::size_t paneRows() const { return m_paneRows; }

    // Enters the TUI: virtual-terminal mode, cursor hidden, scrolling restricted
    // to the log rows ABOVE the reserved pane. Returns false when no console.
    bool begin()
    {
        m_out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (m_out == INVALID_HANDLE_VALUE || !GetConsoleScreenBufferInfo(m_out, &m_info))
        {
            return false;
        }
        DWORD inMode = 0;
        m_in = GetStdHandle(STD_INPUT_HANDLE);
        if (m_in != INVALID_HANDLE_VALUE && GetConsoleMode(m_in, &inMode) != 0)
        {
            m_savedInMode = inMode;
            m_inModeValid = true;
            SetConsoleMode(m_in, inMode & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT));
        }
        DWORD outMode = 0;
        if (GetConsoleMode(m_out, &outMode) != 0)
        {
            m_savedOutMode = outMode;
            m_outModeValid = true;
            SetConsoleMode(m_out, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
        }
        writeVt("\x1b[?25l"); // hide cursor
        writeVt("\x1b[2J");   // clear
        applyRegion();
        parkAtLogBottom();
        m_active = true;
        return true;
    }

    void end()
    {
        if (!m_active)
        {
            return;
        }
        writeVt("\x1b[?25h");
        writeVt("\x1b[0m");
        writeVt("\x1b[r"); // reset the scrolling region
        writeVt("\x1b[2J\x1b[H");
        if (m_outModeValid)
        {
            SetConsoleMode(m_out, m_savedOutMode);
        }
        if (m_inModeValid)
        {
            SetConsoleMode(m_in, m_savedInMode);
        }
        m_active = false;
    }

    bool active() const { return m_active; }

    // Redraw the fixed pane (in place). The scrolling region is restored so a
    // following event line never corrupts the pane.
    void render(const std::vector<std::string>& lines)
    {
        if (!m_active || !GetConsoleScreenBufferInfo(m_out, &m_info))
        {
            return;
        }
        const SHORT width = static_cast<SHORT>(m_info.srWindow.Right - m_info.srWindow.Left + 1);
        const SHORT height = static_cast<SHORT>(m_info.srWindow.Bottom - m_info.srWindow.Top + 1);
        if (width <= 1 || height <= static_cast<SHORT>(m_paneRows + 1))
        {
            return;
        }
        const SHORT paneTopRow = height - static_cast<SHORT>(m_paneRows) + 1; // viewport 1-based
        std::string vt;
        // When the pane shrank since the last draw, clear the rows that stopped
        // belonging to it (they now sit in the scrollable log region above).
        if (m_renderedRows > m_paneRows)
        {
            for (SHORT row = height - static_cast<SHORT>(m_renderedRows) + 1;
                 row <= paneTopRow - 1; ++row)
            {
                vt += "\x1b[" + std::to_string(row) + ";1H\x1b[K";
            }
        }
        m_renderedRows = m_paneRows;
        applyRegion();
        for (std::size_t i = 0; i < m_paneRows; ++i)
        {
            std::string text = i < lines.size() ? lines[i] : std::string();
            if (text.size() > static_cast<std::size_t>(width - 1))
            {
                text.resize(static_cast<std::size_t>(width - 1));
            }
            vt += "\x1b[" + std::to_string(paneTopRow + static_cast<SHORT>(i)) + ";1H" + text + "\x1b[K";
        }
        writeVt(vt);
        parkAtLogBottom();
    }

private:
    void applyRegion() const
    {
        if (!GetConsoleScreenBufferInfo(m_out, const_cast<CONSOLE_SCREEN_BUFFER_INFO*>(&m_info)))
        {
            return;
        }
        const SHORT height = static_cast<SHORT>(m_info.srWindow.Bottom - m_info.srWindow.Top + 1);
        const SHORT regionBottom =
            height > static_cast<SHORT>(m_paneRows + 1) ? height - static_cast<SHORT>(m_paneRows) : 1;
        const std::string vt = "\x1b[1;" + std::to_string(regionBottom) + "r";
        writeVt(vt);
    }

    void parkAtLogBottom() const
    {
        if (!GetConsoleScreenBufferInfo(m_out, const_cast<CONSOLE_SCREEN_BUFFER_INFO*>(&m_info)))
        {
            return;
        }
        const SHORT height = static_cast<SHORT>(m_info.srWindow.Bottom - m_info.srWindow.Top + 1);
        const SHORT logBottom =
            height > static_cast<SHORT>(m_paneRows + 1) ? height - static_cast<SHORT>(m_paneRows) : 1;
        writeVt("\x1b[" + std::to_string(logBottom) + ";1H");
    }

    void writeVt(const std::string& text) const
    {
        if (m_out == INVALID_HANDLE_VALUE)
        {
            return;
        }
        DWORD written = 0;
        (void)WriteConsoleA(m_out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    }

    HANDLE m_out = INVALID_HANDLE_VALUE;
    HANDLE m_in = INVALID_HANDLE_VALUE;
    CONSOLE_SCREEN_BUFFER_INFO m_info{};
    DWORD m_savedOutMode = 0;
    DWORD m_savedInMode = 0;
    bool m_outModeValid = false;
    bool m_inModeValid = false;
    bool m_active = false;
    std::size_t m_paneRows = ConsoleUi::defaultPaneRows();
    std::size_t m_renderedRows = 0;
};

// Native VST3 file picker (classic common dialog). Returns an empty string when
// the user cancels. Runs on a control thread with its own message state.
std::string pickVst3File(const std::string& initialDir)
{
    std::wstring wideDir(initialDir.begin(), initialDir.end());
    wchar_t fileBuffer[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"VST3 plug-ins (*.vst3)\0*.vst3\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = wideDir.empty() ? nullptr : wideDir.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"Select the microphone-chain VST3 effect";
    if (!GetOpenFileNameW(&ofn))
    {
        return {};
    }
    // Convert the picked path to UTF-8.
    const int needed = WideCharToMultiByte(CP_UTF8, 0, fileBuffer, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
    {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    (void)WideCharToMultiByte(CP_UTF8, 0, fileBuffer, -1, &out[0], needed, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// Native editor host window (mirrors routing_demo_main). The plug-in view is a
// child of this window; Vst3Editor owns that child. WM_CLOSE asks the control
// loop to close the editor; closing the window NEVER stops the audio stream.
// ---------------------------------------------------------------------------
const wchar_t kEditorHostClassName[] = L"AudientConsolePhysicalVmicEditorHost";

struct EditorHostState
{
    audient::vst3::Vst3Editor* editor = nullptr;
    bool wantClose = false;
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

bool ensureEditorHostClass()
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &editorHostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kEditorHostClassName;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// ---------------------------------------------------------------------------
// Diagnostic WAV recorder: consumes three SPSC taps (A/B/C) on a normal thread
// and writes 48 kHz mono PCM16 files. Never touches the realtime path.
// ---------------------------------------------------------------------------
class WavRecorder
{
public:
    bool start(const std::string& prefix, int seconds)
    {
        const int cappedSeconds = seconds > 30 ? 30 : (seconds > 0 ? seconds : 8);
        const std::size_t frames = static_cast<std::size_t>(48000) * static_cast<std::size_t>(cappedSeconds);
        m_a = std::make_unique<SpscFloatSample>(frames);
        m_b = std::make_unique<SpscFloatSample>(frames);
        m_c = std::make_unique<SpscFloatSample>(frames);
        m_pathA = prefix + "-A-input.wav";
        m_pathB = prefix + "-B-micuplink.wav";
        m_pathC = prefix + "-C-presink.wav";
        m_running.store(true, std::memory_order_release);
        m_thread = std::thread(&WavRecorder::run, this);
        return true;
    }

    SpscFloatSample* a() const { return m_a.get(); }
    SpscFloatSample* b() const { return m_b.get(); }
    SpscFloatSample* c() const { return m_c.get(); }

    void stopAndFinalize()
    {
        m_running.store(false, std::memory_order_release);
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        std::printf("WAV final: A=%s B=%s C=%s\n",
                    m_pathA.c_str(), m_pathB.c_str(), m_pathC.c_str());
    }

private:
    static void drainInto(SpscFloatSample* source, std::vector<std::int16_t>& pcm)
    {
        std::vector<float> scratch(4096, 0.0f);
        std::size_t got = 0;
        do
        {
            got = source->pop(scratch.data(), scratch.size());
            for (std::size_t i = 0; i < got; ++i)
            {
                float v = scratch[i];
                if (!std::isfinite(v)) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                if (v < -1.0f) v = -1.0f;
                pcm.push_back(static_cast<std::int16_t>(v * 32767.0f));
            }
        } while (got != 0);
    }

    void run()
    {
        std::vector<std::int16_t> pcmA;
        std::vector<std::int16_t> pcmB;
        std::vector<std::int16_t> pcmC;
        while (m_running.load(std::memory_order_acquire))
        {
            drainInto(m_a.get(), pcmA);
            drainInto(m_b.get(), pcmB);
            drainInto(m_c.get(), pcmC);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        // Final drain so the tail buffered during stop is flushed to the files.
        drainInto(m_a.get(), pcmA);
        drainInto(m_b.get(), pcmB);
        drainInto(m_c.get(), pcmC);

        const float seconds = static_cast<float>(pcmA.size()) / 48000.0f;
        const bool okA = writePcm16MonoWav(m_pathA, pcmA, 48000);
        const bool okB = writePcm16MonoWav(m_pathB, pcmB, 48000);
        const bool okC = writePcm16MonoWav(m_pathC, pcmC, 48000);
        std::printf("WAV final (%.2f s): A=%s%s B=%s%s C=%s%s\n", seconds,
                    m_pathA.c_str(), okA ? " OK" : " FAIL",
                    m_pathB.c_str(), okB ? " OK" : " FAIL",
                    m_pathC.c_str(), okC ? " OK" : " FAIL");
    }

    std::unique_ptr<SpscFloatSample> m_a;
    std::unique_ptr<SpscFloatSample> m_b;
    std::unique_ptr<SpscFloatSample> m_c;
    std::string m_pathA;
    std::string m_pathB;
    std::string m_pathC;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

// ---------------------------------------------------------------------------
// Tap sink: forwards to the real SharedRingCaptureSink and copies each accepted
// block into the C diagnostic buffer. Runs on the VirtualMicFeeder worker.
// Also supports a ONE-SHOT calibration injection (--probe-latency phase B):
// the next writeMono replaces the block content with a caller marker and stamps
// the QPC of the write, measuring "arrival at the capture-ring feeder" latency.
// ---------------------------------------------------------------------------
class TapCaptureSink final : public audient::virtual_audio::IDriverCaptureSink
{
public:
    TapCaptureSink(audient::virtual_audio::IDriverCaptureSink* delegate, SpscFloatSample* tap)
        : m_delegate(delegate), m_tap(tap)
    {
    }

    bool connected() const override { return m_delegate->connected(); }
    std::size_t capacityFrames() const override { return m_delegate->capacityFrames(); }
    std::size_t availableFrames() const override { return m_delegate->availableFrames(); }

    bool writeMono(const float* mono, std::size_t frames, std::uint64_t generation,
                   std::uint64_t sequence) override
    {
        if (m_feederArmed.exchange(false))
        {
            // One-shot calibration: push the marker through the real sink and
            // stamp QPC at the moment the ring write happened.
            if (m_feederMarker != nullptr)
            {
                LARGE_INTEGER q;
                QueryPerformanceCounter(&q);
                m_lastFeederQpc.store(static_cast<std::uint64_t>(q.QuadPart),
                                     std::memory_order_relaxed);
                if (m_tap != nullptr)
                {
                    (void)m_tap->push(m_feederMarker->data(), frames);
                }
                return m_delegate->writeMono(m_feederMarker->data(), frames, generation, sequence);
            }
            m_lastFeederQpc.store(0, std::memory_order_relaxed);
            return false;
        }
        if (m_tap != nullptr)
        {
            (void)m_tap->push(mono, frames);
        }
        return m_delegate->writeMono(mono, frames, generation, sequence);
    }

    void armFeederProbe(const std::vector<float>& marker)
    {
        m_feederMarker = &marker;
        m_lastFeederQpc.store(0, std::memory_order_relaxed);
        std::atomic_signal_fence(std::memory_order_release);
        m_feederArmed.store(true, std::memory_order_release);
    }

    std::uint64_t lastFeederProbeQpc() const
    {
        return m_lastFeederQpc.load(std::memory_order_relaxed);
    }

private:
    audient::virtual_audio::IDriverCaptureSink* m_delegate;
    SpscFloatSample* m_tap;
    std::atomic<bool> m_feederArmed{false};
    const std::vector<float>* m_feederMarker = nullptr; // caller-owned for the arm window
    std::atomic<std::uint64_t> m_lastFeederQpc{0};
};

// ---------------------------------------------------------------------------
// Latency-probe support (--probe-latency). A WASAPI shared-mode capture client
// on the virtual-mic endpoint detects the injected impulse markers and stamps
// their engine QPC position ("when the engine says the sample was captured")
// and the host QPC when the packet became visible to the client.
// ---------------------------------------------------------------------------

struct ProbeHit
{
    LONGLONG deviceQpc = 0;     // engine QPC position of the marker sample
    LONGLONG receivedQpc = 0;   // host QPC when the packet was delivered
};

class WasapiLatencyCapture
{
public:
    bool open()
    {
        // Request APARTMENTTHREADED: the Audient ASIO driver COM class is
        // ThreadingModel=Apartment, so the thread MUST be STA before the ASIO
        // CoCreateInstance (opening an Apartment-model class from an MTA thread
        // fails with E_NOINTERFACE). This capture client may open before OR
        // after the ASIO open (client-first A4 ordering), so it must never leave
        // the thread as MTA. RPC_E_CHANGED_MODE (already initialized with a
        // different model) is therefore NOT a failure here either.
        const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_comInitOwned = SUCCEEDED(hrCom);
        if (FAILED(hrCom) && hrCom != RPC_E_CHANGED_MODE)
        {
            std::printf("probe: CoInitializeEx failed hr=0x%08X\n", (unsigned)hrCom);
            return false;
        }
        IMMDeviceEnumerator* enumerator = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&enumerator)))
        {
            std::printf("probe: MMDeviceEnumerator CoCreateInstance failed\n");
            CoUninitializeIfOwned();
            return false;
        }
        IMMDeviceCollection* devices = nullptr;
        HRESULT hr = enumerator->EnumAudioEndpoints(
            eCapture,
            DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED |
                DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED,
            &devices);
        enumerator->Release();
        if (FAILED(hr))
        {
            std::printf("probe: EnumAudioEndpoints failed hr=0x%08X\n", (unsigned)hr);
            CoUninitializeIfOwned();
            return false;
        }
        IMMDevice* matched = nullptr;
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count && matched == nullptr; ++i)
        {
            IMMDevice* dev = nullptr;
            if (FAILED(devices->Item(i, &dev)))
            {
                continue;
            }
            IPropertyStore* store = nullptr;
            BOOL nameMatch = FALSE;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)))
            {
                PROPVARIANT var;
                PropVariantInit(&var);
                if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &var)) &&
                    var.vt == VT_LPWSTR && var.pwszVal)
                {
                    nameMatch = std::wcsstr(var.pwszVal, L"Audient Console") != nullptr;
                }
                if (var.pwszVal)
                {
                    PropVariantClear(&var);
                }
                store->Release();
            }
            if (nameMatch)
            {
                matched = dev;
            }
            if (matched == nullptr)
            {
                dev->Release();
            }
        }
        devices->Release();
        if (matched == nullptr)
        {
            std::printf("probe: no capture endpoint contains 'Audient Console'\n");
            CoUninitializeIfOwned();
            return false;
        }

        hr = matched->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_client);
        if (FAILED(hr))
        {
            std::printf("probe: Activate(IAudioClient) failed hr=0x%08X\n", (unsigned)hr);
            matched->Release();
            CoUninitializeIfOwned();
            return false;
        }
        m_device = matched;

        hr = m_client->GetMixFormat(&m_mix);
        if (FAILED(hr))
        {
            std::printf("probe: GetMixFormat failed hr=0x%08X\n", (unsigned)hr);
            cleanup();
            return false;
        }

        // Engine-default shared-mode, event-driven (matches: no explicit client
        // buffer request -> whatever the engine/driver inherits).
        hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  0, 0, m_mix, nullptr);
        if (FAILED(hr))
        {
            std::printf("probe: endpoint Initialize failed hr=0x%08X\n", (unsigned)hr);
            cleanup();
            return false;
        }

        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_event == nullptr || FAILED(m_client->SetEventHandle(m_event)))
        {
            std::printf("probe: SetEventHandle/CreateEvent failed\n");
            cleanup();
            return false;
        }
        hr = m_client->GetService(__uuidof(IAudioCaptureClient), (void**)&m_capture);
        if (FAILED(hr))
        {
            std::printf("probe: GetService(IAudioCaptureClient) failed hr=0x%08X\n", (unsigned)hr);
            cleanup();
            return false;
        }
        m_client->GetBufferSize(&m_bufferFrames);
        m_client->GetStreamLatency(&m_latency100ns);
        return true;
    }

    UINT32 bufferFrames() const { return m_bufferFrames; }
    double streamLatencyMs() const { return (double)m_latency100ns / 10000.0; }

    void start()
    {
        if (m_thread.joinable())
        {
            return; // already running; single capture thread only
        }
        m_stop.store(false, std::memory_order_release);
        m_client->Start();
        m_thread = std::thread(&WasapiLatencyCapture::run, this);
    }

    // Stop the capture thread + device stream and move all collected results.
    void stopAndCollect(std::vector<ProbeHit>& hits, std::vector<double>& pktFrames,
                        std::vector<double>& interMs)
    {
        m_stop.store(true, std::memory_order_release);
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        m_client->Stop();
        {
            std::lock_guard<std::mutex> guard(m_mutex);
            hits = std::move(m_hits);
            pktFrames = std::move(m_packetFrames);
            interMs = std::move(m_interMs);
        }
    }

    // Snapshot + CLEAR the results collected so far WITHOUT stopping the client,
    // so the same client can keep the device stream running (a persistent
    // capture client like Discord) after the probe phase.
    void snapshotAndClear(std::vector<ProbeHit>& hits, std::vector<double>& pktFrames,
                          std::vector<double>& interMs)
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        hits.swap(m_hits);
        pktFrames.swap(m_packetFrames);
        interMs.swap(m_interMs);
        m_hits.clear();
        m_packetFrames.clear();
        m_interMs.clear();
    }

    ~WasapiLatencyCapture()
    {
        m_stop.store(true, std::memory_order_release);
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        if (m_client != nullptr)
        {
            m_client->Stop();
        }
        cleanup();
    }

private:
    void run()
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        LONGLONG last = 0;
        while (!m_stop.load(std::memory_order_acquire))
        {
            if (WaitForSingleObject(m_event, 50) != WAIT_OBJECT_0)
            {
                continue;
            }
            UINT32 packetLength = 0;
            while (SUCCEEDED(m_capture->GetNextPacketSize(&packetLength)) && packetLength > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 devicePos = 0;
                UINT64 qpcPos = 0;
                if (FAILED(m_capture->GetBuffer(&data, &frames, &flags, &devicePos, &qpcPos)))
                {
                    break;
                }
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                const LONGLONG receive = now.QuadPart;
                std::lock_guard<std::mutex> guard(m_mutex);
                if (last != 0)
                {
                    m_interMs.push_back((double)(receive - last) * 1000.0 / (double)freq.QuadPart);
                }
                last = receive;
                m_packetFrames.push_back((double)frames);

                if (data != nullptr && frames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                {
                    m_frameScratch.resize(frames);
                    convertToMono(data, frames, m_frameScratch.data());
                    for (UINT32 i = 0; i < frames; ++i)
                    {
                        if (std::fabs(m_frameScratch[i]) > 0.5f)
                        {
                            ProbeHit h;
                            h.deviceQpc = (LONGLONG)((LONGLONG)qpcPos +
                                                     (double)i * (double)freq.QuadPart / 48000.0);
                            h.receivedQpc = receive;
                            m_hits.push_back(h);
                            break; // one marker per packet max
                        }
                    }
                }
                m_capture->ReleaseBuffer(frames);
            }
        }
    }

    void convertToMono(const BYTE* data, UINT32 frames, float* out)
    {
        const WAVEFORMATEX* w = m_mix;
        const UINT32 ch = w != nullptr ? w->nChannels : 0;
        if (ch == 0)
        {
            std::fill(out, out + frames, 0.0f);
            return;
        }
        const UINT32 bits = w->wBitsPerSample;
        const bool isFloat =
            w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w)->SubFormat ==
                 KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        const UINT32 n = std::min<UINT32>(ch, 2u);
        if (isFloat && bits == 32)
        {
            const float* p = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < frames; ++i)
            {
                float s = 0.0f;
                for (UINT32 c = 0; c < n; ++c)
                {
                    s += p[i * ch + c];
                }
                out[i] = s / (float)n;
            }
        }
        else if (bits == 32)
        {
            const int* p = reinterpret_cast<const int*>(data);
            for (UINT32 i = 0; i < frames; ++i)
            {
                float s = 0.0f;
                for (UINT32 c = 0; c < n; ++c)
                {
                    s += static_cast<float>(p[i * ch + c]);
                }
                out[i] = s / 2147483647.0f / (float)n;
            }
        }
        else if (bits == 16)
        {
            const short* p = reinterpret_cast<const short*>(data);
            for (UINT32 i = 0; i < frames; ++i)
            {
                float s = 0.0f;
                for (UINT32 c = 0; c < n; ++c)
                {
                    s += static_cast<float>(p[i * ch + c]);
                }
                out[i] = s / 32767.0f / (float)n;
            }
        }
        else
        {
            std::fill(out, out + frames, 0.0f);
        }
    }

    void CoUninitializeIfOwned()
    {
        if (m_comInitOwned)
        {
            CoUninitialize();
            m_comInitOwned = false;
        }
    }

    void cleanup()
    {
        if (m_capture)
        {
            m_capture->Release();
            m_capture = nullptr;
        }
        if (m_client)
        {
            m_client->Release();
            m_client = nullptr;
        }
        if (m_mix)
        {
            CoTaskMemFree(m_mix);
            m_mix = nullptr;
        }
        if (m_event)
        {
            CloseHandle(m_event);
            m_event = nullptr;
        }
        if (m_device)
        {
            m_device->Release();
            m_device = nullptr;
        }
        CoUninitializeIfOwned();
    }

    IMMDevice* m_device = nullptr;
    IAudioClient* m_client = nullptr;
    IAudioCaptureClient* m_capture = nullptr;
    WAVEFORMATEX* m_mix = nullptr;
    HANDLE m_event = nullptr;
    UINT32 m_bufferFrames = 0;
    REFERENCE_TIME m_latency100ns = 0;
    bool m_comInitOwned = false;
    std::atomic<bool> m_stop{false};
    std::mutex m_mutex; // guards m_hits / m_packetFrames / m_interMs during live capture
    std::thread m_thread;
    std::vector<ProbeHit> m_hits;
    std::vector<double> m_packetFrames;
    std::vector<double> m_interMs;
    std::vector<float> m_frameScratch;
};

// ---------------------------------------------------------------------------
// Aggregate PC-audio output meter (Mini Monitor). Read-only
// IAudioMeterInformation on the active iD14 render endpoint, polled by a small
// non-realtime worker. Publishes an atomic linear peak; -1.0 means "no safe iD14
// render endpoint found" (the UI shows unavailable, never a fabricated value).
// Never touches the ASIO path, Pico, or the virtual mic.
// ---------------------------------------------------------------------------
class PcAudioPeakMeter
{
public:
    void start()
    {
        if (m_thread.joinable())
        {
            return;
        }
        m_stop.store(false, std::memory_order_release);
        m_thread = std::thread(&PcAudioPeakMeter::run, this);
    }
    void stop()
    {
        m_stop.store(true, std::memory_order_release);
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        m_peakL.store(-1.0f, std::memory_order_relaxed);
        m_peakR.store(-1.0f, std::memory_order_relaxed);
    }
    ~PcAudioPeakMeter() { stop(); }
    float peakL() const { return m_peakL.load(std::memory_order_relaxed); }
    float peakR() const { return m_peakR.load(std::memory_order_relaxed); }

    // Windows SYSTEM endpoint mute state read by the worker from
    // IAudioEndpointVolume::GetMute(): nullopt = endpoint/mute API unavailable.
    std::optional<bool> systemMuted() const
    {
        const int v = m_muted.load(std::memory_order_relaxed);
        if (v < 0)
        {
            return std::nullopt;
        }
        return v != 0;
    }
    // UI/command path (never the ASIO callback): record a requested endpoint mute.
    // The worker performs SetMute + GetMute readback; the published state above is
    // authoritative, not the request.
    void requestSystemMute(bool on) { m_pendingMute.store(on ? 1 : 0, std::memory_order_release); }

private:
    static bool nameMatches(const std::wstring& raw)
    {
        std::wstring s;
        s.reserve(raw.size());
        for (wchar_t c : raw)
        {
            s.push_back(c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c);
        }
        return s.find(L"audient") != std::wstring::npos || s.find(L"id14") != std::wstring::npos;
    }
    static std::wstring friendlyName(IMMDevice* dev)
    {
        IPropertyStore* store = nullptr;
        std::wstring name;
        if (dev != nullptr && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)) && store != nullptr)
        {
            PROPVARIANT var;
            PropVariantInit(&var);
            if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &var)) &&
                var.vt == VT_LPWSTR && var.pwszVal != nullptr)
            {
                name = var.pwszVal;
            }
            PropVariantClear(&var);
            store->Release();
        }
        return name;
    }
    IMMDevice* findEndpoint(IMMDeviceEnumerator* enumerator)
    {
        // Prefer the DEFAULT console render endpoint when it is the iD14 (that is
        // where normal Windows playback goes); otherwise use the first active iD14
        // render endpoint. No match => unavailable (the UI never shows fake data).
        IMMDevice* def = nullptr;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &def)) && def != nullptr)
        {
            if (nameMatches(friendlyName(def)))
            {
                return def;
            }
            def->Release();
        }
        IMMDeviceCollection* all = nullptr;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all)) || all == nullptr)
        {
            return nullptr;
        }
        IMMDevice* found = nullptr;
        UINT count = 0;
        all->GetCount(&count);
        for (UINT i = 0; i < count && found == nullptr; ++i)
        {
            IMMDevice* dev = nullptr;
            if (FAILED(all->Item(i, &dev)) || dev == nullptr)
            {
                continue;
            }
            if (nameMatches(friendlyName(dev)))
            {
                found = dev; // keep the reference
            }
            else
            {
                dev->Release();
            }
        }
        all->Release();
        return found;
    }
    void markUnavailable()
    {
        m_peakL.store(-1.0f, std::memory_order_relaxed);
        m_peakR.store(-1.0f, std::memory_order_relaxed);
        m_muted.store(-1, std::memory_order_relaxed);
        m_pendingMute.store(-1, std::memory_order_relaxed);
    }
    void run()
    {
        const HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comOwned = SUCCEEDED(hrInit);
        if (!comOwned && hrInit != RPC_E_CHANGED_MODE)
        {
            markUnavailable();
            return;
        }
        while (!m_stop.load(std::memory_order_acquire))
        {
            IMMDeviceEnumerator* enumerator = nullptr;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator), (void**)&enumerator)) ||
                enumerator == nullptr)
            {
                markUnavailable();
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            IMMDevice* dev = findEndpoint(enumerator);
            enumerator->Release();
            if (dev == nullptr)
            {
                markUnavailable();
                std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                continue;
            }
            IAudioMeterInformation* meter = nullptr;
            if (FAILED(dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)&meter)) ||
                meter == nullptr)
            {
                meter = nullptr;
            }
            // Windows SYSTEM endpoint mute (IAudioEndpointVolume). Endpoint-wide
            // state only; GetMute() read by this non-RT worker is the source of
            // truth. Unavailable (null) if activation fails or the endpoint goes
            // away - ASIO/Pico/VST/hardware control are never involved here.
            IAudioEndpointVolume* vol = nullptr;
            if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&vol)) ||
                vol == nullptr)
            {
                vol = nullptr;
            }
            // Stereo L/R from the endpoint's real per-channel peaks. GetPeakValue()
            // is only the aggregate and must never be duplicated into two bars.
            UINT32 channels = 0;
            const bool stereoMeter =
                (meter != nullptr) && SUCCEEDED(meter->GetMeteringChannelCount(&channels)) && channels >= 2;
            if (!m_logged)
            {
                m_logged = true;
                std::printf("[pc-meter] iD14 render endpoint: %ls (meteringChannels=%u, muteApi=%s)\n",
                            friendlyName(dev).c_str(), stereoMeter ? channels : 0u,
                            vol != nullptr ? "ok" : "unavailable");
            }
            while (!m_stop.load(std::memory_order_acquire))
            {
                if (meter == nullptr && vol == nullptr)
                {
                    break; // endpoint changed/disappeared: re-resolve
                }
                if (vol != nullptr)
                {
                    const int pending = m_pendingMute.exchange(-1, std::memory_order_acq_rel);
                    if (pending >= 0)
                    {
                        // One SetMute attempt; GetMute readback below publishes truth.
                        vol->SetMute(pending ? TRUE : FALSE, nullptr);
                    }
                    BOOL muted = FALSE;
                    if (SUCCEEDED(vol->GetMute(&muted)))
                    {
                        const int next = muted ? 1 : 0;
                        if (m_muted.load(std::memory_order_relaxed) != next)
                        {
                            std::printf("[pc-meter] SYSTEM endpoint mute=%s\n", muted ? "on" : "off");
                        }
                        m_muted.store(next, std::memory_order_relaxed);
                    }
                    else
                    {
                        m_muted.store(-1, std::memory_order_relaxed);
                    }
                }
                else
                {
                    m_muted.store(-1, std::memory_order_relaxed);
                    m_pendingMute.store(-1, std::memory_order_relaxed);
                }

                if (stereoMeter)
                {
                    float peaks[8] = {};
                    const UINT32 n = channels > 8 ? 8u : channels;
                    if (FAILED(meter->GetChannelsPeakValues(n, peaks)))
                    {
                        break; // endpoint changed/disappeared: re-resolve
                    }
                    float l = peaks[0];
                    float r = peaks[1];
                    if (!std::isfinite(l)) l = 0.0f;
                    if (!std::isfinite(r)) r = 0.0f;
                    m_peakL.store(l < 0.0f ? 0.0f : (l > 1.0f ? 1.0f : l), std::memory_order_relaxed);
                    m_peakR.store(r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r), std::memory_order_relaxed);
                }
                else
                {
                    // Fewer than 2 channels: stereo meter unavailable (never fake L=R).
                    m_peakL.store(-1.0f, std::memory_order_relaxed);
                    m_peakR.store(-1.0f, std::memory_order_relaxed);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
            if (vol != nullptr)
            {
                vol->Release();
            }
            if (meter != nullptr)
            {
                meter->Release();
            }
            dev->Release();
        }
        if (comOwned)
        {
            CoUninitialize();
        }
    }

    std::atomic<bool> m_stop{false};
    std::atomic<float> m_peakL{-1.0f};
    std::atomic<float> m_peakR{-1.0f};
    std::atomic<int> m_muted{-1};       // -1 = unavailable, 0 = unmuted, 1 = muted
    std::atomic<int> m_pendingMute{-1}; // -1 = none, 0 = unmute, 1 = mute
    std::thread m_thread;
    bool m_logged = false;
};

// Match WASAPI marker hits to the injection that produced them: every injection
// P0 (tap-A or feeder) is host-QPC; choose the nearest injection by time and
// record the channel delta. Returns how many hits were matched per channel.
struct ProbeSummary
{
    std::vector<double> tapADeltaMs;   // deviceQpc(address) - injectionQpc
    std::vector<double> feederDeltaMs;
    std::vector<double> tapAPerceivedMs;  // receivedQpc - injectionQpc
    std::vector<double> feederPerceivedMs;
};

void runLatencyProbe(audient::asio::AsioRoutingAdapter& adapter, long asioBuffer,
                     audient::transport::VirtualCaptureTransport& transport,
                     TapCaptureSink& tapSink,
                     audient::virtual_audio::SharedRingCaptureSink& sink,
                     WasapiLatencyCapture& wc)
{
    std::printf("\n== latency probe: physical ASIO input -> virtual endpoint ==\n");

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    std::printf("probe: endpoint client buffer=%u frames streamLatency=%.2f ms\n",
                (unsigned)wc.bufferFrames(), wc.streamLatencyMs());

    std::vector<float> markerA(static_cast<std::size_t>(asioBuffer > 0 ? asioBuffer : 64), 0.0f);
    markerA[0] = 0.95f;
    std::vector<float> markerF(64, 0.0f);
    markerF[0] = 0.95f;

    const int intervalMs = 320;
    const int tapACount = 10;
    const int feederCount = 10;

    std::vector<LONGLONG> p0TapA;
    std::vector<LONGLONG> p0Feeder;
    std::vector<ProbeHit> hits;
    std::vector<double> pktFrames;
    std::vector<double> interMs;

    wc.start();

    // Phase A: inject at the physical ASIO input (the adapter's one-shot probe).
    for (int i = 0; i < tapACount; ++i)
    {
        adapter.armMicInputProbe(markerA.data(), markerA.size());
        Sleep(static_cast<DWORD>(intervalMs));
        const std::uint64_t q = adapter.micInputProbeQpc();
        if (q != 0)
        {
            p0TapA.push_back(static_cast<LONGLONG>(q));
        }
    }

    // Phase B: inject at the capture-ring feeder (pre-sink tap C).
    for (int i = 0; i < feederCount; ++i)
    {
        tapSink.armFeederProbe(markerF);
        Sleep(static_cast<DWORD>(intervalMs));
        const std::uint64_t q = tapSink.lastFeederProbeQpc();
        if (q != 0)
        {
            p0Feeder.push_back(static_cast<LONGLONG>(q));
        }
    }

    Sleep(static_cast<DWORD>(intervalMs));
    // Snapshot the probe results WITHOUT stopping the client: it stays open (and
    // the engine keeps the device stream running) for the rest of the run, so
    // the ring drains continuously just like a real client (Discord) would.
    wc.snapshotAndClear(hits, pktFrames, interMs);

    ProbeSummary s;
    const LONGLONG windowTicks = (freq.QuadPart * intervalMs) / 1000;
    for (const ProbeHit& h : hits)
    {
        // Nearest injection (tap-A or feeder) within +/- half the interval.
        LONGLONG best = 0;
        double bestAbs = 1e18;
        bool bestIsTapA = false;
        const auto consider = [&](LONGLONG p0, bool isTapA) {
            const double t = static_cast<double>(h.deviceQpc - p0);
            const double a = t < 0 ? -t : t;
            if (a < bestAbs)
            {
                bestAbs = a;
                best = p0;
                bestIsTapA = isTapA;
            }
        };
        for (LONGLONG p0 : p0TapA)
        {
            consider(p0, true);
        }
        for (LONGLONG p0 : p0Feeder)
        {
            consider(p0, false);
        }
        if (best == 0 || bestAbs > (double)windowTicks)
        {
            continue; // unpaired hit
        }
        const double deviceMs = static_cast<double>(h.deviceQpc - best) * 1000.0 / (double)freq.QuadPart;
        const double perceivedMs =
            static_cast<double>(h.receivedQpc - best) * 1000.0 / (double)freq.QuadPart;
        if (bestIsTapA)
        {
            s.tapADeltaMs.push_back(deviceMs);
            s.tapAPerceivedMs.push_back(perceivedMs);
        }
        else
        {
            s.feederDeltaMs.push_back(deviceMs);
            s.feederPerceivedMs.push_back(perceivedMs);
        }
    }

    const auto summarize = [&](const char* label, const std::vector<double>& vMs) {
        if (vMs.empty())
        {
            std::printf("  %-34s n=0\n", label);
            return;
        }
        std::vector<double> srt = vMs;
        std::sort(srt.begin(), srt.end());
        double sum = 0.0;
        for (double x : srt)
        {
            sum += x;
        }
        std::printf("  %-34s n=%zu min=%8.2f p50=%8.2f p95=%8.2f max=%8.2f ms\n",
                    label, srt.size(), srt.front(), srt[srt.size() / 2],
                    srt[(size_t)(0.95 * (double)(srt.size() - 1))], srt.back());
    };

    // deviceQpc - P0: producer->driver-position (ring + kernel fill + engine).
    summarize("tap-A -> engine device-position", s.tapADeltaMs);
    summarize("feeder -> engine device-position", s.feederDeltaMs);
    // receivedQpc - P0: total producer -> WASAPI-visible.
    summarize("tap-A -> WASAPI-visible (total)", s.tapAPerceivedMs);
    summarize("feeder -> WASAPI-visible (total)", s.feederPerceivedMs);

    if (pktFrames.size() > 4)
    {
        std::vector<double> fs = pktFrames;
        std::sort(fs.begin(), fs.end());
        double sumI = 0.0;
        for (double v : interMs)
        {
            sumI += v;
        }
        const double meanPeriod = interMs.empty() ? 0.0 : sumI / (double)interMs.size();
        std::printf("  WASAPI packet frames p50=%.0f max=%.0f; inter-arrival mean=%.3f ms"
                    " (= engine notification period)\n",
                    fs[fs.size() / 2], fs.back(), meanPeriod);
    }

    // Current ring backlogs at end of probe.
    const std::uint64_t w = sink.region()->writePos;
    const std::uint64_t r = sink.region()->readPos;
    const std::uint64_t backlog = w > r ? w - r : 0;
    std::printf("  capture-ring backlog at end: %llu frames (%.2f ms)\n",
                static_cast<unsigned long long>(backlog),
                static_cast<double>(backlog) * 1000.0 / 48000.0);
    std::printf("  transport ring available at end: %llu frames (%.2f ms)\n",
                static_cast<unsigned long long>(transport.availableFrames()),
                static_cast<double>(transport.availableFrames()) * 1000.0 / 48000.0);
}

// --- A4 automatic device recovery: re-creatable ASIO stream session -------------
//
// A StreamSession owns one complete ASIO session (backend + routing adapter +
// negotiated geometry). Device-loss recovery REBUILDS the session from scratch:
// the old dead stream is never resumed (AGENTS §8 / Q5-A4). All persistent
// runtime (VST3 chain, capture transport, recorder taps, monitor/render config,
// control plane) is re-attached to a fresh adapter via ReconnectWiringFn on the
// control thread BEFORE streaming starts.

struct StreamSession
{
    std::unique_ptr<audient::asio::AsioBackend> backend;
    std::unique_ptr<audient::asio::AsioRoutingAdapter> adapter;
    long buffer = 0;
    long sampleRate = 48000;
    long micInput = -1;
    audient::asio::ChannelPlan plan;
    std::vector<long> supportedRates;
    std::vector<long> supportedBuffers;
};

// Re-wires the shared runtime to a fresh adapter (chains, capture transport,
// render config, recorder taps). Control thread only; never audio-thread.
using ReconnectWiringFn = std::function<void(audient::asio::AsioRoutingAdapter&)>;

// App-side recoverable ASIO stream: select device -> derive plan -> negotiate
// buffer -> build routing adapter -> wire runtime -> start. The first open is
// verbose (full banner); recovery re-opens are terse and PRESERVE the first
// (requested) sample rate (48 kHz) and first-negotiated buffer/mic channel.
class DemoAsioStream final : public audient::asio::IRecoverableAsioStream
{
public:
    DemoAsioStream(audient::asio::NativeAsioDriverProvider& provider,
                   audient::asio::DriverIdentity identity,
                   long bufferArg,
                   long micInputArg,
                   bool dualMode,
                   ReconnectWiringFn wiring)
        : m_provider(provider)
        , m_identity(std::move(identity))
        , m_bufferArg(bufferArg)
        , m_micInputArg(micInputArg)
        , m_dualMode(dualMode)
        , m_wiring(std::move(wiring))
    {
    }

    DemoAsioStream(const DemoAsioStream&) = delete;
    DemoAsioStream& operator=(const DemoAsioStream&) = delete;

    // IRecoverableAsioStream -----------------------------------------------------
    std::uint64_t callbackCount() override
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        return m_session != nullptr ? m_session->backend->callbackCount() : 0u;
    }

    bool openAndStart(std::string& error) override
    {
        const bool verbose = m_firstBuild;
        m_rebuilding.store(true);
        std::unique_ptr<StreamSession> candidate;
        const auto buildBegin = std::chrono::steady_clock::now();
        if (!buildAndStart(candidate, error, verbose))
        {
            // Leave m_rebuilding=true: the recovery loop will retry, and the UI
            // must keep showing Reconfiguring (never a bogus/absent buffer).
            std::printf("[audio-timing] rebuild FAILED after %.1f ms (%s)\n",
                        std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - buildBegin).count(),
                        error.c_str());
            return false;
        }
        const double buildMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - buildBegin).count();
        {
            // Short critical section: swap the session pointer only. The long
            // build already completed OUTSIDE this lock.
            std::lock_guard<std::mutex> guard(m_mtx);
            m_session = std::move(candidate);
            m_firstBuild = false;
            if (m_firstBuffer == 0)
            {
                m_firstBuffer = m_session->buffer;
            }
            if (m_firstMicInput < 0)
            {
                m_firstMicInput = m_session->micInput;
            }
        }
        // Publish the coherent UI snapshot AFTER the swap.
        m_publishedBuffer.store(m_session->buffer);
        m_publishedRate.store(m_session->sampleRate);
        {
            std::lock_guard<std::mutex> guard(m_metaMtx);
            m_supportedBuffersCache = m_session->supportedBuffers;
            m_supportedRatesCache = m_session->supportedRates;
        }
        m_rebuilding.store(false);
        std::printf("[audio-timing] rebuild OK in %.1f ms (buffer=%ld)\n", buildMs, m_session->buffer);
        return true;
    }

    void closeStream() override
    {
        // Tear down the dead stream on a control thread. We do NOT call
        // stopStream() into a driver whose device vanished (it may block):
        // releasing the adapter staging + the driver object is the clean exit.
        // IMPORTANT: move the session OUT under the short lock, then detach
        // OUTSIDE it, so a UI snapshot getter can never wait on a long detach.
        std::unique_ptr<StreamSession> session;
        {
            std::lock_guard<std::mutex> guard(m_mtx);
            session = std::move(m_session);
        }
        if (session != nullptr)
        {
            session->adapter->detach();
        }
    }

    // Status snapshot (single lock) for the status loop / teardown summary.
    // In --dual, Mic RAW/POST are authoritative CH1 per-slot meters (RAW pre
    // VST, POST post-VST, pre shared fade), so the legacy Mic line and Dual CH1
    // agree when both represent Analog 1. Ch2 is analog 2 on runtime slot 1.
    struct Status
    {
        bool live = false; // a session is currently present
        std::string stateText; // stateName(...) or "(none)"
        long buffer = 0;
        std::uint64_t callbacks = 0;
        std::uint64_t xruns = 0;
        std::uint64_t overloads = 0;
        double lastMs = 0.0;
        double p95Ms = 0.0;
        double micDb = -120.0; // meter silence/disconnected by default (CH1 POST in --dual)
        float ch1RawPeak = 0.0f;
        float ch1PostPeak = 0.0f;
        float ch2RawPeak = 0.0f;
        float ch2PostPeak = 0.0f;
        std::string fadeText; // full / muted / mid / "--"
        bool dualPlan = false;
        std::uint32_t inputCount = 0;
        std::size_t configuredInputs = 0;
    };

    Status snapshotStatus()
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        Status s;
        s.buffer = m_publishedBuffer.load();
        if (m_session == nullptr)
        {
            // During a planned reconfiguration keep the LAST STABLE state text so
            // the UI never flashes a misleading "ASIO (none)" mid-transition.
            s.stateText = m_rebuilding.load() ? m_lastStateText : "(none)";
            s.fadeText = "--";
            return s;
        }
        s.live = true;
        s.buffer = m_session->buffer;
        s.callbacks = m_session->backend->callbackCount();
        s.xruns = m_session->backend->xrunCount();
        s.overloads = m_session->backend->overloadCount();
        s.lastMs = m_session->backend->lastCallbackMs();
        s.p95Ms = m_session->backend->p95CallbackMs();
        s.stateText = stateName(m_session->backend->state());
        m_lastStateText = s.stateText;
        s.fadeText = m_session->adapter->fadeIsFull() ? "full"
            : (m_session->adapter->fadeIsMuted() ? "muted" : "mid");
        s.dualPlan = m_session->plan.inputCount > 1 || m_dualMode;
        s.inputCount = m_session->plan.inputCount;
        s.configuredInputs = m_session->adapter->configuredInputChannels();
        s.ch1RawPeak = m_session->adapter->inputRawMeter(0).peakAbs;
        s.ch1PostPeak = m_session->adapter->inputPostMeter(0).peakAbs;
        if (s.inputCount > 1 && s.configuredInputs > 1)
        {
            s.ch2RawPeak = m_session->adapter->inputRawMeter(1).peakAbs;
            s.ch2PostPeak = m_session->adapter->inputPostMeter(1).peakAbs;
        }
        else
        {
            s.ch2RawPeak = 0.0f;
            s.ch2PostPeak = 0.0f;
        }
        if (s.dualPlan)
        {
            const float peakPost = s.ch1PostPeak;
            s.micDb = peakPost > 0.0001f ? 20.0 * std::log10(peakPost) : -120.0;
        }
        else
        {
            const float peak = m_session->adapter->micUplinkPeak();
            s.micDb = peak > 0.0001f ? 20.0 * std::log10(peak) : -120.0;
        }
        return s;
    }

    // Control-thread copy of the physical-output de-pop startup diagnostics.
    bool depopSnapshot(audient::asio::AsioRoutingAdapter::DepopSnapshot& out)
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        if (m_session == nullptr)
        {
            return false;
        }
        out = m_session->adapter->depopSnapshot();
        return true;
    }

    audient::asio::AsioRoutingAdapter::RenderConfig snapshotRenderConfig()
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        if (m_session == nullptr)
        {
            return {};
        }
        return m_session->adapter->renderConfig();
    }

    void snapshotChannelRuntime(std::size_t ch, audient::channel::ChannelRuntimeSnapshot& out)
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        if (m_session == nullptr)
        {
            out = {};
            return;
        }
        out = m_session->adapter->channelRuntime(ch);
    }

    // Startup-probe accessors (called before the watchdog thread exists, so the
    // FIRST session is stable while the probe runs).
    audient::asio::AsioRoutingAdapter* probeAdapter()
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        return m_session != nullptr ? m_session->adapter.get() : nullptr;
    }

    long buffer()
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        return m_session != nullptr ? m_session->buffer : m_firstBuffer;
    }

    long micInput()
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        return m_session != nullptr ? m_session->micInput : m_firstMicInput;
    }

    // --- Audio settings (buffer-only; 48 kHz locked) ---------------------------
    // Every value the UI reads comes from a coherent cache owned by the control
    // side. The UI snapshot path never touches the live ASIO session/driver, so
    // it can never block on a teardown/rebuild.
    std::vector<long> supportedBuffers()
    {
        std::lock_guard<std::mutex> guard(m_metaMtx);
        return m_supportedBuffersCache;
    }
    std::vector<long> supportedSampleRates()
    {
        std::lock_guard<std::mutex> guard(m_metaMtx);
        return m_supportedRatesCache;
    }
    long configuredSampleRate() const { return m_requestedRate.load(); }
    long requestedBuffer() const { return m_requestedBuffer.load(); }
    long currentBuffer() const { return m_publishedBuffer.load(); }
    // Cached actual rate (polled on the control thread; 0 until first poll).
    long actualSampleRate() const { return m_publishedActualRate.load(); }
    // Control-thread poll: query the live driver without holding the session lock
    // across the COM call, then cache the result for the UI.
    void pollActualRate()
    {
        audient::asio::AsioBackend* backend = nullptr;
        {
            std::lock_guard<std::mutex> guard(m_mtx);
            backend = m_session != nullptr ? m_session->backend.get() : nullptr;
        }
        long rate = 0;
        if (backend != nullptr && backend->queryDriverSampleRate(rate) && rate > 0)
        {
            m_publishedActualRate.store(rate);
        }
    }
    // Control-thread driver buffer query (no session lock held across the COM call).
    long driverBufferSize()
    {
        audient::asio::AsioBackend* backend = nullptr;
        {
            std::lock_guard<std::mutex> guard(m_mtx);
            backend = m_session != nullptr ? m_session->backend.get() : nullptr;
        }
        long buffer = 0;
        return (backend != nullptr && backend->queryDriverBufferSize(buffer)) ? buffer : 0;
    }
    unsigned bufferChangeNotifyCount()
    {
        audient::asio::AsioBackend* backend = nullptr;
        {
            std::lock_guard<std::mutex> guard(m_mtx);
            backend = m_session != nullptr ? m_session->backend.get() : nullptr;
        }
        return backend != nullptr ? backend->driverBufferSizeChangeNotifyCount() : 0u;
    }
    // Adopt an externally chosen buffer as the next rebuild target (no range
    // validation here: the caller validates against the engine ceiling).
    void stageExternalBuffer(long samples)
    {
        if (samples > 0)
        {
            m_requestedBuffer.store(samples);
        }
    }
    bool bufferSupported(long samples)
    {
        std::lock_guard<std::mutex> guard(m_metaMtx);
        for (const long candidate : m_supportedBuffersCache)
        {
            if (candidate == samples)
            {
                return true;
            }
        }
        return false;
    }
    bool rebuilding() const { return m_rebuilding.load(); }
    // Publish the Reconfiguring state immediately when a request is accepted so
    // the UI reacts before the control worker starts its teardown.
    void markReconfiguring() { m_rebuilding.store(true); }
    // Stage a validated buffer request. Deduplicated + coalesced: selecting the
    // already-active buffer or repeating the pending target is a no-op, and no
    // new transition may start while one is already in flight.
    bool requestBuffer(long samples)
    {
        if (samples <= 0 || m_rebuilding.load())
        {
            return false;
        }
        if (samples == m_publishedBuffer.load())
        {
            return false; // already the active buffer
        }
        if (samples == m_requestedBuffer.load())
        {
            return false; // already pending
        }
        {
            std::lock_guard<std::mutex> guard(m_metaMtx);
            bool allowed = false;
            for (const long candidate : m_supportedBuffersCache)
            {
                if (candidate == samples)
                {
                    allowed = true;
                    break;
                }
            }
            if (!allowed)
            {
                return false;
            }
        }
        m_requestedBuffer.store(samples);
        return true;
    }

    // True when the negotiated plan actually created a secondary ASIO output pair
    // (slots 2/3). In v1 the Local Monitor uses Output 1/2 only, so this is false
    // unless the AUDIENT_ASIO_FORCE4OUT diagnostic added a second pair.
    bool hasSecondaryOutputPair()
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        return m_session != nullptr && m_session->plan.outputLeft2 >= 0 &&
               m_session->plan.outputRight2 >= 0;
    }

    // Control-thread LIVE render-config update on the current session's adapter
    // (e.g. toggling the processed-mic local monitor). Lifetime-safe against the
    // recovery watchdog: the whole read-mutate-publish happens under the same
    // mutex that guards session replacement, so no concurrent closeStream() can
    // free the adapter mid-update. Bumps the revision so the callback re-applies
    // the new target through the click-free gain ramps at the next block.
    void updateRenderConfig(const std::function<void(audient::asio::AsioRoutingAdapter::RenderConfig&)>& mutate)
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        if (m_session == nullptr)
        {
            return;
        }
        audient::asio::AsioRoutingAdapter::RenderConfig config = m_session->adapter->renderConfig();
        ++config.revision;
        mutate(config);
        m_session->adapter->publishRenderConfig(config);
    }
    void updateChannelRuntime(std::size_t channel,
                              const std::function<void(audient::channel::ChannelRuntimeSnapshot&)>& mutate)
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        if (m_session == nullptr)
        {
            return;
        }
        audient::channel::ChannelRuntimeSnapshot snap = m_session->adapter->channelRuntime(channel);
        mutate(snap);
        m_session->adapter->publishChannelRuntime(channel, snap);
    }

    // Diagnostic output-pair tone (safe channel identification; low level only).
    // Lifetime-safe against the recovery watchdog like updateRenderConfig.
    void armOutputTone(std::uint32_t pair, float amplitudeLinear)
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        if (m_session != nullptr)
        {
            m_session->adapter->armOutputPairTone(pair, amplitudeLinear);
        }
    }
    void disarmOutputTone()
    {
        std::lock_guard<std::mutex> guard(m_mtx);
        if (m_session != nullptr)
        {
            m_session->adapter->disarmOutputPairTone();
        }
    }

    // Graceful shutdown (fade -> stop -> detach) of the current session. Run on
    // the main thread AFTER the watchdog has been asked to stop and joined.
    // The session is moved OUT under a short lock; the long fade/stop/detach
    // happens with the lock released so UI snapshot getters never block on it.
    void gracefulTeardown()
    {
        std::unique_ptr<StreamSession> session;
        {
            std::lock_guard<std::mutex> guard(m_mtx);
            session = std::move(m_session);
        }
        if (session == nullptr)
        {
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        session->adapter->requestFadeOut();
        const auto deadline = t0 + std::chrono::milliseconds(3000);
        while (std::chrono::steady_clock::now() < deadline && !session->adapter->fadeIsMuted())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto tFade = std::chrono::steady_clock::now();
        std::string error;
        if (!session->backend->stop(error))
        {
            std::printf("WARN: backend stop failed: %s\n", error.c_str());
        }
        const auto tStop = std::chrono::steady_clock::now();
        session->adapter->detach();
        const auto tDetach = std::chrono::steady_clock::now();
        const auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        std::printf("[audio-timing] teardown fade=%.1f stop=%.1f detach=%.1f total=%.1f ms\n",
                    ms(t0, tFade), ms(tFade, tStop), ms(tStop, tDetach), ms(t0, tDetach));
    }

    // FAST teardown for a planned/external BUFFER reconfiguration. A normal
    // graceful fade can never complete once an external iD.exe change has
    // already stopped ASIO callbacks, so waiting the 3 s shutdown fade just
    // delays the rebuild. Policy: request the fade, wait only a short bounded
    // time AND abort early if callbacks have stopped advancing.
    void reconfigureTeardown()
    {
        std::unique_ptr<StreamSession> session;
        {
            std::lock_guard<std::mutex> guard(m_mtx);
            session = std::move(m_session);
        }
        if (session == nullptr)
        {
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::uint64_t lastCallbacks = session->backend->callbackCount();
        session->adapter->requestFadeOut();
        constexpr auto kFastFadeBudget = std::chrono::milliseconds(100);
        constexpr auto kStallGrace = std::chrono::milliseconds(20);
        while ((std::chrono::steady_clock::now() - t0) < kFastFadeBudget &&
               !session->adapter->fadeIsMuted())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            const std::uint64_t now = session->backend->callbackCount();
            if (now == lastCallbacks &&
                (std::chrono::steady_clock::now() - t0) > kStallGrace)
            {
                break; // callbacks already stopped: further waiting is pointless
            }
            lastCallbacks = now;
        }
        const auto tFade = std::chrono::steady_clock::now();
        std::string error;
        (void)session->backend->stop(error);
        const auto tStop = std::chrono::steady_clock::now();
        session->adapter->detach();
        const auto tDetach = std::chrono::steady_clock::now();
        const auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        std::printf("[audio-timing] reconfigure-teardown fade=%.1f stop=%.1f detach=%.1f total=%.1f ms\n",
                    ms(t0, tFade), ms(tFade, tStop), ms(tStop, tDetach), ms(t0, tDetach));
    }

    void dispose()
    {
        std::unique_ptr<StreamSession> session;
        {
            std::lock_guard<std::mutex> guard(m_mtx);
            session = std::move(m_session);
        }
        if (session != nullptr)
        {
            session->adapter->detach();
        }
    }

private:
    bool buildAndStart(std::unique_ptr<StreamSession>& out, std::string& error, bool verbose)
    {
        auto session = std::make_unique<StreamSession>();
        session->backend = std::make_unique<audient::asio::AsioBackend>(m_provider);
        if (!session->backend->selectDevice(m_identity, error))
        {
            return false;
        }
        const audient::asio::DriverCapabilities& caps = session->backend->driverCapabilities();

        if (verbose)
        {
            std::printf("driver: %s ({%s})\n", m_identity.name.c_str(), m_identity.clsid.c_str());
            std::printf("CAPS: %ld in / %ld out; rates:", caps.inputChannels, caps.outputChannels);
            for (const long rate : caps.supportedSampleRates)
            {
                std::printf(" %ld", rate);
            }
            std::printf("\n");
            std::printf("BUFFER: min=%ld max=%ld preferred=%ld granularity=%ld\n",
                        caps.bufferSizes.minimum, caps.bufferSizes.maximum,
                        caps.bufferSizes.preferred, caps.bufferSizes.granularity);
            std::printf("LATENCY: input=%ld output=%ld samples\n",
                        caps.latencies.input, caps.latencies.output);
        }

        std::vector<audient::asio::ChannelInfo> inputs;
        std::vector<audient::asio::ChannelInfo> outputs;
        for (const audient::asio::ChannelInfo& channel : caps.channels)
        {
            if (!channel.isActive)
            {
                continue;
            }
            (channel.isInput ? inputs : outputs).push_back(channel);
        }
        if (verbose)
        {
            std::printf("ACTIVE INPUT CHANNELS (index \"name\" ASIO-sample-type):\n");
            for (const audient::asio::ChannelInfo& channel : inputs)
            {
                std::printf("  [%2ld] \"%-16s\" type=%s\n", channel.index, channel.name.c_str(),
                            sampleFormatName(channel.preferredFormat));
            }
            std::printf("ACTIVE OUTPUT CHANNELS:\n");
            for (const audient::asio::ChannelInfo& channel : outputs)
            {
                std::printf("  [%2ld] \"%-16s\" type=%s\n", channel.index, channel.name.c_str(),
                            sampleFormatName(channel.preferredFormat));
            }
        }

        audient::asio::ChannelPlan plan;
        if (m_dualMode)
        {
            plan = audient::asio::AsioChannelMap::planProductionInputsAndStereoDownlink(inputs, outputs, false);
        }
        else
        {
            plan = audient::asio::AsioChannelMap::planMicUplinkAndStereoDownlink(inputs, outputs, false);
        }
        if (!plan.valid())
        {
            error = "channel plan invalid (mic input / stereo output mapping failed)";
            return false;
        }

        if (m_dualMode)
        {
            if (plan.inputCount < 1)
            {
                error = "--dual requires at least one analogue input";
                return false;
            }
            if (verbose)
            {
                std::printf("[dual] plan: %u input(s)\n", plan.inputCount);
                for (std::uint32_t slot = 0; slot < plan.inputCount; ++slot)
                {
                    const auto& binding = plan.inputs[slot];
                    std::printf("[dual]   slot %u -> Analogue %u / ASIO index %ld\n",
                                slot, binding.identity.ordinal,
                                binding.asioChannelIndex);
                }
            }
            long micIndex = m_micInputArg >= 0 ? m_micInputArg : plan.micInput;
            bool foundMic = false;
            for (const audient::asio::ChannelInfo& channel : inputs)
            {
                if (channel.index == micIndex)
                {
                    foundMic = true;
                    break;
                }
            }
            if (!foundMic)
            {
                error = "--channel <n> (mic input) is not an active input channel";
                return false;
            }
            plan.micInput = micIndex;
        }
        else
        {
            long micIndex = m_micInputArg >= 0 ? m_micInputArg
                : (m_firstMicInput >= 0 ? m_firstMicInput : plan.micInput);
            bool foundMic = false;
            for (const audient::asio::ChannelInfo& channel : inputs)
            {
                if (channel.index == micIndex)
                {
                    foundMic = true;
                    break;
                }
            }
            if (!foundMic)
            {
                error = "--channel <n> (mic input) is not an active input channel";
                return false;
            }
            plan.micInput = micIndex;
        }

        // V1 architecture: the Local Monitor bus renders ONLY to ASIO Output 1/2.
        // ASIO 3/4 / an independent software headphone bus is a v2+ feature, so
        // drop any secondary pair from the plan unless an env-gated diagnostic
        // explicitly asks for it (AUDIENT_ASIO_FORCE4OUT=1).
        bool force4Out = false;
        {
            char* value = nullptr;
            std::size_t len = 0;
            if (_dupenv_s(&value, &len, "AUDIENT_ASIO_FORCE4OUT") == 0)
            {
                force4Out = (value != nullptr);
                std::free(value);
            }
        }
        if (force4Out && outputs.size() >= 4)
        {
            const bool used2 = (outputs[2].index == plan.outputLeft ||
                                outputs[2].index == plan.outputRight);
            const bool used3 = (outputs[3].index == plan.outputLeft ||
                                outputs[3].index == plan.outputRight);
            if (outputs[2].isActive && outputs[3].isActive && !used2 && !used3)
            {
                plan.outputLeft2 = outputs[2].index;
                plan.outputRight2 = outputs[3].index;
                std::printf("[asio-diag] FORCE4OUT: adding output pair 2/3 "
                            "(\"%s\", \"%s\") to the stream (diagnostic only)\n",
                            outputs[2].name.c_str(), outputs[3].name.c_str());
            }
            else
            {
                std::printf("[asio-diag] FORCE4OUT: outputs[2]/[3] unavailable for a "
                            "second pair (active2=%d active3=%d used=%d/%d)\n",
                            outputs[2].isActive ? 1 : 0, outputs[3].isActive ? 1 : 0,
                            used2 ? 1 : 0, used3 ? 1 : 0);
            }
        }
        else
        {
            plan.outputLeft2 = -1;
            plan.outputRight2 = -1;
        }

        if (verbose)
        {
            const audient::asio::ChannelInfo* chosen = nullptr;
            for (const audient::asio::ChannelInfo& channel : inputs)
            {
                if (channel.index == plan.micInput)
                {
                    chosen = &channel;
                    break;
                }
            }
            std::printf("plan: micIn=%ld (\"%s\", type=%s) outL=%ld outR=%ld",
                        plan.micInput,
                        chosen != nullptr ? chosen->name.c_str() : "?",
                        chosen != nullptr ? sampleFormatName(chosen->preferredFormat) : "?",
                        plan.outputLeft, plan.outputRight);
            if (plan.outputLeft2 >= 0)
            {
                const auto findOut = [&](long index) -> std::string {
                    for (const audient::asio::ChannelInfo& channel : outputs)
                    {
                        if (channel.index == index)
                        {
                            return channel.name;
                        }
                    }
                    return "?";
                };
                std::printf(" outL2=%ld(\"%s\") outR2=%ld(\"%s\")", plan.outputLeft2,
                            findOut(plan.outputLeft2).c_str(), plan.outputRight2,
                            findOut(plan.outputRight2).c_str());
            }
            else
            {
                std::printf(" (no secondary Headphone pair in plan)");
            }
            std::printf("\n");
        }

        // Publish the driver's ACTUAL capabilities so the GUI lists only real,
        // supported values (never hard-coded universals).
        session->supportedRates = caps.supportedSampleRates;
        session->supportedBuffers.clear();
        for (const long candidate : {32L, 64L, 128L, 256L, 512L, 1024L})
        {
            if (candidate > kMaxBlock)
            {
                continue; // engine preallocates for kMaxBlock; never exceed it
            }
            if (candidate < caps.bufferSizes.minimum || candidate > caps.bufferSizes.maximum)
            {
                continue;
            }
            if (caps.bufferSizes.granularity > 0 &&
                (candidate - caps.bufferSizes.minimum) % caps.bufferSizes.granularity != 0)
            {
                continue;
            }
            session->supportedBuffers.push_back(candidate);
        }

        const long sampleRate = m_requestedRate.load() > 0 ? m_requestedRate.load() : 48000;

        // Buffer: user-requested, else --buffer, else the first-negotiated buffer,
        // else 64 -> 128 -> driver preferred (never force unsupported).
        long buffer = 0;
        const long staged = m_requestedBuffer.load();
        long desired = staged > 0 ? staged : (m_bufferArg > 0 ? m_bufferArg : m_firstBuffer);
        if (desired > kMaxBlock)
        {
            desired = 0; // never configure a buffer larger than the engine preallocation
        }
        if (desired > 0 && desired >= caps.bufferSizes.minimum && desired <= caps.bufferSizes.maximum)
        {
            std::string cfgError;
            if (session->backend->configure(sampleRate, desired, plan, cfgError))
            {
                buffer = desired;
            }
            else if (verbose)
            {
                std::printf("configure(%ld,%ld) failed (%s)\n", sampleRate, desired, cfgError.c_str());
            }
        }
        if (buffer == 0)
        {
            const long candidates[] = {64L, 128L, caps.bufferSizes.preferred};
            for (const long candidate : candidates)
            {
                if (candidate > kMaxBlock || candidate < caps.bufferSizes.minimum ||
                    candidate > caps.bufferSizes.maximum)
                {
                    continue;
                }
                std::string cfgError;
                if (session->backend->configure(sampleRate, candidate, plan, cfgError))
                {
                    buffer = candidate;
                    break;
                }
                if (verbose)
                {
                    std::printf("configure(%ld,%ld) failed (%s)\n", sampleRate, candidate, cfgError.c_str());
                }
            }
        }
        if (buffer == 0)
        {
            error = "no buffer size in the driver range could be configured";
            return false;
        }
        if (verbose)
        {
            std::printf("CONFIG: requested %ld Hz / %ld samples / mic-in=%ld\n", sampleRate, buffer, plan.micInput);
        }

        session->adapter = std::make_unique<audient::asio::AsioRoutingAdapter>(*session->backend, kMaxBlock);
        m_wiring(*session->adapter);
        session->adapter->setSourceLatency(audient::routing::BusId::PhysicalInput1,
                                           static_cast<std::uint32_t>(
                                               caps.latencies.input > 0 ? caps.latencies.input : 0));
        session->adapter->attach();
        session->adapter->requestFadeIn();
        if (!session->backend->start(error))
        {
            session->adapter->detach();
            return false;
        }

        session->buffer = buffer;
        session->sampleRate = sampleRate;
        session->micInput = plan.micInput;
        session->plan = plan;
        out = std::move(session);
        return true;
    }

    audient::asio::NativeAsioDriverProvider& m_provider;
    audient::asio::DriverIdentity m_identity;
    long m_bufferArg = 0;
    long m_micInputArg = -1;
    bool m_dualMode = false;
    ReconnectWiringFn m_wiring;

    // m_mtx guards ONLY the m_session pointer swap; it is never held across an
    // ASIO stop/start/detach or a worker join. m_metaMtx guards the immutable
    // capability lists. The UI snapshot path reads the published atomics only.
    std::mutex m_mtx;
    std::unique_ptr<StreamSession> m_session;
    std::mutex m_metaMtx;
    std::vector<long> m_supportedBuffersCache;
    std::vector<long> m_supportedRatesCache;
    bool m_firstBuild = true;
    long m_firstBuffer = 0;
    long m_firstMicInput = -1;
    // Internal processing + transport are fixed at 48 kHz; buffer is user-selectable.
    std::atomic<long> m_requestedRate{48000};
    std::atomic<long> m_requestedBuffer{0};
    std::atomic<bool> m_rebuilding{false};
    // Coherent values published to the UI (never transient/absent).
    std::atomic<long> m_publishedBuffer{0};
    std::atomic<long> m_publishedRate{48000};
    std::atomic<long> m_publishedActualRate{0};
    std::string m_lastStateText = "Streaming"; // main/UI-thread cache
};

} // namespace

int main(int argc, char** argv)
{
    // The distributed product is a Windows GUI app (no console window). Developer
    // diagnostics opt in explicitly with --console, which allocates a console if
    // the process has none. Normal daily launch never depends on a console.
    bool consoleMode = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--console") == 0)
        {
            consoleMode = true;
        }
    }
    if (consoleMode && !ConsoleUi::available())
    {
        if (AllocConsole())
        {
            FILE* f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
            freopen_s(&f, "CONOUT$", "w", stderr);
            freopen_s(&f, "CONIN$", "r", stdin);
        }
    }
    int unused = SetConsoleOutputCP(CP_UTF8);
    (void)unused;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    long channelArg = -1;
    long bufferArg = 0; // 0 = auto (64 -> 128 -> driver preferred)
    long secondsArg = 0;
    long recordSeconds = 8;
    std::vector<std::string> vst3Paths; // repeatable; order = chain order (Channel 0)
    std::vector<std::string> vst3Ch2Paths; // --vst3-ch2 (Channel 1, --dual only)
    bool dualMode = false; // --dual: two analogue inputs via planProductionInputsAndStereoDownlink
    std::string recordPrefix;
    bool monitorMuted = true; // local processed-mic monitor starts OFF (app-level only)
    bool outputMono = false; // MONO OUTPUT fold (diagnostic TUI 'k' + --mono flags)
    // C2 diagnostic per-channel Local Monitor sends (used only when --dual)
    float localMonCh0LevelDb = -6.0f;
    float localMonCh1LevelDb = -18.0f;
    bool localMonCh0Enabled = false;
    bool localMonCh1Enabled = false;
    bool localMonCh0Muted = false;
    bool localMonCh1Muted = false;
    std::uint64_t localMonCh0Rev = 1;
    std::uint64_t localMonCh1Rev = 1;

    // --- Output control state (Monitor bus = Output 1/2, Headphone bus = Output
    // 3/4 when the driver exposes them). Software gain/routing ONLY - never any
    // undocumented Audient hardware volume/mute command. Conservative defaults:
    // both buses start MUTED at a modest level so a fresh/recovered session can
    // never blast at full scale; enable deliberately and levels ramp click-free.
    float monitorLevelDb = -18.0f;
    bool monitorBusMuted = true;   // whole MONITOR bus mute (Output 1/2)
    bool monitorDim = false;
    float monitorDimDb = -20.0f;
    float headphoneLevelDb = -18.0f;
    bool headphoneBusMuted = true; // whole HEADPHONE bus mute (Output 3/4)
    bool micToHeadphones = false;  // processed-mic monitor independently routed to headphones
    bool outputMode = false;       // TUI OUTPUT control mode (o toggles)
    bool outputSelHeadphones = false; // which bus the output-mode keys adjust
    bool identifyOutputArmed = false;

    // --- HARDWARE device volume (official Audient API, ADR-008). Deliberately
    // separate from the software OUTPUT buses above and from the LOCAL processed
    // -mic monitor. Controls the iD14 physical output volume only.
    audient::hardware::AudientHardwareControl hw;
    PcAudioPeakMeter pcMeter; // Mini Monitor PC-audio aggregate meter (read-only)
    bool hwMode = false; // TUI HARDWARE device-volume mode ('h' toggles)
    bool hwSelHeadphones = false; // which device volume the HARDWARE keys adjust
    std::chrono::steady_clock::time_point hwRetryAt{};
    bool hwOpenFailureLogged = false;
    bool hwConnectedPrev = false; // state-transition logging (connected/lost edges)
    bool noHardware = false; // --no-hardware: skip the iD14 device-volume layer
    long hwPollMs = 200;     // --hw-poll-ms <ms>: device read poll cadence (5 Hz default)
    bool useGui = false;     // --gui or no-argument daily launch: WebView product GUI
    bool pickVstAtStart = false; // --pick-vst: diagnostic startup picker only
    bool noVstRestore = false;   // --no-vst-restore: safe-start, ignore mixer-state
    bool requireDriver = false;
    bool probeLatency = false;
    long simulateLossAfterMs = 0;
    bool vst3Enum = false;
    bool vst3Bypass = false;
    bool editorRequested = false;
    long identifyOutputPair = -1; // --identify-output 0|1 (diagnostic pair tone)
    bool testDownlink = false; // --test-downlink: synthetic asymmetric stereo downlink for mono audition (diagnostic only)
    bool picoTransport = false; // --pico: enable the VoxBridge Pico v0.1.1 UAC2 virtual-mic transport
    std::string vst3Dir = "C:\\Program Files\\Common Files\\VST3";
    std::string vst3SaveFile;
    std::string vst3RestoreFile;
    std::vector<std::pair<Steinberg::Vst::ParamID, double>> vst3Params;
    std::vector<std::string> scriptTokens; // --keys tokens (space or '+' separated)

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        const auto nextValue = [&]() -> std::string {
            return (i + 1 < argc) ? std::string(argv[i + 1]) : std::string();
        };
        if (arg == "--channel")
        {
            channelArg = std::strtol(nextValue().c_str(), nullptr, 10);
            ++i;
        }
        else if (arg == "--buffer")
        {
            bufferArg = std::strtol(nextValue().c_str(), nullptr, 10);
            ++i;
        }
        else if (arg == "--seconds")
        {
            secondsArg = std::strtol(nextValue().c_str(), nullptr, 10);
            ++i;
        }
        else if (arg == "--record-seconds")
        {
            recordSeconds = std::strtol(nextValue().c_str(), nullptr, 10);
            ++i;
        }
        else if (arg == "--vst3")
        {
            const std::string path = nextValue();
            if (!path.empty())
            {
                vst3Paths.push_back(path);
            }
            ++i;
        }
        else if (arg == "--vst3-ch2")
        {
            const std::string path = nextValue();
            if (!path.empty())
            {
                vst3Ch2Paths.push_back(path);
            }
            ++i;
        }
        else if (arg == "--dual")
        {
            dualMode = true;
        }
        else if (arg == "--record")
        {
            recordPrefix = nextValue();
            ++i;
        }
        else if (arg == "--monitor-off")
        {
            monitorMuted = true;
        }
        else if (arg == "--monitor-on")
        {
            monitorMuted = false;
        }
        else if (arg == "--mono")
        {
            outputMono = true;
        }
        else if (arg == "--mono-on")
        {
            outputMono = true;
        }
        else if (arg == "--mono-off")
        {
            outputMono = false;
        }
        else if (arg == "--localmon-ch0")
        {
            localMonCh0Enabled = true;
            localMonCh0Muted = false;
            localMonCh0LevelDb = std::stof(nextValue());
            ++i;
        }
        else if (arg == "--localmon-ch1")
        {
            localMonCh1Enabled = true;
            localMonCh1Muted = false;
            localMonCh1LevelDb = std::stof(nextValue());
            ++i;
        }
        else if (arg == "--identify-output")
        {
            // Safe channel-identification: play a LOW-level tone (-30 dBFS,
            // 100 ms on/off) on ONE output pair for the run duration.
            // 0 = primary pair (Output 1/2), 1 = secondary pair (Output 3/4).
            identifyOutputPair = std::strtol(nextValue().c_str(), nullptr, 10);
            ++i;
        }
        else if (arg == "--require-driver")
        {
            requireDriver = true;
        }
        else if (arg == "--probe-latency")
        {
            probeLatency = true;
        }
        else if (arg == "--simulate-loss-after")
        {
            simulateLossAfterMs = std::strtol(nextValue().c_str(), nullptr, 10);
            ++i;
        }
        else if (arg == "--no-hardware")
        {
            // Skip the iD14 HARDWARE device-volume layer entirely (A/B pop test B).
            noHardware = true;
        }
        else if (arg == "--hw-poll-ms")
        {
            hwPollMs = std::strtol(nextValue().c_str(), nullptr, 10);
            if (hwPollMs < 10)
            {
                hwPollMs = 10;
            }
            if (hwPollMs > 5000)
            {
                hwPollMs = 5000;
            }
            ++i;
        }
        else if (arg == "--vst3-enum")
        {
            vst3Enum = true;
        }
        else if (arg == "--vst3-bypass")
        {
            vst3Bypass = true;
        }
        else if (arg == "--vst3-param")
        {
            const std::string spec = nextValue(); // copy: keep the temp alive past strtoul
            if (spec.empty())
            {
                std::printf("WARN: --vst3-param expects <id>:<value>\n");
            }
            else
            {
                char* end = nullptr;
                const unsigned long id = std::strtoul(spec.c_str(), &end, 0);
                if (end != nullptr && *end == ':')
                {
                    const double value = std::strtod(end + 1, nullptr);
                    vst3Params.emplace_back(static_cast<Steinberg::Vst::ParamID>(id), value);
                }
                else
                {
                    std::printf("WARN: --vst3-param expects <id>:<value>\n");
                }
            }
            ++i;
        }
        else if (arg == "--vst3-save")
        {
            vst3SaveFile = nextValue();
            ++i;
        }
        else if (arg == "--vst3-restore")
        {
            vst3RestoreFile = nextValue();
            ++i;
        }
        else if (arg == "--editor")
        {
            editorRequested = true;
        }
        else if (arg == "--test-downlink")
        {
            testDownlink = true;
        }
        else if (arg == "--pico")
        {
            picoTransport = true;
        }
        else if (arg == "--gui")
        {
            useGui = true;
        }
        else if (arg == "--console")
        {
            // Handled before argument parsing (AllocConsole when needed).
        }
        else if (arg == "--pick-vst")
        {
            // Diagnostic-only startup VST picker; never used by the daily GUI.
            pickVstAtStart = true;
        }
        else if (arg == "--no-vst-restore")
        {
            // Safe-start: ignore saved mixer state and begin with empty chains.
            noVstRestore = true;
        }
        else if (arg == "--vst3dir")
        {
            vst3Dir = nextValue();
            ++i;
        }
        else if (arg == "--keys")
        {
            // Scripted key sequence (space or '+' separated) fed through the SAME
            // dispatch path as typed keys - used for automated TUI/key regression
            // runs. Tokens: single characters, or up/down/left/right.
            const std::string spec = nextValue();
            std::string current;
            for (const char c : spec)
            {
                if (c == ' ' || c == '+')
                {
                    if (!current.empty())
                    {
                        scriptTokens.push_back(current);
                        current.clear();
                    }
                }
                else
                {
                    current.push_back(c);
                }
            }
            if (!current.empty())
            {
                scriptTokens.push_back(current);
            }
            ++i;
        }
        else
        {
            std::printf("usage: physical_vmic_demo [--channel <n>] [--vst3 <path>]... [--vst3-ch2 <path>]... [--buffer <frames>]\n"
                        "                      [--dual] [--seconds <N>] [--monitor-off|--monitor-on] [--require-driver]\n"
                        "                      [--record <prefix>] [--record-seconds <N>]\n"
                        "                      [--probe-latency] [--simulate-loss-after <ms>]\n"
                        "                      [--vst3-enum] [--vst3-bypass]\n"
                        "                      [--vst3-param <id>:<value>] [--vst3-save <file>]\n"
                        "                      [--vst3-restore <file>] [--editor] [--vst3dir <dir>]\n"
                         "                      [--identify-output <0|1>] [--test-downlink]\n"
                        "                      [--no-hardware] [--hw-poll-ms <ms>]\n"
                        "  --vst3 <path> is repeatable; order = chain order (max 8). --vst3-param /\n"
                         "  --vst3-restore / --vst3-save apply to the FIRST slot (single-plugin compat).\n"
                         "  --dual enables two analogue inputs (slot0=Analogue 1, slot1=Analogue 2);\n"
                         "         --vst3-ch2 <path> (repeatable) loads Channel 1 chain (requires --dual).\n"
                        "  Local processed-mic monitoring defaults to OFF; --monitor-on enables it.\n"
                        "  Outputs: Monitor = Output 1/2, Headphones = Output 3/4 (confirm with\n"
                        "  --identify-output 0|1, a -30 dBFS tone on one pair). 'o' opens the LOCAL\n"
                        "  OUTPUT control mode and 'h' the HARDWARE (iD14 device-volume) mode in the\n"
                        "  interactive TUI.\n"
                        "  --no-hardware disables the iD14 device-volume layer (pop A/B test B).\n"
                        "  --hw-poll-ms <ms> sets the device read poll cadence (default 200 ms = 5 Hz).\n");
            return 2;
        }
    }

    // --- persistent user preferences (ADR-011) ---------------------------------
    // Bounded local file (%LOCALAPPDATA%\Audient Console\settings.json). Loading
    // failures are non-fatal: safe defaults are used and audio startup proceeds.
    // A normal distributed launch (no arguments) is the daily GUI product; any
    // explicit flag keeps the existing diagnostic/console behavior, and
    // --console forces console diagnostics even with no other flags.
    if (argc <= 1)
    {
        // Installed daily product profile: the accepted daily behavior without
        // CLI flags (equivalent to --dual --gui --pico). Diagnostic explicit CLI
        // launches keep their existing semantics.
        useGui = true;
        dualMode = true;      // CH1 + CH2 analogue inputs
        picoTransport = true; // Pico UAC2 bridge worker
    }

    // Single instance: a second GUI launch must not create a second WebView2
    // environment on the shared %LOCALAPPDATA%\Audient Console\WebView2
    // user-data folder. WebView2 does not support two processes sharing one
    // user-data folder: the second process still renders (shared compositor) but
    // its input/hit-testing breaks (clicks/drags do nothing). When a GUI instance
    // is already running (commonly hidden to the tray), restore and foreground it
    // and exit this process before any WebView/ASIO setup.
    HANDLE singleInstance = nullptr;
    if (useGui)
    {
        singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\AudientConsole.SingleInstance");
        if (singleInstance != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (HWND existing = FindWindowW(L"AudientDailyWebContainer", nullptr))
            {
                ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
            CloseHandle(singleInstance);
            return 0;
        }
    }

    audient::preferences::AppPreferences prefs;
    {
        std::string prefError;
        const std::string prefPath = audient::preferences::defaultPreferencesPath();
        (void)audient::preferences::loadPreferencesFromFile(prefPath, prefs, &prefError);
        std::printf("[prefs] %s (source=%s closeToTray=%d startMinimized=%d)\n",
                    prefPath.empty() ? "<none>" : prefPath.c_str(),
                    audient::preferences::virtualMicSourceName(prefs.virtualMicSource),
                    prefs.closeToTray ? 1 : 0, prefs.startMinimized ? 1 : 0);
        if (!prefError.empty())
        {
            std::printf("[prefs] %s\n", prefError.c_str());
        }
    }
    const auto savePrefs = [&]() {
        std::string error;
        if (!audient::preferences::savePreferences(prefs, &error))
        {
            std::printf("[prefs] save failed: %s\n", error.c_str());
        }
    };

    // Raise the Windows timer resolution so the feeder's sub-ms poll actually
    // wakes at the poll period instead of the ~15.6 ms scheduler tick (see the
    // pacing note in the file header). timeEndPeriod(1) at teardown.
    timeBeginPeriod(1);
    pcMeter.start(); // non-RT read-only IAudioMeterInformation worker

    // Native editor windows render in physical pixels (project guidelines §15).
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
    SetConsoleTitleA("Audient Console - physical mic -> virtual mic demo");

    // Interactive TUI (fixed status pane) when stdout is a real console;
    // redirected output (ssh/tests/--seconds logging) stays plain line output.
    ConsoleUi tui;
    const bool interactiveConsole = ConsoleUi::available();
    if (interactiveConsole)
    {
        tui.begin();
    }

    std::printf("== Audient Console live physical-input virtual-mic demo ==\n");
    std::printf("  path: physical ASIO input -> RoutingCore -> input VST chain -> "
                "VirtualMicFeeder -> SharedRingCaptureSink -> virtual mic driver\n\n");

    // --- 1. Real Audient ASIO driver identity (registry) -----------------------
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
        std::printf("ERROR: no Audient ASIO driver found in the registry (this machine has no iD14/iD-series ASIO).\n");
        timeEndPeriod(1);
        tui.end();
        return 1;
    }

    // --- 2. Input VST3 mic chain (multi-slot, ordered, live-mutable) ------------
    // The demo owns every slot (loader/host/processor/name). The REALTIME thread
    // never touches the vector below: the ASIO callback only runs the immutable
    // snapshot published into micChain. Every chain mutation (add/remove/reorder/
    // per-slot or whole-chain bypass) happens on this control thread and republishes
    // a fresh snapshot (Vst3Chain::publish + grace/reap), so a mutation is
    // swap-atomic at a block boundary. Full VST-007 click-free prepared swaps are
    // later production work and out of this demo's scope.
    struct PluginSlot
    {
        std::unique_ptr<audient::vst3::Vst3ModuleLoader> loader;
        std::unique_ptr<audient::vst3::Vst3Host> host;
        std::unique_ptr<audient::vst3::Vst3Processor> processor;
        std::string name;
        std::string path;
        std::string stateId; // mixer-state insert id (empty until assigned)
        bool bypass = false; // per-slot host-side bypass (slot is skipped)
    };
    std::vector<std::unique_ptr<PluginSlot>> slots;    // ordered chain ch0 (control thread only)
    std::vector<std::unique_ptr<PluginSlot>> retiring; // removed slots held until chain.settled()
    std::size_t selected = 0;                          // 0-based selected slot (editor/bypass/remove target)
    audient::vst3::Vst3Chain micChain;
    micChain.configure(kMaxBlock);
    std::atomic<bool> chainBypass{vst3Bypass};
    std::atomic<bool> mixerDirty{false}; // any chain mutation; autosave debounces it
    audient::daily::MixerState mixerState; // persisted mixer state (control thread)
    std::vector<std::string> missingCh0;   // state ids that failed to load at startup
    std::vector<std::string> missingCh1;
    // --dual Channel 1 independent chain (throttled diagnostics only, never summed).
    std::vector<std::unique_ptr<PluginSlot>> ch2Slots;
    std::vector<std::unique_ptr<PluginSlot>> ch2Retiring;
    audient::vst3::Vst3Chain ch2Chain;
    ch2Chain.configure(kMaxBlock);
    std::atomic<bool> ch2ChainBypass{false};
    // B7 diagnostic focus for independent VST acceptance (no Phase C work).
    std::size_t vstFocusChannel = 0; // 0 = CH1 (micChain), 1 = CH2 (ch2Chain), toggled by 'n' in --dual
    std::size_t ch2Selected = 0;

    const auto publishChain = [&]() {
        mixerDirty.store(true, std::memory_order_relaxed);
        std::vector<audient::vst3::Vst3Chain::Slot> snapshot;
        snapshot.reserve(slots.size());
        for (const auto& s : slots)
        {
            snapshot.push_back({s->processor.get(), s->bypass});
        }
        const bool whole = chainBypass.load(std::memory_order_relaxed);
        if (!micChain.publish(std::move(snapshot), whole))
        {
            std::printf("WARN: chain publish rejected (max %zu slots)\n", audient::vst3::Vst3Chain::kMaxSlots);
            return;
        }
        micChain.reap();
        std::printf("[vst3] chain published: %zu slot(s), %s, total latency %u samples\n", slots.size(),
                    whole ? "whole-chain BYPASS" : "processed",
                    static_cast<unsigned>(micChain.totalLatencySamples()));
    };
    const auto publishCh2Chain = [&]() {
        mixerDirty.store(true, std::memory_order_relaxed);
        std::vector<audient::vst3::Vst3Chain::Slot> snapshot;
        snapshot.reserve(ch2Slots.size());
        for (const auto& s : ch2Slots)
        {
            snapshot.push_back({s->processor.get(), s->bypass});
        }
        const bool whole = ch2ChainBypass.load(std::memory_order_relaxed);
        if (!ch2Chain.publish(std::move(snapshot), whole))
        {
            std::printf("WARN: ch2 chain publish rejected (max %zu slots)\n", audient::vst3::Vst3Chain::kMaxSlots);
            return;
        }
        ch2Chain.reap();
        std::printf("[vst3-ch2] chain published: %zu slot(s), %s, total latency %u samples (focus %s)\n",
                    ch2Slots.size(), whole ? "whole-chain BYPASS" : "processed",
                    static_cast<unsigned>(ch2Chain.totalLatencySamples()),
                    vstFocusChannel == 1 ? "CH2" : "CH1");
    };
    const auto printChain = [&]() {
        for (std::size_t i = 0; i < slots.size(); ++i)
        {
            const auto& s = slots[i];
            std::printf("[vst3]   [%zu] %s %s (latency %u samples)\n", i + 1, s->name.c_str(),
                        s->bypass ? "BYPASSED" : "ACTIVE", static_cast<unsigned>(s->processor->latencySamples()));
        }
        if (dualMode)
        {
            for (std::size_t i = 0; i < ch2Slots.size(); ++i)
            {
                const auto& s = ch2Slots[i];
                std::printf("[vst3-ch2] [%zu] %s %s (latency %u samples)\n", i + 1, s->name.c_str(),
                            s->bypass ? "BYPASSED" : "ACTIVE", static_cast<unsigned>(s->processor->latencySamples()));
            }
            if (ch2Slots.empty())
            {
                std::printf("[vst3-ch2] (channel 2 wire - no plug-in)\n");
            }
        }
    };
    const auto printChainOrder = [&]() {
        std::string order;
        for (const auto& s : slots)
        {
            if (!order.empty())
            {
                order += " -> ";
            }
            order += s->name;
        }
        std::printf("[ui] chain order now: %s\n", order.empty() ? "(empty)" : order.c_str());
        if (dualMode)
        {
            std::string order2;
            for (const auto& s : ch2Slots)
            {
                if (!order2.empty())
                {
                    order2 += " -> ";
                }
                order2 += s->name;
            }
            std::printf("[ui] ch2 order now: %s (focus %s)\n", order2.empty() ? "(wire)" : order2.c_str(),
                        vstFocusChannel == 1 ? "CH2" : "CH1");
        }
    };

    // Loads one .vst3, classifies it, and prepares a MONO processor for the mic
    // chain. Returns an unappended slot; the caller appends it to `slots` and
    // republishes. restoreFirst applies --vst3-restore to this instance (keeps
    // the old single-plugin startup semantics); wantEnum prints the parameter
    // table for this instance.
    const auto createPluginSlot = [&](const std::string& path, bool restoreFirst, bool wantEnum)
        -> std::unique_ptr<PluginSlot> {
        auto slot = std::make_unique<PluginSlot>();
        slot->path = path;
        slot->loader = std::make_unique<audient::vst3::Vst3ModuleLoader>();
        std::printf("[vst3] loading %s ...\n", path.c_str());
        if (!slot->loader->load(path))
        {
            std::printf("WARN: plugin load failed: %s (slot not added)\n", slot->loader->lastError().c_str());
            return {};
        }
        slot->host = std::make_unique<audient::vst3::Vst3Host>();
        if (!slot->host->attachFactory(slot->loader->factory()))
        {
            std::printf("WARN: factory attach failed (slot not added)\n");
            return {};
        }
        const std::vector<audient::vst3::PluginClassInfo> classes = slot->host->classes();
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
            std::printf("WARN: no Audio Effect class in module (slot not added)\n");
            return {};
        }
        slot->processor = slot->host->createEffectProcessor(*effect, 48000.0,
                                                            static_cast<long>(kMaxBlock),
                                                            audient::vst3::BusLayout::Mono);
        if (!slot->processor || !slot->processor->valid())
        {
            std::printf("WARN: plugin prepare failed: %s (slot not added)\n", slot->host->lastError().c_str());
            for (const audient::vst3::PluginClassInfo& cls : slot->host->classes())
            {
                std::printf("[vst3]   class \"%s\" category=\"%s\" isEffect=%d\n", cls.name.c_str(),
                            cls.category.c_str(), cls.isEffect() ? 1 : 0);
            }
            return {};
        }
        slot->name = effect->name;
        if (restoreFirst && !vst3RestoreFile.empty())
        {
            std::ifstream in(vst3RestoreFile, std::ios::binary);
            const std::vector<std::uint8_t> blob((std::istreambuf_iterator<char>(in)),
                                                 std::istreambuf_iterator<char>());
            if (!blob.empty() && slot->processor->restoreState(blob))
            {
                std::printf("[vst3] slot[1] restored state from %s (%zu bytes)\n", vst3RestoreFile.c_str(),
                            blob.size());
            }
            else
            {
                std::printf("WARN: --vst3-restore %s rejected (continuing with factory state)\n",
                            vst3RestoreFile.c_str());
            }
        }
        if (wantEnum)
        {
            audient::vst3::Vst3Controller controller;
            if (controller.open(slot->processor->component(), slot->loader->factory(), slot->host->hostContext()))
            {
                Steinberg::Vst::IEditController* edit = controller.controller();
                const Steinberg::int32 count = edit->getParameterCount();
                std::printf("[vst3] \"%s\" parameter count: %ld\n", slot->name.c_str(), static_cast<long>(count));
                for (Steinberg::int32 index = 0; index < count; ++index)
                {
                    Steinberg::Vst::ParameterInfo info{};
                    if (edit->getParameterInfo(index, info) != Steinberg::kResultOk)
                    {
                        continue;
                    }
                    std::printf("[vst3]   param[%ld] id=%06X \"%s\" -> \"%s\" units=\"%s\" step=%ld "
                                "default=%.4f flags=0x%x%s%s\n",
                                static_cast<long>(index), static_cast<unsigned>(info.id),
                                narrowUtf16(info.title).c_str(), narrowUtf16(info.shortTitle).c_str(),
                                narrowUtf16(info.units).c_str(), static_cast<long>(info.stepCount),
                                static_cast<double>(info.defaultNormalizedValue),
                                static_cast<unsigned>(info.flags),
                                (info.flags & Steinberg::Vst::ParameterInfo::kIsBypass) != 0 ? " BYPASS" : "",
                                (info.flags & Steinberg::Vst::ParameterInfo::kIsReadOnly) != 0 ? " RO" : "");
                }
                controller.close();
            }
            else
            {
                std::printf("WARN: controller open failed (--vst3-enum)\n");
            }
        }
        return slot;
    };

    // Initial chain: explicit --vst3 (diagnostic) or the persisted mixer state.
    // The startup file picker is diagnostic-only (--pick-vst); the daily GUI
    // never opens a dialog at startup and starts with zero inserts when there is
    // no saved state (the only normal picker path is the UI "+ ADD INSERT").
    if (vst3Paths.empty() && tui.active() && pickVstAtStart)
    {
        std::printf("[vst3] no --vst3 given; select the plug-in in the dialog...\n");
        const std::string picked = pickVst3File(vst3Dir);
        if (!picked.empty())
        {
            std::printf("[vst3] picked: %s\n", picked.c_str());
            vst3Paths.push_back(picked);
        }
        else
        {
            std::printf("[vst3] no plug-in selected; running without a mic-chain effect\n");
        }
    }
    // State-driven mode (no explicit --vst3) restores the persisted mixer; the
    // diagnostic explicit-chain mode is unchanged. `mixerPersist` also gates
    // autosave so diagnostic CLI runs never overwrite the daily mixer state.
    const bool mixerPersist = vst3Paths.empty() && vst3Ch2Paths.empty();
    if (mixerPersist && !noVstRestore)
    {
        std::string loadError;
        audient::daily::MixerState loaded;
        if (audient::daily::loadMixerState(audient::daily::mixerStatePath(), loaded, loadError))
        {
            mixerState = loaded;
            std::printf("[mixer] restored %s: ch0=%zu ch1=%zu inserts\n",
                        audient::daily::mixerStatePath().c_str(), mixerState.ch0.inserts.size(),
                        mixerState.ch1.inserts.size());
        }
        else
        {
            std::printf("[mixer] no saved mixer state (%s): starting with empty inserts\n",
                        loadError.c_str());
        }
        for (const audient::daily::MixerInsert& ins : mixerState.ch0.inserts)
        {
            auto slot = createPluginSlot(ins.path, false, false);
            if (slot == nullptr)
            {
                std::printf("[mixer] ch0 insert \"%s\" unavailable (path missing or load/prepare failed): %s (skipped; metadata kept)\n",
                            ins.name.c_str(), ins.path.c_str());
                missingCh0.push_back(ins.id);
                continue;
            }
            slot->stateId = ins.id;
            slot->bypass = ins.bypass;
            if (!ins.stateFile.empty())
            {
                std::vector<unsigned char> blob;
                if (audient::daily::readBinaryFile(audient::daily::stateBlobPath(ins.stateFile), blob) &&
                    !blob.empty())
                {
                    if (slot->processor->restoreState(blob))
                    {
                        std::printf("[mixer] ch0 \"%s\" VST3 state restored (%zu bytes)\n",
                                    ins.name.c_str(), blob.size());
                    }
                    else
                    {
                        std::printf("[mixer] ch0 \"%s\" saved state rejected (factory state kept)\n",
                                    ins.name.c_str());
                    }
                }
            }
            slots.push_back(std::move(slot));
        }
        chainBypass.store(mixerState.ch0.wholeBypass, std::memory_order_relaxed);
        publishChain();
        printChain();
        if (dualMode)
        {
            for (const audient::daily::MixerInsert& ins : mixerState.ch1.inserts)
            {
                auto slot = createPluginSlot(ins.path, false, false);
                if (slot == nullptr)
                {
                    std::printf("[mixer] ch1 insert \"%s\" unavailable (path missing or load/prepare failed): %s (skipped; metadata kept)\n",
                                ins.name.c_str(), ins.path.c_str());
                    missingCh1.push_back(ins.id);
                    continue;
                }
                slot->stateId = ins.id;
                slot->bypass = ins.bypass;
                if (!ins.stateFile.empty())
                {
                    std::vector<unsigned char> blob;
                    if (audient::daily::readBinaryFile(audient::daily::stateBlobPath(ins.stateFile), blob) &&
                        !blob.empty())
                    {
                        if (slot->processor->restoreState(blob))
                        {
                            std::printf("[mixer] ch1 \"%s\" VST3 state restored (%zu bytes)\n",
                                        ins.name.c_str(), blob.size());
                        }
                    }
                }
                ch2Slots.push_back(std::move(slot));
            }
            ch2ChainBypass.store(mixerState.ch1.wholeBypass, std::memory_order_relaxed);
            publishCh2Chain();
        }
    }
    else
    {
        for (std::size_t i = 0; i < vst3Paths.size(); ++i)
        {
            auto slot = createPluginSlot(vst3Paths[i], /*restoreFirst=*/i == 0, /*wantEnum=*/vst3Enum);
            if (slot == nullptr)
            {
                continue;
            }
            std::printf("[vst3] mic-chain slot[%zu] \"%s\" active (latency %u samples, layout=%s)%s\n", i + 1,
                        slot->name.c_str(), static_cast<unsigned>(slot->processor->latencySamples()),
                        slot->processor->layout() == audient::vst3::BusLayout::Stereo ? "stereo" : "mono",
                        vst3Bypass ? " [whole-chain bypass at start]" : "");
            slots.push_back(std::move(slot));
        }
        publishChain();
        printChain();
        if (dualMode)
        {
            if (!vst3Ch2Paths.empty())
            {
                for (std::size_t i = 0; i < vst3Ch2Paths.size(); ++i)
                {
                    auto slot = createPluginSlot(vst3Ch2Paths[i], /*restoreFirst=*/false, /*wantEnum=*/false);
                    if (slot == nullptr)
                    {
                        continue;
                    }
                    std::printf("[vst3-ch2] ch1 slot[%zu] \"%s\" active (latency %u samples, layout=%s)\n",
                                i + 1, slot->name.c_str(),
                                static_cast<unsigned>(slot->processor->latencySamples()),
                                slot->processor->layout() == audient::vst3::BusLayout::Stereo ? "stereo" : "mono");
                    ch2Slots.push_back(std::move(slot));
                }
                publishCh2Chain();
            }
            else
            {
                std::printf("[dual] ch1 chain empty (no --vst3-ch2); running wire\n");
            }
        }
        else if (!vst3Ch2Paths.empty())
        {
            std::printf("WARN: --vst3-ch2 requires --dual (ignored)\n");
        }
    }

    // Autosave: rebuild the persisted list from the live chains (preserving
    // missing entries), capture each present plug-in's VST3 state blob, then
    // write mixer-state.json atomically. Control/background thread only.
    const auto saveMixerState = [&]() {
        if (!mixerPersist)
        {
            return;
        }
        for (auto& s : slots)
        {
            if (s->stateId.empty())
            {
                s->stateId = audient::daily::newInsertId();
            }
        }
        for (auto& s : ch2Slots)
        {
            if (s->stateId.empty())
            {
                s->stateId = audient::daily::newInsertId();
            }
        }
        const auto rebuild = [](audient::daily::MixerChannel& ch,
                                std::vector<std::unique_ptr<PluginSlot>>& live,
                                const std::vector<std::string>& missing) {
            std::vector<audient::daily::MixerInsert> out;
            std::vector<bool> slotUsed(live.size(), false);
            for (audient::daily::MixerInsert ins : ch.inserts)
            {
                bool present = false;
                for (std::size_t i = 0; i < live.size(); ++i)
                {
                    if (!slotUsed[i] && live[i]->stateId == ins.id)
                    {
                        ins.path = live[i]->path;
                        ins.name = live[i]->name;
                        ins.bypass = live[i]->bypass;
                        ins.missing = false;
                        if (ins.stateFile.empty())
                        {
                            ins.stateFile = ins.id + ".vststate";
                        }
                        slotUsed[i] = true;
                        present = true;
                        break;
                    }
                }
                if (present)
                {
                    out.push_back(std::move(ins));
                }
                else
                {
                    bool wasMissing = false;
                    for (const std::string& m : missing)
                    {
                        if (m == ins.id)
                        {
                            wasMissing = true;
                            break;
                        }
                    }
                    if (wasMissing)
                    {
                        ins.missing = true;
                        out.push_back(std::move(ins)); // preserved for recovery
                    }
                    // else: removed by the user -> dropped
                }
            }
            for (std::size_t i = 0; i < live.size(); ++i)
            {
                if (slotUsed[i])
                {
                    continue;
                }
                audient::daily::MixerInsert ins;
                ins.id = live[i]->stateId;
                ins.path = live[i]->path;
                ins.name = live[i]->name;
                ins.bypass = live[i]->bypass;
                ins.stateFile = ins.id + ".vststate";
                out.push_back(std::move(ins));
            }
            ch.inserts = std::move(out);
        };
        audient::daily::MixerChannel ch0;
        ch0.inserts = mixerState.ch0.inserts;
        ch0.wholeBypass = chainBypass.load(std::memory_order_relaxed);
        rebuild(ch0, slots, missingCh0);
        mixerState.ch0 = std::move(ch0);
        for (const audient::daily::MixerInsert& ins : mixerState.ch0.inserts)
        {
            if (ins.missing)
            {
                continue;
            }
            for (auto& s : slots)
            {
                if (s->stateId == ins.id && s->processor)
                {
                    std::vector<std::uint8_t> blob;
                    std::string err;
                    if (s->processor->saveState(blob))
                    {
                        (void)audient::daily::writeBinaryFileAtomic(
                            audient::daily::stateBlobPath(ins.stateFile), blob, err);
                    }
                    else
                    {
                        std::printf("WARN: VST3 state save failed for \"%s\"\n", s->name.c_str());
                    }
                }
            }
        }
        if (dualMode)
        {
            audient::daily::MixerChannel ch1;
            ch1.inserts = mixerState.ch1.inserts;
            ch1.wholeBypass = ch2ChainBypass.load(std::memory_order_relaxed);
            rebuild(ch1, ch2Slots, missingCh1);
            mixerState.ch1 = std::move(ch1);
            for (const audient::daily::MixerInsert& ins : mixerState.ch1.inserts)
            {
                if (ins.missing)
                {
                    continue;
                }
                for (auto& s : ch2Slots)
                {
                    if (s->stateId == ins.id && s->processor)
                    {
                        std::vector<std::uint8_t> blob;
                        std::string err;
                        if (s->processor->saveState(blob))
                        {
                            (void)audient::daily::writeBinaryFileAtomic(
                                audient::daily::stateBlobPath(ins.stateFile), blob, err);
                        }
                    }
                }
            }
        }
        std::string err;
        if (!audient::daily::saveMixerStateAtomic(audient::daily::mixerStatePath(), mixerState, err))
        {
            std::printf("WARN: mixer-state save failed: %s\n", err.c_str());
        }
        mixerDirty.store(false, std::memory_order_relaxed);
    };
    std::chrono::steady_clock::time_point lastMixerSave = std::chrono::steady_clock::now();

    // --- 3. Capture transport (production path) ---------------------------------
    const audient::transport::Format format{48000, 1, 64, audient::transport::SampleType::Float32};
    audient::transport::VirtualCaptureTransport captureTransport(format, 8192);
    std::printf("[transport] mono float32 48 kHz, block 64, capacity 8192\n");

    // Diagnostic WAV recorder (A/B/C taps). The taps are (re)armed per ASIO
    // session inside the wiring hook below; the recorder object SURVIVES device
    // recovery so captures continue seamlessly across reconnects.
    std::unique_ptr<WavRecorder> recorder;
    if (!recordPrefix.empty())
    {
        recorder = std::make_unique<WavRecorder>();
        if (!recorder->start(recordPrefix, static_cast<int>(recordSeconds)))
        {
            std::printf("WARN: could not start WAV recorder\n");
            recorder.reset();
        }
        else
        {
            std::printf("[record] WAV recorder armed (taps re-armed per ASIO session)\n");
        }
    }

    // --- 4. Endpoint + feeder (the transport -> driver boundary) ---------------
    audient::virtual_audio::SoftwareCaptureEndpoint endpoint(captureTransport);
    audient::virtual_audio::VirtualMicFeeder feeder(endpoint, kCaptureBlockFrames);
    if (picoTransport)
    {
        // Pico sessions must survive buffer changes up to 1024. The engine
        // publishes a full ASIO callback block (up to 1024 frames) into the
        // capture transport at once; the accepted 512-frame threshold would
        // misclassify that legitimate in-flight block as stale and drop it
        // (silent virtual mic at 1024). 2048 frames == 42.7 ms bounded latency.
        feeder.setStaleThresholdFrames(2048);
    }
    else
    {
        // Accepted (frozen) default behavior when the Pico transport is off.
        feeder.setStaleThresholdFrames(512); // live mic: >10 ms backlog is stale
    }

    // --- 5. Driver control plane (optional) ------------------------------------
    audient::virtual_audio::CaptureControlClient client;
    const bool driverControlAvailable = client.open();
    std::uint64_t connectedGeneration = 0;
    bool appOnlyMode = false;

    const std::uint64_t mappedBytes = audient::capture_control::CaptureControlMappedBytes();
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        static_cast<DWORD>(mappedBytes), nullptr);
    if (section == nullptr)
    {
        std::printf("ERROR: CreateFileMapping failed (err=%lu)\n", static_cast<unsigned long>(GetLastError()));
        timeEndPeriod(1);
        tui.end();
        return 1;
    }
    void* view = MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(mappedBytes));
    if (view == nullptr)
    {
        std::printf("ERROR: MapViewOfFile failed (err=%lu)\n", static_cast<unsigned long>(GetLastError()));
        CloseHandle(section);
        timeEndPeriod(1);
        tui.end();
        return 1;
    }

    if (driverControlAvailable)
    {
        audient::capture_control::CaptureControlCaps ctrlCaps;
        if (!client.queryCaps(&ctrlCaps))
        {
            std::printf("WARN: control-plane QUERY_CAPS failed\n");
        }
        connectedGeneration = client.connect(view, mappedBytes);
        if (connectedGeneration == 0)
        {
            std::printf("WARN: driver CONNECT failed (busy or not ready) - running app-only\n");
        }
        else
        {
            std::printf("[driver] CONNECTED generation=%llu (control plane ready)\n",
                        static_cast<unsigned long long>(connectedGeneration));
        }
    }
    if (connectedGeneration == 0)
    {
        appOnlyMode = true;
        if (requireDriver)
        {
            std::printf("ERROR: --require-driver but the virtual-mic driver control plane is unavailable.\n");
            UnmapViewOfFile(view);
            CloseHandle(section);
            timeEndPeriod(1);
            tui.end();
            return 3;
        }
        std::printf("[driver] NOT available: virtual-mic driver not installed/not started -> "
                    "APP-ONLY mode (local ring; Windows endpoint will serve silence)\n");
        if (!audient::capture_ring::CaptureRingInit(
                static_cast<audient::capture_ring::CaptureRingHeader*>(view),
                audient::capture_control::CaptureControlRegionBytes(),
                audient::capture_control::CONTROL_CAPACITY_FRAMES,
                audient::capture_control::CONTROL_BLOCK_FRAMES))
        {
            std::printf("ERROR: local ring init failed\n");
            UnmapViewOfFile(view);
            CloseHandle(section);
            timeEndPeriod(1);
            tui.end();
            return 1;
        }
    }

    audient::virtual_audio::SharedRingCaptureSink sink(
        format, view, audient::capture_control::CaptureControlRegionBytes());
    audient::virtual_audio::IDriverCaptureSink* feederSink = &sink;
    std::unique_ptr<TapCaptureSink> tapSink;
    if (recorder != nullptr || probeLatency)
    {
        tapSink = std::make_unique<TapCaptureSink>(&sink, recorder != nullptr ? recorder->c() : nullptr);
        feederSink = tapSink.get();
    }

    // --- 4b. Pico v0.1.1 virtual-mic transport (fan-out, optional) --------------
    // The accepted driver sink and the Pico transport stay fully INDEPENDENT: the
    // fan-out forwards every processed-mic block to both, a block counts as
    // accepted when ANY sink accepts it, and Pico presence never controls the
    // driver path or the ASIO engine. PicoUsbWorker runs on its own non-RT
    // worker thread and owns the WASAPI/device lifetime.
    audient::virtual_audio::FanoutCaptureSink fanoutSink;
    audient::pico::PicoVirtualMicSink picoSink(format, 8192);
    std::unique_ptr<audient::pico::PicoUsbWorker> picoWorker;
    audient::virtual_audio::IDriverCaptureSink* attachedSink = feederSink;
    if (picoTransport)
    {
        fanoutSink.add(feederSink);
        fanoutSink.add(&picoSink);
        attachedSink = &fanoutSink;
        audient::pico::PicoWorkerConfig picoConfig;
        picoWorker = std::make_unique<audient::pico::PicoUsbWorker>(
            picoSink, std::make_unique<audient::pico::WasapiPicoRenderDevice>(), picoConfig);
        picoWorker->start();
        std::printf("[pico] transport enabled (VoxBridge v0.1.1 UAC2; WASAPI render to the bridge playback endpoint)\n");
    }

    // A4: align the feeder epoch with the driver CONNECT generation (0 in
    // app-only mode => the feeder bumps its own epoch). The feeder/sink and the
    // virtual-mic driver are INDEPENDENT of the physical iD14, so the epoch is
    // preserved across device-loss/recovery (generation stays sane/monotonic).
    // Without --pico this attaches the accepted feederSink directly (unchanged).
    feeder.attachSink(attachedSink, connectedGeneration);
    if (connectedGeneration != 0)
    {
        std::printf("[feeder] epoch aligned to driver CONNECT generation %llu\n",
                    static_cast<unsigned long long>(connectedGeneration));
    }

    audient::engine::SyntheticDownlink syntheticDownlink;
    if (testDownlink)
    {
        syntheticDownlink.configure(440.0, 1105.0, -14.0, 48000);
        std::printf("[downlink] synthetic stereo 440/1105 Hz @ -14 dBFS on Output 1/2 (diagnostic --test-downlink; k mono collapses to center)\n");
    }

    // --- 6. Re-creatable ASIO session wiring + FIRST session --------------------
    // Each fresh AsioRoutingAdapter is wired to the persistent runtime here
    // (chains, capture transport, render config, recorder taps). Runs on the
    // CONTROL thread; never in the audio callback.
    MicInputTapCtx micInputTap;
    micInputTap.recA = recorder != nullptr ? recorder->a() : nullptr;

    // Applies the current output-control state (Monitor/Headphone software
    // buses, local mic-monitor routing) to a RenderConfig. Used both when
    // (re)wiring a fresh adapter and for live control edits, so the SAME state
    // is ramped back into place after device-loss recovery.
    const auto applyOutputState = [&](audient::asio::AsioRoutingAdapter::RenderConfig& cfg) {
        cfg.monitorGain = dBToLinear(-6.0f);
        cfg.monitorMute = monitorMuted;
        cfg.downlinkGain = 1.0f;
        cfg.downlinkMute = testDownlink ? false : true;
        cfg.physicalOutputGain = dBToLinear(monitorLevelDb);
        cfg.physicalOutputMute = false;
        cfg.monitorDim = false;
        cfg.monitorDimGain = 1.0f;
        cfg.headphoneGain = 1.0f;
        cfg.headphoneMute = true;
        cfg.micToHeadphones = false;
        cfg.downlinkToHeadphones = false;
        cfg.outputMono = outputMono;
    };
    auto publishChannelSends = [&](audient::asio::AsioRoutingAdapter& adapter) {
        auto sendFor = [](bool enabled, float lvlDb, bool muted, std::uint64_t rev) {
            audient::channel::ChannelSendState s;
            s.enabled = enabled;
            s.levelDb = lvlDb;
            s.muted = muted;
            s.revision = rev;
            return s;
        };
        if (dualMode)
        {
            const audient::channel::VirtualMicSends sends = audient::channel::virtualMicSendsFor(
                static_cast<audient::channel::VirtualMicSource>(prefs.virtualMicSource), dualMode);
            audient::channel::ChannelRuntimeSnapshot s0;
            s0.virtualMicSend = sends.channel0;
            s0.localMonitorSend = sendFor(localMonCh0Enabled, localMonCh0LevelDb, localMonCh0Muted, 0);
            s0.revision = localMonCh0Rev;
            adapter.publishChannelRuntime(0, s0);
            audient::channel::ChannelRuntimeSnapshot s1;
            s1.virtualMicSend = sends.channel1;
            s1.localMonitorSend = sendFor(localMonCh1Enabled, localMonCh1LevelDb, localMonCh1Muted, 0);
            s1.revision = localMonCh1Rev;
            adapter.publishChannelRuntime(1, s1);
        }
    };

    auto wireAdapter = [&](audient::asio::AsioRoutingAdapter& a) {
        a.setChannelInputChain(0, &audient::vst3::Vst3Chain::processMono, &micChain);
        a.setChannelInputChainLatency(0, &audient::vst3::Vst3Chain::chainLatencyQuery, &micChain);
        if (dualMode)
        {
            a.setChannelInputChain(1, &audient::vst3::Vst3Chain::processMono, &ch2Chain);
            a.setChannelInputChainLatency(1, &audient::vst3::Vst3Chain::chainLatencyQuery, &ch2Chain);
        }
        a.setCaptureTransport(&captureTransport);
        if (testDownlink)
        {
            a.enableSyntheticDownlink(&syntheticDownlink);
        }

        publishChannelSends(a);
        audient::asio::AsioRoutingAdapter::RenderConfig cfg;
        applyOutputState(cfg);
        cfg.panicMute = false;
        cfg.revision = 1;
        a.publishRenderConfig(cfg);

        // MIC RAW meter (always) + recorder-A tap (when recording) share one
        // realtime-clean adapter mic-input tap. Re-armed per session.
        a.setMicInputTap(&micInputTapHook, &micInputTap);
        if (recorder != nullptr)
        {
            a.setMicUplinkTap([](const float* mono, std::size_t frames, void* ctx) {
                static_cast<WavRecorder*>(ctx)->b()->push(mono, frames);
            }, recorder.get());
        }
    };

    // --- 6b. Persistent capture client BEFORE the ASIO engine starts (optional) --
    // A persistent shared-mode capture client (like Discord) keeps the engine's
    // endpoint stream running. It must start BEFORE the engine so the DMA drain
    // is live from the first ring write (the A4 client-first latency fix). It
    // survives device-loss/recovery in ONE stream: exact silence while the iD14
    // is gone, then the resumed audio.
    std::unique_ptr<WasapiLatencyCapture> holdClient;
    if (probeLatency)
    {
        holdClient = std::make_unique<WasapiLatencyCapture>();
        if (!holdClient->open())
        {
            std::printf("probe: cannot open the virtual-mic capture endpoint (skipping probe)\n");
            holdClient.reset();
        }
        else
        {
            holdClient->start();
            std::printf("probe: persistent capture client draining the endpoint\n");
        }
    }

    DemoAsioStream asioStream(provider, match, bufferArg, channelArg, dualMode, std::move(wireAdapter));
    std::string startError;
    if (!asioStream.openAndStart(startError))
    {
        std::printf("ERROR: ASIO stream open failed: %s\n", startError.c_str());
        holdClient.reset();
        feeder.stop();
        UnmapViewOfFile(view);
        CloseHandle(section);
        timeEndPeriod(1);
        tui.end();
        return 1;
    }
    feeder.start(std::chrono::microseconds(250));

    std::printf("STREAM STARTED (48 kHz / %ld samples). Running %s...\n",
                asioStream.buffer(),
                secondsArg > 0 ? ("for " + std::to_string(secondsArg) + " s").c_str() : "until Ctrl+C");
    std::printf("  driver state: %s\n", appOnlyMode ? "NOT connected (app-only)" : "connected");
    std::printf("  feeder: poll 250us, timer period 1ms (realtime pacing fix)\n");
    SetConsoleCtrlHandler(consoleBreakHandler, TRUE);

    // --- 6d. Safe output-pair identification tone (diagnostic only). --------
    // Low-level (-30 dBFS) 1.5 kHz tone with a 100 ms on/off cadence on ONE
    // output pair so the user can name which physical speakers/headphones each
    // ASIO pair drives. Never full scale. Run for --seconds and identify.
    if (identifyOutputPair >= 0)
    {
        const bool secondPairRequested = identifyOutputPair != 0;
        if (secondPairRequested && !asioStream.hasSecondaryOutputPair())
        {
            std::printf("\n[identify] ASIO Output 3/4 is NOT created in v1 (Local Monitor uses "
                        "Output 1/2 only). Set AUDIENT_ASIO_FORCE4OUT=1 to run the 3/4 diagnostic.\n");
            identifyOutputArmed = false;
        }
        else
        {
            const bool pairIsMonitor = identifyOutputPair == 0;
            std::printf("\n[identify] arming LOW-level (-30 dBFS) 1.5 kHz pair-identification tone on "
                        "ASIO Output %s (%s).\n",
                        pairIsMonitor ? "1/2" : "3/4 (diagnostic)",
                        pairIsMonitor ? "the v1 Local Monitor pair" : "a forced secondary pair");
            std::printf("[identify] Listen: WHICH physical device (speakers vs headphones) plays the beeping tone?\n");
            asioStream.armOutputTone(static_cast<std::uint32_t>(pairIsMonitor ? 0 : 1), 0.0316f);
            identifyOutputArmed = true;
        }
    }

    // --- 6c. VST3 diagnostic: apply --vst3-param edits on a control thread after
    // the latency probe so the probe measures the prepared path, then capture
    // --vst3-save state. Never touches the audio callback (bounded param queue).
    std::thread paramThread;
    if (!vst3Params.empty())
    {
        paramThread = std::thread([&]() {
            std::this_thread::sleep_for(kParamApplyDelayMs);
            for (const auto& [id, value] : vst3Params)
            {
                const bool accepted = !slots.empty() && slots[0]->processor->enqueueParameter(id, value);
                std::printf("[vst3] enqueue param id=%u value=%.4f -> slot[1] %s\n", static_cast<unsigned>(id), value,
                            accepted ? "OK" : "FULL/DROP");
            }
            if (!slots.empty() && !vst3SaveFile.empty())
            {
                std::this_thread::sleep_for(kParamSaveGraceMs);
                std::vector<std::uint8_t> blob;
                if (slots[0]->processor->saveState(blob))
                {
                    std::ofstream out(vst3SaveFile, std::ios::binary);
                    out.write(reinterpret_cast<const char*>(blob.data()),
                              static_cast<std::streamsize>(blob.size()));
                    std::printf("[vst3] saved state to %s (%zu bytes)\n", vst3SaveFile.c_str(), blob.size());
                }
                else
                {
                    std::printf("WARN: --vst3-save %s failed (state rejected by the plug-in)\n",
                                vst3SaveFile.c_str());
                }
            }
        });
    }

    // --- 7. Startup latency probe (first session only; watchdog not started yet) --
    if (probeLatency && holdClient)
    {
        audient::asio::AsioRoutingAdapter* probe = asioStream.probeAdapter();
        if (probe != nullptr)
        {
            runLatencyProbe(*probe, asioStream.buffer(), captureTransport, *tapSink, sink, *holdClient);
        }
    }

    // --- 9. Device-loss watchdog + automatic recovery (control thread) ------------
    audient::asio::AsioRecoveryController recovery(asioStream, {kWatchdogStallMs, kRecoveryRetryMs});
    std::atomic<bool> simulatedLossTriggered{false};
    // Set by the GUI when the user picks a new ASIO buffer size; handled on the
    // watchdog control thread via the proven graceful-teardown + recovery rebuild.
    std::atomic<bool> audioReconfigureRequested{false};
    bool rateMismatchActive = false; // watchdog-thread only
    std::chrono::steady_clock::time_point nextBufferPoll = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point simulateAfter{};
    if (simulateLossAfterMs > 0)
    {
        simulateAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(simulateLossAfterMs);
    }
    std::thread watchdogThread = std::thread([&, simulateAfter]() {
        recovery.markStreaming(std::chrono::steady_clock::now());
        while (!g_stop.load(std::memory_order_relaxed) && !recovery.stopRequested())
        {
            const auto now = std::chrono::steady_clock::now();
            if (audioReconfigureRequested.exchange(false))
            {
                std::printf("[audio] buffer change accepted -> planned rebuild "
                            "(target buffer=%ld)\n",
                            asioStream.requestedBuffer());
                asioStream.reconfigureTeardown();
                recovery.requestReconfigure(std::chrono::steady_clock::now());
            }
            // Bounded control-thread poll of the live driver (NEVER on the UI
            // thread): refresh the cached actual rate and detect an external
            // buffer-size change. Both are reconciled through a safe rebuild.
            if (now >= nextBufferPoll)
            {
                nextBufferPoll = now + std::chrono::milliseconds(250);
                asioStream.pollActualRate();
                if (!asioStream.rebuilding())
                {
                    // Internal processing + transport are fixed at 48 kHz. A
                    // different actual rate is unsafe -> restore 48 kHz once.
                    const long actual = asioStream.actualSampleRate();
                    const long expected = asioStream.configuredSampleRate();
                    if (actual > 0 && actual != expected)
                    {
                        if (!rateMismatchActive)
                        {
                            rateMismatchActive = true;
                            std::printf("[audio] WARN: driver sample rate %ld Hz != required %ld Hz "
                                        "(external change); 48 kHz is required -> restoring\n",
                                        actual, expected);
                            asioStream.reconfigureTeardown();
                            recovery.requestReconfigure(std::chrono::steady_clock::now());
                        }
                    }
                    else
                    {
                        rateMismatchActive = false;
                    }
                }
                if (!asioStream.rebuilding() && asioStream.currentBuffer() > 0)
                {
                    const long driverBuffer = asioStream.driverBufferSize();
                    const long runningBuffer = asioStream.currentBuffer();
                    if (driverBuffer > 0 && driverBuffer != runningBuffer)
                    {
                        if (asioStream.bufferSupported(driverBuffer))
                        {
                            std::printf("[audio] external buffer change detected: driver=%ld (running=%ld, "
                                        "driver-notified=%u); adopting and reconciling\n",
                                        driverBuffer, runningBuffer, asioStream.bufferChangeNotifyCount());
                            asioStream.stageExternalBuffer(driverBuffer);
                        }
                        else if (driverBuffer > static_cast<long>(kMaxBlock))
                        {
                            std::printf("[audio] WARN: external buffer %ld exceeds the engine maximum %zu; "
                                        "restoring %ld\n",
                                        driverBuffer, kMaxBlock, runningBuffer);
                            asioStream.stageExternalBuffer(runningBuffer);
                        }
                        else
                        {
                            std::printf("[audio] WARN: external buffer %ld is not a driver-advertised "
                                        "candidate; restoring %ld\n",
                                        driverBuffer, runningBuffer);
                            asioStream.stageExternalBuffer(runningBuffer);
                        }
                        asioStream.reconfigureTeardown();
                        recovery.requestReconfigure(std::chrono::steady_clock::now());
                    }
                }
            }
            if (simulateLossAfterMs > 0 && !simulatedLossTriggered.load() &&
                std::chrono::steady_clock::now() >= simulateAfter)
            {
                simulatedLossTriggered.store(true);
                std::printf("[device] SIMULATED device-loss trigger at +%ld ms (diagnostic only)\n",
                            simulateLossAfterMs);
                // The simulated test runs while the physical ASIO engine is STILL
                // live. Gracefully stop it FIRST so the driver callback thread
                // exits before the controller's closeStream() detaches the
                // session (a hard detach under a running callback is a
                // use-after-free; the plugin's ~30 us blocks make that race
                // reachable). Real device loss stops callbacks by itself, so the
                // stall-detection path never needs this.
                asioStream.gracefulTeardown();
                recovery.simulateLoss(std::chrono::steady_clock::now());
            }
            recovery.tick(std::chrono::steady_clock::now());
            const auto ev = recovery.consumeEvents();
            if (ev.lost)
            {
                std::printf("[device] PHYSICAL DEVICE LOST - ASIO callbacks stalled >= %lld ms; "
                            "serving exact silence; waiting for the iD14 "
                            "(reconnect check every %lld ms)\n",
                            static_cast<long long>(kWatchdogStallMs.count()),
                            static_cast<long long>(kRecoveryRetryMs.count()));
            }
            else if (ev.recovered)
            {
                std::printf("[device] RECOVERED - fresh ASIO stream established; resuming Mic 1 "
                            "(buffer=%ld, mic-in=%ld)\n",
                            asioStream.buffer(), asioStream.micInput());
            }
            std::this_thread::sleep_for(kWatchdogTickMs);
        }
    });

    // --- 10. Control loop -------------------------------------------------------
    // Native editor windows are created + pumped on THIS control thread (never
    // the audio callback); closing them never touches the stream. In the
    // interactive console a fixed status pane is repainted in place; under
    // redirected output the legacy one-line-per-second [status] is appended so
    // captured logs keep their exact format.
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    if (secondsArg > 0)
    {
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(secondsArg);
    }

    audient::vst3::Vst3Editor pluginEditor;
    EditorHostState editorHostState{};
    HWND editorHostWindow = nullptr;
    audient::vst3::Vst3Host* editorSinkHost = nullptr;
    std::size_t editorOpenSlot = std::numeric_limits<std::size_t>::max(); // slot index the editor is bound to
    std::size_t editorOpenChannel = std::numeric_limits<std::size_t>::max(); // 0=CH1 1=CH2, max=none

    const auto clearParameterEditSink = [&]() {
        if (editorSinkHost != nullptr)
        {
            editorSinkHost->setParameterEditSink(nullptr);
        }
        editorSinkHost = nullptr;
    };
    const auto closeEditor = [&]() {
        const bool hadUI = pluginEditor.isOpen() || editorHostWindow != nullptr;
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
        editorOpenSlot = std::numeric_limits<std::size_t>::max();
        editorOpenChannel = std::numeric_limits<std::size_t>::max();
        if (hadUI)
        {
            std::printf("[ui] editor closed\n");
        }
    };
    const auto openEditorForSlotOnChannel = [&](std::size_t channel, std::size_t slotIndex) -> bool {
        std::vector<std::unique_ptr<PluginSlot>>* chainSlots = (channel == 1 ? &ch2Slots : &slots);
        if (slotIndex >= chainSlots->size())
        {
            std::printf("[ui] editor unavailable (no plug-in in slot %zu) [%s]\n", slotIndex + 1,
                        channel == 1 ? "CH2" : "CH1");
            return false;
        }
        PluginSlot* target = (*chainSlots)[slotIndex].get();
        if (pluginEditor.isOpen())
        {
            closeEditor(); // one editor at a time; switching target rebinds the view
        }
        if (!ensureEditorHostClass())
        {
            std::printf("[ui] editor host window class registration failed\n");
            return false;
        }
        editorHostState = EditorHostState{};
        editorHostState.editor = &pluginEditor;
        editorHostWindow = CreateWindowExW(0, kEditorHostClassName, L"VST3 Editor", WS_OVERLAPPEDWINDOW,
                                           CW_USEDEFAULT, CW_USEDEFAULT, 860, 660, nullptr, nullptr,
                                           GetModuleHandleW(nullptr), &editorHostState);
        if (editorHostWindow == nullptr)
        {
            std::printf("[ui] editor host window creation failed (%lu)\n", GetLastError());
            return false;
        }
        if (!pluginEditor.open(target->processor->component(), target->loader->factory(), editorHostWindow,
                               target->host->hostContext()))
        {
            std::printf("[ui] editor open failed for slot[%zu]: %s (audio continues)\n", slotIndex + 1,
                        pluginEditor.lastError().c_str());
            DestroyWindow(editorHostWindow);
            editorHostWindow = nullptr;
            return false;
        }
        // VST-009: route controller edits (performEdit) into THIS processor's
        // bounded queue; the realtime callback drains them at the next block.
        // The editor stays bound to this exact plugin instance even if the chain
        // is reordered (the processor object is not moved by publish()).
        if (channel == 1)
        {
            ch2Selected = slotIndex;
        }
        else
        {
            selected = slotIndex;
        }
        target->host->setParameterEditSink(target->processor.get());
        editorSinkHost = target->host.get();
        editorOpenSlot = slotIndex;
        editorOpenChannel = channel;
        vstFocusChannel = channel;
        ShowWindow(editorHostWindow, SW_SHOW);
        UpdateWindow(editorHostWindow);
        std::printf("[ui] editor open for slot[%zu] \"%s\" [%s] (close the window or press e; audio continues)\n",
                    slotIndex + 1, target->name.c_str(), channel == 1 ? "CH2" : "CH1");
        return true;
    };
    const auto openEditorForSlot = [&](std::size_t slotIndex) -> bool {
        return openEditorForSlotOnChannel(0, slotIndex);
    };
    const auto openEditor = [&]() -> bool {
        if (slots.empty())
        {
            std::printf("[ui] editor unavailable (no plug-in loaded)\n");
            return false;
        }
        return openEditorForSlotOnChannel(0, selected);
    };

    // --- Live chain mutations (control thread ONLY; each one republishes an
    // immutable snapshot; the audio thread never sees `slots`) ---------------
    const auto addSlotFromPicker = [&]() {
        if (slots.size() >= audient::vst3::Vst3Chain::kMaxSlots)
        {
            std::printf("[ui] cannot add: chain is full (max %zu slots)\n", audient::vst3::Vst3Chain::kMaxSlots);
            return;
        }
        const std::string path = pickVst3File(vst3Dir);
        if (path.empty())
        {
            std::printf("[ui] add cancelled\n");
            return;
        }
        auto slot = createPluginSlot(path, /*restoreFirst=*/false, /*wantEnum=*/false);
        if (slot == nullptr)
        {
            return;
        }
        std::printf("[vst3] mic-chain slot[%zu] \"%s\" active (latency %u samples, layout=%s)\n", slots.size() + 1,
                    slot->name.c_str(), static_cast<unsigned>(slot->processor->latencySamples()),
                    slot->processor->layout() == audient::vst3::BusLayout::Stereo ? "stereo" : "mono");
        slots.push_back(std::move(slot));
        selected = slots.size() - 1;
        publishChain();
        printChainOrder();
    };
    const auto toggleSlotBypass = [&](std::size_t idx) {
        if (idx >= slots.size())
        {
            return;
        }
        PluginSlot* s = slots[idx].get();
        s->bypass = !s->bypass;
        std::printf("[ui] slot[%zu] \"%s\" %s (skipped in the audio path)\n", idx + 1, s->name.c_str(),
                    s->bypass ? "BYPASSED" : "ACTIVE");
        publishChain();
    };
        const auto removeSlot = [&](std::size_t idx) {
        if (idx >= slots.size())
        {
            std::printf("[ui] remove: no slot %zu\n", idx + 1);
            return;
        }
        if (pluginEditor.isOpen() && editorOpenChannel == 0 && selected == idx)
        {
            closeEditor(); // never destroy a processor an open editor references
        }
        PluginSlot* victim = slots[idx].get();
        std::printf("[ui] removing slot[%zu] \"%s\"...\n", idx + 1, victim->name.c_str());
        retiring.push_back(std::move(slots[idx])); // destroyed after chain grace
        slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(idx));
        if (!slots.empty())
        {
            if (selected > idx)
            {
                --selected;
            }
            else if (selected == idx)
            {
                selected = std::min(selected, slots.size() - 1);
            }
        }
        else
        {
            selected = 0;
        }
        publishChain();
        printChainOrder();
    };
    const auto moveSlot = [&](std::size_t from, std::size_t to) {
        if (from >= slots.size() || to >= slots.size() || from == to)
        {
            return;
        }
        std::unique_ptr<PluginSlot> item = std::move(slots[from]);
        slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(from));
        slots.insert(slots.begin() + static_cast<std::ptrdiff_t>(to), std::move(item));
        if (selected == from)
        {
            selected = to;
        }
        else if (from < to)
        {
            if (selected > from && selected <= to)
            {
                --selected;
            }
        }
        else if (selected >= to && selected < from)
        {
            ++selected;
        }
        publishChain();
        printChainOrder();
    };
    const auto toggleWholeBypass = [&]() {
        const bool next = !chainBypass.load(std::memory_order_relaxed);
        chainBypass.store(next, std::memory_order_relaxed);
        std::printf("[ui] chain bypass %s (raw path)\n", next ? "ON" : "OFF");
        publishChain();
    };
    const auto reapRetired = [&]() {
        micChain.reap();
        ch2Chain.reap();
        if (!ch2Retiring.empty() && ch2Chain.settled())
        {
            std::printf("[dual] ch2 retired slot(s) reclaimed (%zu)\n", ch2Retiring.size());
            ch2Retiring.clear();
        }
        if (!retiring.empty() && micChain.settled())
        {
            std::printf("[ui] retired slot(s) reclaimed (%zu)\n", retiring.size());
            retiring.clear();
        }
    };

    // Slot selection + local-monitor toggle (used by the interactive keys).
    const auto selectSlot = [&](std::size_t index) {
        if (slots.empty())
        {
            std::printf("[ui] no plug-ins loaded (chain is empty)\n");
            return;
        }
        if (index >= slots.size())
        {
            std::printf("[ui] slot[%zu] does not exist (chain has %zu slot(s))\n", index + 1, slots.size());
            return;
        }
        selected = index;
        std::printf("[ui] selected slot[%zu] \"%s\"\n", index + 1, slots[index]->name.c_str());
    };

    // Live publish of the current output-control state to the running session
    // (Monitor/Headphone level, mute, dim, mic routing). Lifetime-safe against
    // the recovery watchdog; a fresh adapter is rewired from the same state.
    // Bumps revision even when payload is unchanged (so a GUI write cannot be
    // lost to a concurrent raw-serial write that coincidentally produced the
    // same dB value).
    const auto publishOutputState = [&]() {
        asioStream.updateRenderConfig(
            [&](audient::asio::AsioRoutingAdapter::RenderConfig& cfg) { applyOutputState(cfg); });
    };
    const auto changeOutputLevel = [&](float deltaDb) {
        float& level = outputSelHeadphones ? headphoneLevelDb : monitorLevelDb;
        level = std::max(-60.0f, std::min(0.0f, level + deltaDb));
        std::printf("[ui] local output: %s software gain %.1f dB%s\n", outputSelHeadphones ? "Mic->Headphones" : "Mic->Speakers", level,
                    ((outputSelHeadphones ? headphoneBusMuted : monitorBusMuted) ? " (muted)" : ""));
        publishOutputState();
    };
    const auto toggleSelectedMute = [&]() {
        if (outputSelHeadphones)
        {
            headphoneBusMuted = !headphoneBusMuted;
        }
        else
        {
            monitorBusMuted = !monitorBusMuted;
        }
        std::printf("[ui] local output: %s %s\n", outputSelHeadphones ? "Mic->Headphones" : "Mic->Speakers",
                    (outputSelHeadphones ? headphoneBusMuted : monitorBusMuted) ? "MUTED" : "ACTIVE");
        publishOutputState();
    };
    const auto toggleDim = [&]() {
        if (outputSelHeadphones)
        {
            std::printf("[ui] dim applies to the Mic->Speakers bus only\n");
            return;
        }
        monitorDim = !monitorDim;
        std::printf("[ui] Mic->Speakers dim %s (%+.0f dB)\n", monitorDim ? "ON" : "OFF", monitorDimDb);
        publishOutputState();
    };
    const auto toggleMicRouteToSelected = [&]() {
        if (outputSelHeadphones)
        {
            micToHeadphones = !micToHeadphones;
            std::printf("[ui] mic monitor -> headphones %s\n", micToHeadphones ? "ON" : "OFF");
        }
        else
        {
            monitorMuted = !monitorMuted;
            std::printf("[ui] local monitor %s\n", monitorMuted ? "OFF" : "ON");
        }
        publishOutputState();
    };

    // Local processed-mic monitor into the MONITOR bus (chain-mode 'm'); the
    // whole output state is republished (independence is enforced by the
    // adapter's separate Monitor/Headphone ramps).
    const auto toggleLocalMonitor = [&]() {
        monitorMuted = !monitorMuted;
        std::printf("[ui] LOCAL MONITOR %s (Mic -> ASIO Output 1/2; iD14 sends it to both "
                    "speakers and headphones)\n", monitorMuted ? "OFF" : "ON");
        publishOutputState();
    };

    // V1 Local Monitor software gain (feed level into ASIO Output 1/2).
    const auto changeLocalMonitorGain = [&](float deltaDb) {
        monitorLevelDb = std::max(-60.0f, std::min(0.0f, monitorLevelDb + deltaDb));
        std::printf("[ui] LOCAL MONITOR software gain %.1f dB (Mic -> Output 1/2)%s\n",
                    monitorLevelDb, monitorMuted ? "  [Local Monitor OFF - press m to hear]" : "");
        publishOutputState();
    };
    const auto toggleOutputMono = [&]() {
        outputMono = !outputMono;
        std::printf("[ui] OUTPUT MONO %s (0.5*(L+R) -> both L/R, before clamp/output ramp)\n",
                    outputMono ? "ON" : "OFF");
        publishOutputState();
    };
    std::size_t tuiFocusChannel = 0;
    const auto publishChannelLocalSend = [&](std::size_t ch) {
        const float lvl = ch == 1 ? localMonCh1LevelDb : localMonCh0LevelDb;
        const bool en = ch == 1 ? localMonCh1Enabled : localMonCh0Enabled;
        const bool muted = ch == 1 ? localMonCh1Muted : localMonCh0Muted;
        std::uint64_t& rev = ch == 1 ? localMonCh1Rev : localMonCh0Rev;
        ++rev;
        asioStream.updateChannelRuntime(ch, [&](audient::channel::ChannelRuntimeSnapshot& s) {
            s.localMonitorSend.enabled = en;
            s.localMonitorSend.muted = muted;
            s.localMonitorSend.levelDb = lvl;
            s.revision = rev;
        });
        std::printf("[localmon] CH%zu %s %.1f dB%s (rev %llu)\n", ch + 1, en && !muted ? "ON" : "OFF", lvl,
                    muted ? " [muted]" : "", static_cast<unsigned long long>(rev));
    };

    // ADR-011: apply a single-source virtual-mic selection live (exactly one
    // channel enabled) and persist it. An Input 2 request on a single-input plan
    // falls back to Input 1.
    const auto applyVirtualMicSource = [&](int source) {
        const int effective = (dualMode && source == 1) ? 1 : 0;
        prefs.virtualMicSource = effective;
        savePrefs();
        const audient::channel::VirtualMicSends sends = audient::channel::virtualMicSendsFor(
            static_cast<audient::channel::VirtualMicSource>(effective), dualMode);
        ++localMonCh0Rev;
        if (dualMode)
        {
            ++localMonCh1Rev;
        }
        asioStream.updateChannelRuntime(0, [&](audient::channel::ChannelRuntimeSnapshot& s) {
            s.virtualMicSend = sends.channel0;
            s.revision = localMonCh0Rev;
        });
        if (dualMode)
        {
            asioStream.updateChannelRuntime(1, [&](audient::channel::ChannelRuntimeSnapshot& s) {
                s.virtualMicSend = sends.channel1;
                s.revision = localMonCh1Rev;
            });
        }
        std::printf("[vmic] source -> %s (CH%d POST-VST -> virtual mic; other channel excluded)\n",
                    audient::preferences::virtualMicSourceName(effective), effective + 1);
    };

    // HARDWARE device volume nudge (iD14 physical output via official API).
    // Independent of the software OUTPUT buses and the local monitor. +- 1.0 dB.
    const auto changeHardwareLevel = [&](double deltaDb) {
        if (!hw.isOpen() || !hw.connected())
        {
            std::printf("[hardware] iD14 not connected; ignoring %+.1f dB nudge\n", deltaDb);
            return;
        }
        const std::optional<double> cur = hwSelHeadphones ? hw.headphoneDb() : hw.monitorDb();
        if (!cur.has_value())
        {
            std::printf("[hardware] no current device level to nudge from\n");
            return;
        }
        const double target = *cur + deltaDb;
        const audient::hardware::Result r =
            hwSelHeadphones ? hw.setHeadphoneDb(target) : hw.setMonitorDb(target);
        const std::string err = hw.lastError();
        std::printf("[hardware] iD14 %s %+.1f dB (%.2f -> %.2f): %s%s\n",
                    hwSelHeadphones ? "Headphones" : "Monitor", deltaDb, *cur, target,
                    audient::hardware::resultName(r),
                    (r == audient::hardware::Result::Ok ? "" : (" - " + err).c_str()));
    };

    // Logical key codes: plain ASCII characters for normal keys plus these
    // sentinels for the extended arrow keys (which _getch reports as 0xE0/0x00
    // followed by a scan code; we normalize them so one dispatch path serves
    // both the real keyboard and the --keys test hook).
    enum KeyCode : int
    {
        kKeyUp = 0x101,
        kKeyDown = 0x102,
        kKeyLeft = 0x103,
        kKeyRight = 0x104,
        kNoKey = 0
    };
    std::atomic<bool> quit{false}; // 'q' is the only normal quit key

    // Single dispatch function used by BOTH the interactive console (_kbhit)
    // and the --keys scripted-input hook, so scripted runs exercise exactly the
    // same control flow as typed keys. Runs on the main control thread only
    // (editor windows must be created/pumped there). 'q' is the only key that
    // ever sets quit; every numeric/arrow branch only moves the selection or
    // edits the chain and can never exit the control loop.
    const auto dispatchKey = [&](int key) {
        // HARDWARE device-volume mode: edits the iD14 PHYSICAL output volume via
        // the official Audient API. Fully separate from OUTPUT (software buses)
        // and the LOCAL processed-mic monitor. 'q' still quits; 'h'/'o' exit.
        if (hwMode)
        {
            switch (key)
            {
            case '1':
                hwSelHeadphones = false;
                std::printf("[hardware] editing iD14 MONITOR (physical device volume)\n");
                break;
            case '2':
                hwSelHeadphones = true;
                std::printf("[hardware] editing iD14 HEADPHONES (physical device volume)\n");
                break;
            case 'n':
            case 'N':
                if (dualMode)
                {
                    vstFocusChannel = (vstFocusChannel == 0 ? 1 : 0);
                    std::printf("[dual] VST focus -> %s (%zu slot(s), ch1 %zu / ch2 %zu)  [n toggles]\n",
                                vstFocusChannel == 0 ? "CH1" : "CH2",
                                vstFocusChannel == 0 ? slots.size() : ch2Slots.size(),
                                slots.size(), ch2Slots.size());
                }
                break;
            case '+':
            case '=':
            case kKeyUp:
                changeHardwareLevel(1.0);
                break;
            case '-':
            case '_':
            case kKeyDown:
                changeHardwareLevel(-1.0);
                break;
            case 'h':
            case 'o':
                hwMode = false;
                std::printf("[hardware] HARDWARE mode off (VST chain keys active)\n");
                break;
            case 'q':
                quit.store(true, std::memory_order_relaxed);
                break;
            default:
                break;
            }
            return;
        }
        // OUTPUT control mode: keys act on the two software output buses only
        // (Monitor/Headphones). Selecting a bus or changing gain/mute/dim/route
        // here NEVER stops the stream and NEVER touches the VST chain or the
        // virtual-mic capture path. 'q' still quits; 'o' returns to chain mode.
        if (outputMode)
        {
            switch (key)
            {
            case 'm':
            case '1':
                outputSelHeadphones = false;
                std::printf("[ui] local output: editing Mic->Speakers (software bus)\n");
                break;
            case 'h':
            case '2':
                outputSelHeadphones = true;
                std::printf("[ui] local output: editing Mic->Headphones (software bus)\n");
                break;
            case '+':
            case '=':
            case kKeyUp:
                changeOutputLevel(1.0f);
                break;
            case '-':
            case '_':
            case kKeyDown:
                changeOutputLevel(-1.0f);
                break;
            case 's':
                toggleSelectedMute();
                break;
            case 'd':
                toggleDim();
                break;
            case 'r':
                toggleMicRouteToSelected();
                break;
            case 'o':
                outputMode = false;
                std::printf("[ui] local output mode off (VST chain keys active)\n");
                break;
            case 'q':
                quit.store(true, std::memory_order_relaxed);
                break;
            default:
                break;
            }
            return;
        }
        if (dualMode && key == 'n')
        {
            vstFocusChannel = (vstFocusChannel == 0 ? 1 : 0);
            if (vstFocusChannel == 1 && ch2Slots.empty())
            {
                std::printf("[dual] VST focus -> CH2 (ch2 wire - no plug-in yet; load with --vst3-ch2 or future add)\n");
            }
            else
            {
                std::printf("[dual] VST focus -> %s (%zu slot(s), ch1 %zu / ch2 %zu)\n",
                            vstFocusChannel == 0 ? "CH1" : "CH2", vstFocusChannel == 0 ? slots.size() : ch2Slots.size(),
                            slots.size(), ch2Slots.size());
            }
            return;
        }
        switch (key)
        {
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            if (dualMode && vstFocusChannel == 1)
            {
                if (key - '1' < static_cast<int>(ch2Slots.size()))
                {
                    ch2Selected = static_cast<std::size_t>(key - '1');
                    std::printf("[dual] CH2 selected slot[%zu] \"%s\"\n", ch2Selected + 1,
                                ch2Slots[ch2Selected]->name.c_str());
                }
                else if (ch2Slots.empty())
                {
                    std::printf("[dual] CH2 has no plug-ins (wire); load with --vst3-ch2\n");
                }
                else
                {
                    std::printf("[dual] CH2 slot[%d] does not exist (chain has %zu slot(s))\n", key - '0',
                                ch2Slots.size());
                }
                break;
            }
            selectSlot(static_cast<std::size_t>(key - '1'));
            break;
        case kKeyUp:
            if (dualMode && vstFocusChannel == 1)
            {
                if (!ch2Slots.empty() && ch2Selected > 0) { ch2Selected--; std::printf("[dual] CH2 selected slot[%zu] \"%s\"\n", ch2Selected+1, ch2Slots[ch2Selected]->name.c_str()); }
                break;
            }
            if (!slots.empty() && selected > 0)
            {
                selectSlot(selected - 1);
            }
            break;
        case kKeyDown:
            if (dualMode && vstFocusChannel == 1)
            {
                if (!ch2Slots.empty() && ch2Selected + 1 < ch2Slots.size()) { ch2Selected++; std::printf("[dual] CH2 selected slot[%zu] \"%s\"\n", ch2Selected+1, ch2Slots[ch2Selected]->name.c_str()); }
                break;
            }
            if (!slots.empty())
            {
                selectSlot(selected + 1);
            }
            break;
        case kKeyLeft:
            if (dualMode && vstFocusChannel == 1)
            {
                if (ch2Selected > 0 && ch2Selected < ch2Slots.size())
                {
                    auto item = std::move(ch2Slots[ch2Selected]);
                    ch2Slots.erase(ch2Slots.begin() + static_cast<std::ptrdiff_t>(ch2Selected));
                    ch2Slots.insert(ch2Slots.begin() + static_cast<std::ptrdiff_t>(ch2Selected - 1), std::move(item));
                    ch2Selected--;
                    publishCh2Chain(); printChainOrder();
                }
                break;
            }
            if (selected > 0)
            {
                moveSlot(selected, selected - 1);
            }
            break;
        case kKeyRight:
            if (dualMode && vstFocusChannel == 1)
            {
                if (!ch2Slots.empty() && ch2Selected + 1 < ch2Slots.size())
                {
                    auto item = std::move(ch2Slots[ch2Selected]);
                    ch2Slots.erase(ch2Slots.begin() + static_cast<std::ptrdiff_t>(ch2Selected));
                    ch2Slots.insert(ch2Slots.begin() + static_cast<std::ptrdiff_t>(ch2Selected + 1), std::move(item));
                    ch2Selected++;
                    publishCh2Chain(); printChainOrder();
                }
                break;
            }
            if (!slots.empty() && selected + 1 < slots.size())
            {
                moveSlot(selected, selected + 1);
            }
            break;
        case 'e':
            if (dualMode && vstFocusChannel == 1)
            {
                if (ch2Slots.empty())
                {
                    std::printf("[dual] CH2 editor unavailable (wire - no plug-in)\n");
                    break;
                }
                if (pluginEditor.isOpen() && editorOpenChannel == 1 && editorOpenSlot == ch2Selected)
                {
                    closeEditor();
                }
                else
                {
                    (void)openEditorForSlotOnChannel(1, ch2Selected);
                }
                break;
            }
            if (pluginEditor.isOpen() && editorOpenChannel == 0 && editorOpenSlot == selected)
            {
                closeEditor();
            }
            else
            {
                (void)openEditorForSlotOnChannel(0, selected);
            }
            break;
        case 'b':
            if (dualMode && vstFocusChannel == 1)
            {
                const bool next2 = !ch2ChainBypass.load(std::memory_order_relaxed);
                ch2ChainBypass.store(next2, std::memory_order_relaxed);
                std::printf("[dual] CH2 chain bypass %s (raw path)\n", next2 ? "ON" : "OFF");
                publishCh2Chain();
                break;
            }
            toggleWholeBypass();
            break;
        case 'a':
            if (dualMode && vstFocusChannel == 1)
            {
                if (ch2Slots.size() >= audient::vst3::Vst3Chain::kMaxSlots)
                {
                    std::printf("[dual] CH2 cannot add: chain is full (max %zu slots)\n",
                                audient::vst3::Vst3Chain::kMaxSlots);
                    break;
                }
                {
                    const std::string path = pickVst3File(vst3Dir);
                    if (path.empty())
                    {
                        std::printf("[dual] CH2 add cancelled\n");
                        break;
                    }
                    auto slot = createPluginSlot(path, false, false);
                    if (slot == nullptr) { break; }
                    std::printf("[vst3-ch2] ch2 slot[%zu] \"%s\" active (latency %u samples, layout=%s)\n",
                                ch2Slots.size() + 1, slot->name.c_str(),
                                static_cast<unsigned>(slot->processor->latencySamples()),
                                slot->processor->layout() == audient::vst3::BusLayout::Stereo ? "stereo" : "mono");
                    ch2Slots.push_back(std::move(slot));
                    ch2Selected = ch2Slots.size() - 1;
                    publishCh2Chain();
                    printChainOrder();
                }
                break;
            }
            addSlotFromPicker();
            break;
        case 's':
            if (dualMode && vstFocusChannel == 1)
            {
                if (ch2Selected >= ch2Slots.size()) { break; }
                ch2Slots[ch2Selected]->bypass = !ch2Slots[ch2Selected]->bypass;
                std::printf("[dual] CH2 slot[%zu] \"%s\" %s\n", ch2Selected + 1,
                            ch2Slots[ch2Selected]->name.c_str(),
                            ch2Slots[ch2Selected]->bypass ? "BYPASSED" : "ACTIVE");
                publishCh2Chain();
                break;
            }
            toggleSlotBypass(selected);
            break;
        case 'x':
            if (dualMode && vstFocusChannel == 1)
            {
                if (ch2Selected >= ch2Slots.size())
                {
                    std::printf("[dual] CH2 remove: no slot %zu\n", ch2Selected + 1);
                    break;
                }
                if (pluginEditor.isOpen() && editorOpenChannel == 1 && ch2Selected == editorOpenSlot) { closeEditor(); }
                std::printf("[dual] CH2 removing slot[%zu] \"%s\"...\n", ch2Selected + 1,
                            ch2Slots[ch2Selected]->name.c_str());
                ch2Retiring.push_back(std::move(ch2Slots[ch2Selected]));
                ch2Slots.erase(ch2Slots.begin() + static_cast<std::ptrdiff_t>(ch2Selected));
                if (!ch2Slots.empty())
                {
                    ch2Selected = std::min(ch2Selected, ch2Slots.size() - 1);
                }
                else { ch2Selected = 0; }
                publishCh2Chain();
                printChainOrder();
                break;
            }
            removeSlot(selected);
            break;
        case 'm':
            toggleLocalMonitor();
            break;
        case 'k':
            toggleOutputMono();
            break;
        case '[':
            {
                const std::size_t ch = tuiFocusChannel;
                if (dualMode)
                {
                    if (ch == 0) localMonCh0LevelDb = std::max(-60.0f, std::min(0.0f, localMonCh0LevelDb - 1.0f));
                    else localMonCh1LevelDb = std::max(-60.0f, std::min(0.0f, localMonCh1LevelDb - 1.0f));
                    publishChannelLocalSend(ch);
                }
            }
            break;
        case ']':
            {
                const std::size_t ch = tuiFocusChannel;
                if (dualMode)
                {
                    if (ch == 0) localMonCh0LevelDb = std::max(-60.0f, std::min(0.0f, localMonCh0LevelDb + 1.0f));
                    else localMonCh1LevelDb = std::max(-60.0f, std::min(0.0f, localMonCh1LevelDb + 1.0f));
                    publishChannelLocalSend(ch);
                }
            }
            break;
        case ';':
            {
                const std::size_t ch = tuiFocusChannel;
                if (dualMode)
                {
                    if (ch == 0) localMonCh0Muted = !localMonCh0Muted;
                    else localMonCh1Muted = !localMonCh1Muted;
                    if (ch == 0 && !localMonCh0Enabled && !localMonCh0Muted) localMonCh0Enabled = true;
                    if (ch == 1 && !localMonCh1Enabled && !localMonCh1Muted) localMonCh1Enabled = true;
                    publishChannelLocalSend(ch);
                }
            }
            break;
        case ':':
            {
                const std::size_t ch = tuiFocusChannel;
                if (dualMode)
                {
                    tuiFocusChannel = ch == 0 ? 1 : 0;
                    std::printf("[localmon] focus CH%zu (ch1 send: %s %.1f dB | ch2: %s %.1f dB) [:/toggle focus; [/]:dB; ;:mute]\n",
                                tuiFocusChannel + 1,
                                localMonCh0Enabled && !localMonCh0Muted ? "ON" : "OFF", localMonCh0LevelDb,
                                localMonCh1Enabled && !localMonCh1Muted ? "ON" : "OFF", localMonCh1LevelDb);
                }
            }
            break;
        case '\'':
            {
                const std::size_t ch = tuiFocusChannel;
                if (dualMode)
                {
                    if (ch == 0) localMonCh0Enabled = !localMonCh0Enabled;
                    else localMonCh1Enabled = !localMonCh1Enabled;
                    publishChannelLocalSend(ch);
                }
            }
            break;
        case '+':
        case '=':
            changeLocalMonitorGain(1.0f);
            break;
        case '-':
        case '_':
            changeLocalMonitorGain(-1.0f);
            break;
        case 'h':
            hwMode = true;
            hwSelHeadphones = false;
            std::printf("[hardware] HARDWARE VOLUME CONTROL - iD14 physical Monitor/Headphones "
                        "(1/2 select, +/- or Up/Down +/-1 dB, h exit, q quit)\n");
            break;
        case ',':
        case '<':
            if (selected > 0)
            {
                moveSlot(selected, selected - 1);
            }
            else
            {
                std::printf("[ui] slot already first in chain\n");
            }
            break;
        case '.':
        case '>':
            if (!slots.empty() && selected + 1 < slots.size())
            {
                moveSlot(selected, selected + 1);
            }
            else
            {
                std::printf("[ui] slot already last in chain\n");
            }
            break;
        case 'q':
            quit.store(true, std::memory_order_relaxed);
            break;
        case 'o':
            std::printf("[ui] LOCAL MONITOR is a single bus now: 'm' toggles it ON/OFF, "
                        "+/- adjust the software gain; the iD14 physical volume controls "
                        "are under 'h' (HARDWARE).\n");
            break;
        default:
            break;
        }
    };

    // Scripted input queue (--keys). A feeder thread pushes logical keys; the
    // main control loop drains them exactly like typed keys.
    std::mutex scriptMtx;
    std::deque<int> scriptKeys;
    const auto popScriptKey = [&]() -> int {
        std::lock_guard<std::mutex> guard(scriptMtx);
        if (scriptKeys.empty())
        {
            return kNoKey;
        }
        const int key = scriptKeys.front();
        scriptKeys.pop_front();
        return key;
    };
    std::thread keyFeeder;
    if (!scriptTokens.empty())
    {
        keyFeeder = std::thread([&]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            for (const std::string& token : scriptTokens)
            {
                int key = kNoKey;
                if (token == "up")
                {
                    key = kKeyUp;
                }
                else if (token == "down")
                {
                    key = kKeyDown;
                }
                else if (token == "left")
                {
                    key = kKeyLeft;
                }
                else if (token == "right")
                {
                    key = kKeyRight;
                }
                else if (!token.empty())
                {
                    key = static_cast<unsigned char>(token[0]);
                }
                if (key != kNoKey)
                {
                    {
                        std::lock_guard<std::mutex> guard(scriptMtx);
                        scriptKeys.push_back(key);
                    }
                    std::printf("[keys] queued '%s'\n", token.c_str());
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(400));
            }
        });
    }

    const auto dbText = [](float peak) {
        char buffer[16]{};
        if (peak > 0.0001f)
        {
            std::snprintf(buffer, sizeof(buffer), "%6.1f", 20.0 * std::log10(static_cast<double>(peak)));
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer), "%6s", "-120.0");
        }
        return std::string(buffer);
    };
    const auto fmtDb = [](float db) {
        char buffer[16]{};
        std::snprintf(buffer, sizeof(buffer), "%6.1f", static_cast<double>(db));
        return std::string(buffer);
    };
    const auto fmtOptDb = [](const std::optional<double>& db) {
        if (!db.has_value())
        {
            return std::string("   n/a");
        }
        char buffer[16]{};
        std::snprintf(buffer, sizeof(buffer), "%6.1f", *db);
        return std::string(buffer);
    };

    std::uint64_t lastRecoveryForDepop = 0;
    bool depopLogged = false;
    std::uint64_t lastConsumedSamples = 0;
    bool haveLastConsumed = false;
    long long lastCallbackCount = -1;
    std::chrono::steady_clock::time_point lastStatusAt = std::chrono::steady_clock::now();

    if (useGui) { INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES}; InitCommonControlsEx(&icc); }
    HWND diagHwnd = nullptr;
    HWND webHwnd = nullptr;
    bool useWebGui = true;
    audient::daily_gui::DailyTray dailyTray;
    // Daily mode (interactive, no --seconds) hides to the tray on close and gets
    // a tray icon; diagnostic/--seconds runs keep the real close/no-tray behavior.
    const bool dailyTrayMode = useGui && secondsArg == 0;
    if (useGui)
    {
        audient::daily_gui::DailyGuiConfig dcfg;
        dcfg.dualMode = dualMode;
        dcfg.userScale = 1.0f;
        dcfg.onUserScaleChanged = [&](float s){ (void)s; };
        dcfg.getGlobalMonitorOn = [&]() -> bool { return !monitorMuted; };
        dcfg.setGlobalMonitorOn = [&](bool on) {
            monitorMuted = !on;
            publishOutputState();
            auto cfg = asioStream.snapshotRenderConfig();
            std::printf("[diag] Global Local Monitor %s (monitorMute=%d cfg.outputMono=%d rev %llu)\n",
                        on ? "ON" : "OFF", cfg.monitorMute, cfg.outputMono,
                        (unsigned long long)cfg.revision);
        };
        dcfg.getMonoOn = [&]() -> bool { return outputMono; };
        dcfg.setMonoOn = [&](bool on) {
            outputMono = on;
            publishOutputState();
            auto cfg = asioStream.snapshotRenderConfig();
            std::printf("[diag] MONO %s (cfg.outputMono=%d rev %llu monitorMute=%d)\n",
                        on ? "ON" : "OFF", cfg.outputMono,
                        (unsigned long long)cfg.revision, cfg.monitorMute);
        };
        dcfg.getLocalEnable = [&](int ch) -> bool { return ch == 0 ? localMonCh0Enabled : localMonCh1Enabled; };
        dcfg.setLocalEnable = [&](int ch, bool v) {
            if (ch == 0) localMonCh0Enabled = v; else localMonCh1Enabled = v;
            publishChannelLocalSend((std::size_t)ch);
        };
        dcfg.getLocalMute = [&](int ch) -> bool { return ch == 0 ? localMonCh0Muted : localMonCh1Muted; };
        dcfg.setLocalMute = [&](int ch, bool v) {
            if (ch == 0) localMonCh0Muted = v; else localMonCh1Muted = v;
            publishChannelLocalSend((std::size_t)ch);
        };
        dcfg.getLocalGainDb = [&](int ch) -> float { return ch == 0 ? localMonCh0LevelDb : localMonCh1LevelDb; };
        dcfg.setLocalGainDb = [&](int ch, float db) {
            db = std::max(-60.f, std::min(0.f, db));
            if (ch == 0) localMonCh0LevelDb = db; else localMonCh1LevelDb = db;
            publishChannelLocalSend((std::size_t)ch);
        };
        dcfg.getMetersCh1 = [&]() -> std::pair<float,float> {
            auto s = asioStream.snapshotStatus();
            if (s.dualPlan) return {s.ch1RawPeak, s.ch1PostPeak};
            float raw = micInputTap.blockPeak.load(std::memory_order_relaxed);
            float post = s.live && s.micDb > -119 ? (float)std::pow(10.0, s.micDb/20.0) : 0.f;
            return {raw, post};
        };
        dcfg.getMetersCh2 = [&]() -> std::pair<float,float> {
            auto s = asioStream.snapshotStatus();
            return {s.ch2RawPeak, s.ch2PostPeak};
        };
        dcfg.getCh1Names = [&]() -> std::vector<std::string> {
            std::vector<std::string> out;
            for (size_t i=0;i<slots.size();++i) out.push_back(std::to_string(i+1)+": "+slots[i]->name+(slots[i]->bypass?" [BYP]":""));
            return out;
        };
        dcfg.getCh2Names = [&]() -> std::vector<std::string> {
            std::vector<std::string> out;
            for (size_t i=0;i<ch2Slots.size();++i) out.push_back(std::to_string(i+1)+": "+ch2Slots[i]->name+(ch2Slots[i]->bypass?" [BYP]":""));
            return out;
        };
        dcfg.getCh1Sel = [&]() -> int { return slots.empty()? -1 : (int)selected; };
        dcfg.getCh2Sel = [&]() -> int { return ch2Slots.empty()? -1 : (int)ch2Selected; };
        dcfg.setSel = [&](int ch, int idx) {
            if (ch==0 && idx>=0 && (size_t)idx<slots.size()) selected=(size_t)idx;
            if (ch==1 && idx>=0 && (size_t)idx<ch2Slots.size()) ch2Selected=(size_t)idx;
        };
        dcfg.onLoad = [&](int ch) {
            std::string path = pickVst3File(vst3Dir);
            if (path.empty()) return;
            auto slot = createPluginSlot(path,false,false);
            if (!slot) return;
            if (ch==0){ slots.push_back(std::move(slot)); selected=slots.size()-1; publishChain(); printChainOrder(); }
            else { ch2Slots.push_back(std::move(slot)); ch2Selected=ch2Slots.size()-1; publishCh2Chain(); printChainOrder(); }
        };
        dcfg.onOpenEditor = [&](int ch,int idx){ (void)openEditorForSlotOnChannel((size_t)ch,(size_t)idx); };
        dcfg.onBypassSel = [&](int ch,int idx){
            if (ch==0 && idx>=0 && (size_t)idx<slots.size()){ slots[idx]->bypass=!slots[idx]->bypass; publishChain(); }
            if (ch==1 && idx>=0 && (size_t)idx<ch2Slots.size()){ ch2Slots[idx]->bypass=!ch2Slots[idx]->bypass; publishCh2Chain(); }
        };
        dcfg.onBypassWhole = [&](int ch){
            if (ch==0){ bool n=!chainBypass.load(); chainBypass.store(n); publishChain(); }
            else { bool n=!ch2ChainBypass.load(); ch2ChainBypass.store(n); publishCh2Chain(); }
        };
        dcfg.getWholeBypass = [&](int ch)->bool{ return ch==0? chainBypass.load(): ch2ChainBypass.load(); };
        dcfg.onRemove = [&](int ch,int idx){
            if (ch==0 && idx>=0 && (size_t)idx<slots.size()){
                if (pluginEditor.isOpen() && editorOpenChannel==0 && selected==(size_t)idx) closeEditor();
                retiring.push_back(std::move(slots[idx])); slots.erase(slots.begin()+idx);
                if(!slots.empty()) selected=std::min(selected,slots.size()-1); else selected=0;
                publishChain(); printChainOrder();
            }
            if (ch==1 && idx>=0 && (size_t)idx<ch2Slots.size()){
                if (pluginEditor.isOpen() && editorOpenChannel==1 && ch2Selected==(size_t)idx) closeEditor();
                ch2Retiring.push_back(std::move(ch2Slots[idx])); ch2Slots.erase(ch2Slots.begin()+idx);
                if(!ch2Slots.empty()) ch2Selected=std::min(ch2Selected,ch2Slots.size()-1); else ch2Selected=0;
                publishCh2Chain(); printChainOrder();
            }
        };
        dcfg.getAsioText = [&]() -> std::string {
            auto s=asioStream.snapshotStatus();
            long rate = asioStream.actualSampleRate();
            if (rate <= 0) rate = asioStream.configuredSampleRate();
            char b[128]; std::snprintf(b,sizeof(b),"ASIO %s %ldk/%ld", s.stateText.c_str(), rate/1000, s.buffer);
            return b;
        };
        dcfg.getXrunsText = [&]() -> std::string {
            auto s=asioStream.snapshotStatus();
            char b[128]; std::snprintf(b,sizeof(b),"Xruns %llu Ovl %llu", (unsigned long long)s.xruns, (unsigned long long)s.overloads);
            return b;
        };
        dcfg.getAudioCurrentRate = [&]() -> long { return asioStream.configuredSampleRate(); };
        dcfg.getAudioActualRate = [&]() -> long { return asioStream.actualSampleRate(); };
        dcfg.getAudioCurrentBuffer = [&]() -> long { return asioStream.currentBuffer(); };
        dcfg.getAudioRequestedBuffer = [&]() -> long { return asioStream.requestedBuffer(); };
        dcfg.getAudioSupportedRates = [&]() -> std::vector<long> { return asioStream.supportedSampleRates(); };
        dcfg.getAudioSupportedBuffers = [&]() -> std::vector<long> { return asioStream.supportedBuffers(); };
        dcfg.getAudioRebuilding = [&]() -> bool { return asioStream.rebuilding(); };
        // Pico status is published cached state: the UI reads a lightweight
        // snapshot and never blocks on USB/WASAPI/discovery work.
        dcfg.getPicoConnected = [&]() -> bool {
            return picoWorker != nullptr && picoWorker->status().deviceOpen;
        };
        dcfg.getPicoText = [&]() -> std::string {
            if (picoWorker == nullptr) return "Pico v0.1.1 -- disabled";
            const audient::pico::PicoUsbWorker::Status ps = picoWorker->status();
            return ps.deviceOpen ? "Pico v0.1.1 -- connected" : "Pico v0.1.1 -- disconnected";
        };
        dcfg.getVirtualMicSource = [&]() -> int {
            return (dualMode && prefs.virtualMicSource == audient::preferences::kVirtualMicSourceInput2) ? 1 : 0;
        };
        dcfg.setVirtualMicSource = [&](int source) { applyVirtualMicSource(source); };
        dcfg.getCloseToTray = [&]() -> bool { return prefs.closeToTray; };
        dcfg.setCloseToTray = [&](bool on) { prefs.closeToTray = on; savePrefs(); };
        dcfg.getStartMinimized = [&]() -> bool { return prefs.startMinimized; };
        dcfg.setStartMinimized = [&](bool on) { prefs.startMinimized = on; savePrefs(); };
        // Presentation-only view mode (0 = Main Mixer, 1 = Mini Monitor). Persisted;
        // the host uses it to size the fixed window.
        dcfg.getUiMode = [&]() -> int { return prefs.uiMode; };
        dcfg.setUiMode = [&](int mode) { prefs.uiMode = (mode == audient::preferences::kUiModeMini) ? audient::preferences::kUiModeMini : audient::preferences::kUiModeMain; savePrefs(); };
        // Stereo SYSTEM output peaks (L/R) from the iD14 render endpoint (read-only,
        // non-RT worker). -1 = endpoint unavailable (UI shows "--", never fake).
        dcfg.getSystemPeakL = [&]() -> float { return pcMeter.peakL(); };
        dcfg.getSystemPeakR = [&]() -> float { return pcMeter.peakR(); };
        // Windows SYSTEM endpoint mute (IAudioEndpointVolume, endpoint-wide).
        // State is read from Windows by the non-RT worker; the command only
        // requests a write. Unavailable => UI shows the control disabled. No ASIO
        // recovery is ever triggered by endpoint/mute failure.
        dcfg.getSystemMuted = [&]() -> std::optional<bool> { return pcMeter.systemMuted(); };
        dcfg.setSystemMute = [&](bool on) { pcMeter.requestSystemMute(on); };
        dcfg.getAppVersion = [&]() -> std::string { return audient::core::fullVersionString(); };
        // User-facing audio-device status comes from the ACCEPTED ACTIVE ASIO
        // session / known selected Audient device (or a genuine physical loss) -
        // never from the separate hardware-control (Monitor-HW) handle, which
        // can legitimately be closed while ASIO is streaming.
        dcfg.getAudioDeviceStatus = [&]() -> std::string {
            std::string text;
            if (recovery.state() == audient::asio::PhysDeviceState::Lost)
            {
                text = "iD14 MK1 -- Lost (recovering)";
            }
            else
            {
                const DemoAsioStream::Status s = asioStream.snapshotStatus();
                if (s.stateText == "Streaming" || s.stateText == "Ready")
                {
                    text = "iD14 MK1 -- Connected";
                }
                else if (s.stateText == "Reconnecting")
                {
                    text = "iD14 MK1 -- Reconnecting";
                }
                else if (s.stateText == "NoDevice" || s.stateText == "Error")
                {
                    text = "iD14 MK1 -- Disconnected";
                }
                else
                {
                    text = s.live ? "iD14 MK1 -- Connected" : "iD14 MK1 -- Disconnected";
                }
            }
            return text;
        };
        // The separate hardware-control (iD Mixer replacement) channel is
        // labeled explicitly so it is never mistaken for audio connectivity.
        dcfg.getHardwareControlStatus = [&]() -> std::string {
            return (hw.isOpen() && hw.connected()) ? "Connected" : "Not connected";
        };
        // SINGLE buffer-command implementation shared by the WebView and the tray.
        // UI-thread contract: validate + stage + publish Reconfiguring + return.
        // No driver query, no ASIO stop/start, no lock across long work.
        auto setBufferCommand = [&](long samples) -> bool {
            if (!asioStream.requestBuffer(samples)) {
                std::printf("[audio] buffer %ld ignored (duplicate/unsupported/active)\n", samples);
                return false;
            }
            asioStream.markReconfiguring();
            audioReconfigureRequested.store(true);
            std::printf("[audio] buffer change requested: %ld samples (queued for control worker)\n", samples);
            return true;
        };
        dcfg.onSetBufferSize = setBufferCommand;
        dcfg.getHwMonDb = [&]() -> std::optional<double> { if(!hw.isOpen()||!hw.connected()) return std::nullopt; return hw.monitorDb(); };
        dcfg.getHwHpDb = [&]() -> std::optional<double> { if(!hw.isOpen()||!hw.connected()) return std::nullopt; return hw.headphoneDb(); };
        dcfg.getHwMonText = [&]() -> std::string {
            if(!hw.isOpen()||!hw.connected()) return "Monitor HW --";
            auto v=hw.monitorDb(); char b[64]; if(v) std::snprintf(b,sizeof(b),"Monitor HW %.1f dB",*v); else std::snprintf(b,sizeof(b),"Monitor HW n/a"); return b;
        };
        dcfg.getHwHpText = [&]() -> std::string {
            if(!hw.isOpen()||!hw.connected()) return "Headphone HW --";
            auto v=hw.headphoneDb(); char b[64]; if(v) std::snprintf(b,sizeof(b),"Headphone HW %.1f dB",*v); else std::snprintf(b,sizeof(b),"Headphone HW n/a"); return b;
        };
        dcfg.onNudgeHw = [&](bool hp,double d){ hwSelHeadphones = hp; changeHardwareLevel(d); };
        const auto setHwAbs = [&](bool hp, double db){
            if (!hw.isOpen() || !hw.connected()) return;
            hwSelHeadphones = hp;
            auto r = hp ? hw.setHeadphoneDb(db) : hw.setMonitorDb(db);
            (void)r;
        };
        dcfg.onSetHwDb = [&](bool hp, double db){ setHwAbs(hp, db); };
        if (useWebGui)
        {
            wchar_t exePath[MAX_PATH]{};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring exeDir(exePath);
            auto pos = exeDir.find_last_of(L"\\/");
            if (pos != std::wstring::npos) exeDir = exeDir.substr(0, pos);
            const std::wstring candidates[] = {
                exeDir + L"\\ui",                              // production: ui/ beside the exe
                exeDir + L"\\..\\..\\..\\ui",                  // build tree: <config>/ui
                exeDir + L"\\..\\..\\..\\src\\daily_gui\\ui",  // dev: repo source ui
            };
            std::wstring htmlDir;
            for (const std::wstring& candidate : candidates)
            {
                wchar_t absHtml[MAX_PATH]{};
                GetFullPathNameW(candidate.c_str(), MAX_PATH, absHtml, nullptr);
                if (GetFileAttributesW((std::wstring(absHtml) + L"\\index.html").c_str()) != INVALID_FILE_ATTRIBUTES)
                {
                    htmlDir = absHtml;
                    break;
                }
            }
            if (htmlDir.empty())
            {
                htmlDir = exeDir + L"\\ui";
            }
            wchar_t localAppData[MAX_PATH]{};
            const DWORD ladLen = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
            std::wstring userDataDir;
            if (ladLen > 0 && ladLen < MAX_PATH)
            {
                const std::wstring appDir = std::wstring(localAppData) + L"\\Audient Console";
                CreateDirectoryW(appDir.c_str(), nullptr);
                userDataDir = appDir + L"\\WebView2";
            }
            else
            {
                userDataDir = exeDir + L"\\webview2_data";
            }
            audient::daily_gui::DailyWebViewControllerConfig wcfg{};
            wcfg.model = dcfg;
            wcfg.htmlDir = htmlDir;
            wcfg.userDataDir = userDataDir;
            wcfg.hideOnClose = dailyTrayMode;
            wcfg.shouldHideOnClose = [&]() -> bool { return dailyTrayMode && prefs.closeToTray; };
            webHwnd = audient::daily_gui::DailyWebViewController_Create(GetModuleHandleW(nullptr), wcfg);
            if (webHwnd)
            {
                if (!(dailyTrayMode && prefs.startMinimized))
                {
                    ShowWindow(webHwnd, SW_SHOW);
                }
                else
                {
                    std::printf("[gui] start-minimized-to-tray: window hidden, engine + tray running\n");
                }
                std::wprintf(L"[diag-gui] webview panel open (--gui) htmlDir=%ls\n", htmlDir.c_str());
            }
            else
            {
                diagHwnd = audient::daily_gui::Create(GetModuleHandleW(nullptr), dcfg);
                if(diagHwnd) std::printf("[diag-gui] webview failed, fallback Win32 panel open\n");
            }
        }
        else
        {
            diagHwnd = audient::daily_gui::Create(GetModuleHandleW(nullptr), dcfg);
            if(diagHwnd) std::printf("[diag-gui] panel open (--gui) - plain Win32 controls, control thread only\n");
        }

        if (dailyTrayMode)
        {
            audient::daily_gui::DailyTrayConfig tcfg;
            const HWND consoleWindow = webHwnd != nullptr ? webHwnd : diagHwnd;
            tcfg.title = L"Audient Console";
            tcfg.onOpen = [consoleWindow]() {
                if (consoleWindow != nullptr)
                {
                    audient::daily_gui::DailyWebViewController_Show(consoleWindow);
                }
            };
            tcfg.onExit = [&]() { quit.store(true); };
            tcfg.onSetBuffer = [setBufferCommand](long samples) { (void)setBufferCommand(samples); };
            tcfg.getCurrentBuffer = [&]() { return asioStream.currentBuffer(); };
            tcfg.getSupportedBuffers = [&]() { return asioStream.supportedBuffers(); };
            tcfg.isRebuilding = [&]() { return asioStream.rebuilding(); };
            if (dailyTray.create(GetModuleHandleW(nullptr), tcfg))
            {
                std::printf("[tray] notification-area icon created (daily mode; X hides to tray)\n");
            }
            else
            {
                std::printf("WARN: tray icon could not be created\n");
            }
        }
    }

    if (editorRequested && !slots.empty())
    {
        (void)openEditorForSlot(selected);
    }
    else if (editorRequested)
    {
        std::printf("[ui] --editor given but no plug-in is loaded; nothing to open\n");
    }

    while (!g_stop.load(std::memory_order_relaxed) && !quit.load(std::memory_order_relaxed))
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            break;
        }

        // Pump the native editor window's messages (no-op when no window).
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
        }

        // Input: real console keys (_kbhit) + --keys scripted input, both fed
        // through the SAME dispatchKey() path. Chain mutations republish an
        // immutable snapshot and never touch the audio callback. Selection and
        // chain-edit keys can NEVER end the loop - 'q' is the only normal quit
        // key (Ctrl+C and --seconds also end cleanly). A numeric select for a
        // missing slot just logs and keeps streaming.
        if (tui.active() && _kbhit())
        {
            int key = _getch();
            if (key == 0 || key == 0xE0) // extended keys (arrows) = 0xE0 then scan code
            {
                const int scan = _getch();
                switch (scan)
                {
                case 0x48:
                    dispatchKey(kKeyUp);
                    break;
                case 0x50:
                    dispatchKey(kKeyDown);
                    break;
                case 0x4B:
                    dispatchKey(kKeyLeft);
                    break;
                case 0x4D:
                    dispatchKey(kKeyRight);
                    break;
                default:
                    break;
                }
            }
            else
            {
                dispatchKey(std::tolower(key));
            }
        }
        for (;;) // --keys scripted input (drained on the main thread)
        {
            const int key = popScriptKey();
            if (key == kNoKey)
            {
                break;
            }
            dispatchKey(key);
        }
        reapRetired();

        // Debounced mixer-state autosave (control thread only; never the ASIO
        // callback). Chain mutations set mixerDirty -> save ~500 ms after the
        // last change; a periodic save also captures editor parameter edits.
        {
            const auto nowSave = std::chrono::steady_clock::now();
            const auto sinceSave = std::chrono::duration_cast<std::chrono::milliseconds>(nowSave - lastMixerSave).count();
            const bool haveChain = !slots.empty() || !ch2Slots.empty();
            if (mixerPersist && ((mixerDirty.load(std::memory_order_relaxed) && sinceSave >= 500) ||
                                 (haveChain && sinceSave >= 60000)))
            {
                saveMixerState();
                lastMixerSave = nowSave;
            }
        }

        const bool isTui = tui.active();
        const auto tickCadence = isTui ? std::chrono::milliseconds(200) : std::chrono::seconds(1);
        const auto nowTick = std::chrono::steady_clock::now();
        const double elapsedSec =
            std::chrono::duration<double>(nowTick - lastStatusAt).count();
        if (elapsedSec < 0.15)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            continue;
        }
        lastStatusAt = nowTick;

        const DemoAsioStream::Status snap = asioStream.snapshotStatus();
        const bool physLost = recovery.state() == audient::asio::PhysDeviceState::Lost;
        const audient::virtual_audio::VirtualMicFeeder::Snapshot f = feeder.snapshot();

        // Authoritative actual buffer: the value the control side published for
        // the running session (never a rate-derived estimate; stable across a
        // rebuild). The callback-rate estimate is kept only as a diagnostic
        // `estbuf` and is never published to the UI.
        const long long cbNow = static_cast<long long>(snap.callbacks);
        const long long cbDelta = lastCallbackCount >= 0 ? cbNow - lastCallbackCount : 0;
        const bool cbCounterReset = lastCallbackCount >= 0 && cbNow < lastCallbackCount;
        lastCallbackCount = cbNow;
        const long actualBuffer = asioStream.currentBuffer();
        long estimatedBuffer = 0;
        if (!cbCounterReset && cbDelta > 0 && snap.live && elapsedSec > 0.001)
        {
            const double rate = static_cast<double>(cbDelta) / elapsedSec;
            estimatedBuffer = static_cast<long>(std::lround(48000.0 / rate));
        }
        const long cbPerSec = static_cast<long>(
            snap.live && elapsedSec > 0.001 ? std::lround(static_cast<double>(cbDelta) / elapsedSec) : 0);

        audient::capture_control::CaptureControlState st{};
        const bool gotDriverState = !appOnlyMode && client.queryState(&st);
        const std::uint64_t consumedNow = gotDriverState ? st.consumedSamples : 0;
        const bool consumerActive = gotDriverState && st.connected && haveLastConsumed &&
                                    consumedNow > lastConsumedSamples;
        haveLastConsumed = gotDriverState && st.connected;
        if (gotDriverState)
        {
            lastConsumedSamples = st.consumedSamples;
        }

        audient::capture_ring::CaptureRingHeader* header = sink.region();
        const std::uint64_t ringBacklog =
            (header != nullptr && header->capacityFrames > 0 && header->writePos > header->readPos)
                ? (header->writePos - header->readPos)
                : 0ull;
        const std::uint64_t transportBacklog = captureTransport.availableFrames();

        const float legacyRawPeak = snap.live ? micInputTap.blockPeak.load(std::memory_order_relaxed) : 0.0f;
        const float rawPeak = snap.dualPlan ? snap.ch1RawPeak : legacyRawPeak;
        const float legacyPostPeak = snap.live && snap.micDb > -119.0 ? static_cast<float>(std::pow(10.0, snap.micDb / 20.0)) : 0.0f;
        const float postPeak = snap.dualPlan ? snap.ch1PostPeak : legacyPostPeak;
    const bool bypassed = chainBypass.load(std::memory_order_relaxed);

        // HARDWARE device-volume: auto-open once per second until the iD14 is
        // available; afterwards the module's own poll thread handles reconnect
        // (re-reading hardware, never restoring stale software volume). This runs
        // in EVERY Daily runtime mode (interactive TUI, WebView GUI, detached
        // desktop, tray/minimized) - it is deliberately NOT gated on --gui or on
        // an active TUI. --no-hardware disables the whole layer (A/B pop tests).
        // Hardware control is an isolated failure domain: a failed open or a later
        // loss leaves ASIO/VST/Pico/virtual mic/Local Monitor/tray running and must
        // never trigger AsioRecoveryController.
        const auto hwNow = std::chrono::steady_clock::now();
        if (!noHardware && !hw.isOpen() && hwNow >= hwRetryAt)
        {
            hwRetryAt = hwNow + std::chrono::seconds(1);
            hw.setPollPeriodMs(static_cast<unsigned>(hwPollMs));
            const audient::hardware::Result openRes = hw.open();
            if (openRes != audient::hardware::Result::Ok && !hwOpenFailureLogged)
            {
                hwOpenFailureLogged = true;
                const std::string err = hw.lastError();
                std::printf("[hardware] unavailable: open failed: %s%s%s\n",
                            audient::hardware::resultName(openRes),
                            err.empty() ? "" : " (", err.empty() ? "" : err.c_str());
            }
        }
        // State-transition logging only (no per-poll noise): the module's own poll
        // thread can lose and silently re-open the device, so report the connected
        // edges observed on the control thread.
        const bool hwConnectedNow = hw.isOpen() && hw.connected();
        if (hwConnectedNow != hwConnectedPrev)
        {
            if (hwConnectedNow)
            {
                hwOpenFailureLogged = false;
                std::printf("[hardware] connected - iD14 Monitor/Headphones control ready\n");
            }
            else
            {
                std::printf("[hardware] disconnected/lost - ASIO/VST/Pico unaffected\n");
            }
            hwConnectedPrev = hwConnectedNow;
        }

        if (isTui)
        {
            const double ringMs = static_cast<double>(ringBacklog) * 1000.0 / 48000.0;
            const double transportMs = static_cast<double>(transportBacklog) * 1000.0 / 48000.0;
            const std::string driverText = gotDriverState
                ? (st.connected ? "CONNECTED gen=" + std::to_string(st.generation) : "disconnected")
                : (appOnlyMode ? "app-only" : "offline");

            // VST chain panel: a header + one row per plug-in IN PROCESSING ORDER
            // (top of the list = first in the chain). Total latency is the active
            // path sum; per-slot latency is each plug-in's reported value.
            const bool wholeBypass = bypassed;
            const unsigned totalLatency = micChain.totalLatencySamples();
            const std::string selectedText = slots.empty() ? "-" : std::to_string(selected + 1);
            char vstHead[160]{};
            std::snprintf(vstHead, sizeof(vstHead),
                          "VST CHAIN  %zu slot(s) | total latency %u samples | whole bypass %s | selected [%s]",
                          slots.size(), totalLatency, wholeBypass ? "ON" : "OFF", selectedText.c_str());
            std::size_t nameCol = 20;
            for (const auto& s : slots)
            {
                nameCol = std::max(nameCol, std::min<std::size_t>(s->name.size(), 30));
            }
            std::vector<std::string> chainLines;
            chainLines.push_back(vstHead);
            if (slots.empty())
            {
                chainLines.push_back("  (chain empty - press 'a' to add a plug-in)");
            }
            for (std::size_t i = 0; i < slots.size(); ++i)
            {
                const auto& s = slots[i];
                char row[192]{};
                std::snprintf(row, sizeof(row), " %s [%zu] %-*s  %-9s  lat %u%s", selected == i ? ">" : " ", i + 1,
                              static_cast<int>(nameCol), s->name.c_str(),
                              s->bypass ? "BYPASSED" : "ACTIVE",
                              static_cast<unsigned>(s->processor->latencySamples()),
                              (pluginEditor.isOpen() && editorOpenChannel == 0 && i == editorOpenSlot) ? "   *editor open" : "");
                chainLines.push_back(row);
            }
            if (dualMode)
            {
                const unsigned ch2Latency = ch2Chain.totalLatencySamples();
                char ch2Head[160]{};
                std::snprintf(ch2Head, sizeof(ch2Head), "CH2 CHAIN  %zu slot(s) | total latency %u samples | whole bypass %s | selected [%s] | focus %s",
                              ch2Slots.size(), ch2Latency, ch2ChainBypass.load(std::memory_order_relaxed) ? "ON" : "OFF",
                              ch2Slots.empty() ? "-" : std::to_string(ch2Selected + 1).c_str(), vstFocusChannel == 1 ? "CH2" : "CH1");
                chainLines.push_back(ch2Head);
                if (ch2Slots.empty())
                {
                    chainLines.push_back("  (CH2 wire - no plug-in)");
                }
                for (std::size_t i = 0; i < ch2Slots.size(); ++i)
                {
                    const auto& s = ch2Slots[i];
                    char row[192]{};
                    std::snprintf(row, sizeof(row), " %s [%zu] %-*s  %-9s  lat %u%s", ch2Selected == i && vstFocusChannel == 1 ? ">" : " ", i + 1,
                                  static_cast<int>(nameCol), s->name.c_str(),
                                  s->bypass ? "BYPASSED" : "ACTIVE",
                                  static_cast<unsigned>(s->processor->latencySamples()),
                                  (pluginEditor.isOpen() && editorOpenChannel == 1 && i == editorOpenSlot) ? "   *editor open" : "");
                    chainLines.push_back(row);
                }
            }

            std::vector<std::string> pane;
            pane.push_back("----------------------------------------------------------------");
            char asio[256]{};
            std::snprintf(asio, sizeof(asio), "ASIO      %-10s | 48 kHz | %ld samples | cb %ld/s | xr %llu ovl %llu",
                          snap.stateText.c_str(), snap.buffer, cbPerSec,
                          static_cast<unsigned long long>(snap.xruns),
                          static_cast<unsigned long long>(snap.overloads));
            pane.push_back(asio);
            const float rawPeakEffective = dualMode ? snap.ch1RawPeak : rawPeak;
            const float postPeakEffective = dualMode ? snap.ch1PostPeak : postPeak;
            char mic[256]{};
            std::snprintf(mic, sizeof(mic), "Mic       RAW %s dBFS | POST %s dBFS%s",
                          dbText(rawPeakEffective).c_str(), dbText(postPeakEffective).c_str(),
                          dualMode ? "  [CH1 authoritative]" : "");
            pane.push_back(mic);
            if (dualMode)
            {
                char dual[256]{};
                std::snprintf(dual, sizeof(dual),
                              "Dual Ch1 RAW %s dBFS | POST %s dBFS || Ch2 RAW %s dBFS | POST %s dBFS  (%u in, cfg %zu, vm=%s, focus %s)",
                              dbText(snap.ch1RawPeak).c_str(),
                              dbText(snap.ch1PostPeak).c_str(),
                              dbText(snap.ch2RawPeak).c_str(),
                              dbText(snap.ch2PostPeak).c_str(),
                              snap.inputCount, snap.configuredInputs,
                              prefs.virtualMicSource == audient::preferences::kVirtualMicSourceInput2 ? "ch2" : "ch1",
                              vstFocusChannel == 1 ? "CH2 [n]" : "CH1 [n]");
                pane.push_back(dual);
            }
            char monitor[256]{};
            std::snprintf(monitor, sizeof(monitor),
                          "LocalMon  Mic -> Output 1/2  %s (m)  |  software gain %s dB  |  MONO %s (k)",
                          monitorMuted ? "OFF" : "ON", fmtDb(monitorLevelDb).c_str(),
                          outputMono ? "ON" : "OFF");
            pane.push_back(monitor);
            if (dualMode)
            {
                char lm2[192]{};
                std::snprintf(lm2, sizeof(lm2), "  LocalMon CH1 %s %s dB  |  CH2 %s %s dB  (focus %s, [:ch +/-:dB, ;:mute)",
                            localMonCh0Enabled && !localMonCh0Muted ? "ON " : "OFF", fmtDb(localMonCh0LevelDb).c_str(),
                            localMonCh1Enabled && !localMonCh1Muted ? "ON " : "OFF", fmtDb(localMonCh1LevelDb).c_str(),
                            tuiFocusChannel == 1 ? "CH2" : "CH1");
                pane.push_back(lm2);
            }
            for (const std::string& line : chainLines)
            {
                pane.push_back(line);
            }
            char lmRow[192]{};
            std::snprintf(lmRow, sizeof(lmRow), "  Mic -> Output 1/2  %6s dB  %s  |  MONO %s (k)",
                          monitorMuted ? "  -inf" : fmtDb(monitorLevelDb).c_str(),
                          monitorMuted ? "OFF (m)" : "ON (m)", outputMono ? "ON" : "OFF");
            pane.push_back(lmRow);
            const bool hwConnected = hw.isOpen() && hw.connected();
            const std::optional<double> hwMon = hwConnected ? hw.monitorDb() : std::nullopt;
            const std::optional<double> hwHp = hwConnected ? hw.headphoneDb() : std::nullopt;
            char hwHead[160]{};
            std::snprintf(hwHead, sizeof(hwHead), "HARDWARE VOLUME CONTROL  %s", hwMode
                              ? "[editing iD14 device volume - 'h' exits]"
                              : (hwConnected
                                     ? "[press 'h' to control]"
                                     : "[iD14 not connected]"));
            pane.push_back(hwHead);
            const char* hwMonSel = (hwMode && !hwSelHeadphones) ? ">" : " ";
            const char* hwHpSel = (hwMode && hwSelHeadphones) ? ">" : " ";
            char hwMonRow[160]{};
            std::snprintf(hwMonRow, sizeof(hwMonRow), "  [%s] iD14 Monitor     %s dB%s", hwMonSel,
                          fmtOptDb(hwMon).c_str(), (hwMode && !hwSelHeadphones) ? "  <--" : "");
            pane.push_back(hwMonRow);
            char hwHpRow[160]{};
            std::snprintf(hwHpRow, sizeof(hwHpRow), "  [%s] iD14 Headphones  %s dB%s", hwHpSel,
                          fmtOptDb(hwHp).c_str(), (hwMode && hwSelHeadphones) ? "  <--" : "");
            pane.push_back(hwHpRow);
            if (hw.isOpen())
            {
                char hwDiag[160]{};
                std::snprintf(hwDiag, sizeof(hwDiag), "  poll %u ms | last %.2f ms | ticks %llu",
                              hw.pollPeriodMs(), hw.lastPollMs(),
                              static_cast<unsigned long long>(hw.pollTicks()));
                pane.push_back(hwDiag);
            }
            char virt[256]{};
            std::snprintf(virt, sizeof(virt), "Virtual   %s | backlog %.1f ms | endpoint consumer %s",
                          driverText.c_str(), ringMs, consumerActive ? "ACTIVE" : "NONE");
            pane.push_back(virt);
            char health[256]{};
            std::snprintf(health, sizeof(health), "Health    xruns %llu | ovl %llu | overflow %llu | stale %llu | rejected %llu",
                          static_cast<unsigned long long>(snap.xruns),
                          static_cast<unsigned long long>(snap.overloads),
                          static_cast<unsigned long long>(gotDriverState ? st.overflowDrops : 0),
                          static_cast<unsigned long long>(gotDriverState ? st.staleCatchupDrops : 0),
                          static_cast<unsigned long long>(f.rejectedBlocks));
            pane.push_back(health);
            char device[256]{};
            std::snprintf(device, sizeof(device), "Device    %s | losses %llu | recoveries %llu | attempts %llu",
                          physLost ? "LOST" : "STREAMING",
                          static_cast<unsigned long long>(recovery.lostCount()),
                          static_cast<unsigned long long>(recovery.recoveryCount()),
                          static_cast<unsigned long long>(recovery.failedAttempts()));
            pane.push_back(device);
            pane.push_back("----------------------------------------------------------------");
            char hint[256]{};
            if (outputMode)
            {
                std::snprintf(hint, sizeof(hint),
                              "LOCAL OUTPUT: m/1 Mic->Speakers  h/2 Mic->Headphones  +/- or Up/Down level  s mute  d dim  r mic-route  o exit  q quit | "
                              "ring %.1f ms  transport %.1f ms",
                              ringMs, transportMs);
            }
            else if (hwMode)
            {
                std::snprintf(hint, sizeof(hint),
                              "HARDWARE: 1 iD14 Monitor  2 iD14 Headphones  +/- or Up/Down +/-1 dB  h exit  q quit | "
                              "ring %.1f ms  transport %.1f ms",
                              ringMs, transportMs);
            }
            else
            {
                char monoHint[48]{};
                std::snprintf(monoHint, sizeof(monoHint), " k mono[%s]", outputMono ? "ON " : "OFF");
                std::snprintf(hint, sizeof(hint),
                              "keys: 1-9 sel  a add  e edit  m mon  +/- gain%s  h hw  k mono  : focus  [/]dB  ; mute  ' en  q quit | "
                              "%.1f/%.1fms",
                              monoHint, ringMs, transportMs);
            }
            pane.push_back(hint);
            tui.setPaneRows(pane.size());
            tui.render(pane);
        }
        else
        {
            // Legacy automation status line (exact historical format).
            const double micDb = snap.micDb;

            // Ring diagnostics (only computed for the appended line).
            double ringRms = 0.0;
            float ringMin = 0.0f;
            float ringMax = 0.0f;
            std::uint64_t ringNonFinite = 0;
            if (header != nullptr && header->capacityFrames > 0)
            {
                const std::size_t capacity = header->capacityFrames;
                const std::uint64_t write = header->writePos;
                const std::uint64_t read = header->readPos;
                const std::uint64_t available = write > read ? write - read : 0;
                const std::size_t scan = static_cast<std::size_t>(available < capacity ? available : capacity);
                const float* samples = reinterpret_cast<const float*>(
                    reinterpret_cast<const unsigned char*>(header) + header->headerBytes);
                const std::uint64_t firstIndex = (write - scan) & (capacity - 1);
                double sum2 = 0.0;
                float mn = 1.0f;
                float mx = -1.0f;
                std::size_t counted = 0;
                for (std::size_t i = 0; i < scan; ++i)
                {
                    const float v = samples[(firstIndex + i) & (capacity - 1)];
                    if (!std::isfinite(v))
                    {
                        ++ringNonFinite;
                        continue;
                    }
                    ++counted;
                    sum2 += static_cast<double>(v) * static_cast<double>(v);
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
                ringRms = counted > 0 ? std::sqrt(sum2 / static_cast<double>(counted)) : 0.0;
                ringMin = counted > 0 ? mn : 0.0f;
                ringMax = counted > 0 ? mx : 0.0f;
            }

            if (dualMode)
            {
                std::printf("[dual] ch1 RAW %.1fdBFS POST %.1fdBFS | ch2 RAW %.1fdBFS POST %.1fdBFS  (plan %u in, cfg %zu, vm=%s)\n",
                            snap.ch1RawPeak > 0.0001f ? 20.0 * std::log10(snap.ch1RawPeak) : -120.0,
                            snap.ch1PostPeak > 0.0001f ? 20.0 * std::log10(snap.ch1PostPeak) : -120.0,
                            snap.ch2RawPeak > 0.0001f ? 20.0 * std::log10(snap.ch2RawPeak) : -120.0,
                            snap.ch2PostPeak > 0.0001f ? 20.0 * std::log10(snap.ch2PostPeak) : -120.0,
                            snap.inputCount, snap.configuredInputs,
                            prefs.virtualMicSource == audient::preferences::kVirtualMicSourceInput2 ? "ch2" : "ch1");
            }
            std::printf(
                "[status] asio=%s rate=48000 reqbuf=%ld actbuf=%ld estbuf=%ld cb=%llu xruns=%llu ovl=%llu last=%.3fms p95=%.3fms "
                "mic=%.1fdBFS fade=%s monitor=%s "
                "| feeder gen=%llu pulls=%llu pushed=%llu rej=%llu gaps=%llu nosink=%llu "
                "| ring rms=%.4f min=%.4f max=%.4f nonFin=%llu "
                "| driver %s gen=%llu prod=%llu cons=%llu ovfl=%llu stale=%llu underruns=%llu"
                " | device phys=%s rec=%llu lost=%llu reconfig=%llu att=%llu",
                snap.stateText.c_str(), snap.buffer, actualBuffer, estimatedBuffer,
                static_cast<unsigned long long>(snap.callbacks),
                static_cast<unsigned long long>(snap.xruns),
                static_cast<unsigned long long>(snap.overloads),
                snap.lastMs, snap.p95Ms,
                static_cast<double>(micDb),
                snap.fadeText.c_str(),
                monitorMuted ? "OFF" : "ON",
                static_cast<unsigned long long>(f.generation),
                static_cast<unsigned long long>(f.pulls),
                static_cast<unsigned long long>(f.pushedBlocks),
                static_cast<unsigned long long>(f.rejectedBlocks),
                static_cast<unsigned long long>(f.engineGaps),
                static_cast<unsigned long long>(f.noSinkTicks),
                ringRms, static_cast<double>(ringMin), static_cast<double>(ringMax),
                static_cast<unsigned long long>(ringNonFinite),
                gotDriverState ? (st.connected ? "connected" : "disconnected") : (appOnlyMode ? "app-only" : "offline"),
                static_cast<unsigned long long>(gotDriverState ? st.generation : 0),
                static_cast<unsigned long long>(gotDriverState ? st.producedSamples : 0),
                static_cast<unsigned long long>(gotDriverState ? st.consumedSamples : 0),
                static_cast<unsigned long long>(gotDriverState ? st.overflowDrops : 0),
                static_cast<unsigned long long>(gotDriverState ? st.staleCatchupDrops : 0),
                static_cast<unsigned long long>(gotDriverState ? st.underruns : 0),
                physLost ? "LOST" : "streaming",
                static_cast<unsigned long long>(recovery.recoveryCount()),
                static_cast<unsigned long long>(recovery.lostCount()),
                static_cast<unsigned long long>(recovery.reconfigureCount()),
                static_cast<unsigned long long>(recovery.failedAttempts()));
            std::printf(" | ringBacklog=%llu (%.2f ms) transportBacklog=%llu (%.2f ms)",
                        static_cast<unsigned long long>(ringBacklog),
                        static_cast<double>(ringBacklog) * 1000.0 / 48000.0,
                        static_cast<unsigned long long>(transportBacklog),
                        static_cast<double>(transportBacklog) * 1000.0 / 48000.0);
            if (recorder != nullptr)
            {
                std::printf(" | wav A=%llu/%llu B=%llu/%llu C=%llu/%llu",
                            static_cast<unsigned long long>(recorder->a()->recorded()),
                            static_cast<unsigned long long>(recorder->a()->dropped()),
                            static_cast<unsigned long long>(recorder->b()->recorded()),
                            static_cast<unsigned long long>(recorder->b()->dropped()),
                            static_cast<unsigned long long>(recorder->c()->recorded()),
                            static_cast<unsigned long long>(recorder->c()->dropped()));
            }
            std::printf("\n");
            std::fflush(stdout);

            if (dailyTrayMode && dailyTray.hwnd() != nullptr)
            {
                wchar_t tip[160]{};
                swprintf_s(tip, L"Audient Console\n48 kHz / %ld samples\nASIO %hs",
                           asioStream.currentBuffer(), snap.stateText.c_str());
                dailyTray.setTooltip(tip);
            }

            // One de-pop startup report per fresh session.
            const std::uint64_t recNow = recovery.recoveryCount();
            if (recNow != lastRecoveryForDepop)
            {
                lastRecoveryForDepop = recNow;
                depopLogged = false;
            }
            if (!depopLogged)
            {
                audient::asio::AsioRoutingAdapter::DepopSnapshot d;
                if (asioStream.depopSnapshot(d) && d.published)
                {
                    std::printf("[audio-depop] start buffer=%ld firstSample=(%.6f,%.6f) maxAbs=%.6f "
                                "maxJump=%.6f muted=%zu ramp=%zu active=%d\n",
                                asioStream.currentBuffer(), d.firstLeft, d.firstRight, d.maxAbs,
                                d.maxJump, d.mutedSamples, d.rampSamples, d.active ? 1 : 0);
                    depopLogged = true;
                }
            }
        }
        if (useGui && !isTui)
        {
            // Headless/redirected run with the GUI open: wait out the status
            // cadence while CONTINUOUSLY pumping the GUI message queue, so the
            // WebView/Win32 STA never stalls just because stdout is redirected.
            const auto until = std::chrono::steady_clock::now() + tickCadence;
            while (!g_stop.load(std::memory_order_relaxed) && !quit.load(std::memory_order_relaxed) &&
                   std::chrono::steady_clock::now() < until)
            {
                MSG pumpMessage{};
                while (PeekMessageW(&pumpMessage, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&pumpMessage);
                    DispatchMessageW(&pumpMessage);
                }
                if (editorHostState.wantClose)
                {
                    editorHostState.wantClose = false;
                    closeEditor();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        else
        {
            std::this_thread::sleep_for(isTui ? std::chrono::milliseconds(15) : tickCadence);
        }
    }

    closeEditor();

    dailyTray.destroy(); // remove the tray icon before the window/engine teardown

    SetConsoleCtrlHandler(consoleBreakHandler, FALSE);
    if (tui.active())
    {
        tui.end();
    }

    // --- 11. Clean teardown ------------------------------------------------------
    if (g_teardownStarted.exchange(true))
    {
        return 0;
    }
    std::printf("\n== teardown ==\n");

    // Stop the watchdog FIRST; the graceful teardown below must not race the
    // recovery thread (it is the only other session owner).
    recovery.requestStop();
    watchdogThread.join();
    std::printf("watchdog stopped\n");

    if (paramThread.joinable())
    {
        paramThread.join();
    }

    if (keyFeeder.joinable())
    {
        keyFeeder.join();
    }

    if (identifyOutputArmed)
    {
        asioStream.disarmOutputTone();
        std::printf("[identify] tone disarmed\n");
    }

    std::printf("fade-out + ASIO stop...\n");
    asioStream.gracefulTeardown();

    // Persist the final clean mixer state (audio stopped, plug-ins still alive
    // and no longer processed) before any plug-in teardown begins.
    std::printf("mixer-state save...\n");
    saveMixerState();

    std::printf("feeder stop (worker join) ...\n");
    feeder.detachSink();
    feeder.stop();

    if (picoWorker)
    {
        std::printf("pico transport stop (worker join) ...\n");
        picoWorker->stop();
        const audient::pico::PicoUsbWorker::Status ps = picoWorker->status();
        std::printf("[pico] final: open=%d opens=%llu failed=%llu reconnects=%llu deviceLost=%llu "
                    "submits=%llu rendered=%llu silence=%llu underruns=%llu err=\"%s\"\n",
                    ps.deviceOpen ? 1 : 0,
                    static_cast<unsigned long long>(ps.opens),
                    static_cast<unsigned long long>(ps.failedOpens),
                    static_cast<unsigned long long>(ps.reconnects),
                    static_cast<unsigned long long>(ps.deviceLost),
                    static_cast<unsigned long long>(ps.submitCalls),
                    static_cast<unsigned long long>(ps.renderedFrames),
                    static_cast<unsigned long long>(ps.silenceFrames),
                    static_cast<unsigned long long>(ps.underruns),
                    ps.lastError.c_str());
        picoWorker.reset();
    }

    if (recorder != nullptr)
    {
        std::printf("finalizing WAV captures...\n");
        recorder->stopAndFinalize();
    }

    // Stop the persistent capture client before the control-plane flush so the
    // engine releases the endpoint stream first.
    holdClient.reset();

    if (!appOnlyMode)
    {
        const bool flushed = client.flush();
        audient::capture_control::CaptureControlState afterFlush{};
        const bool gotFlushState = client.queryState(&afterFlush);
        const bool disconnected = client.disconnect();
        audient::capture_control::CaptureControlState afterDisconnect{};
        const bool gotDisconnectState = client.queryState(&afterDisconnect);
        std::printf("driver: flush=%s gen=%llu disconnect=%s connected=%d\n",
                    flushed ? "OK" : "FAIL",
                    gotFlushState ? static_cast<unsigned long long>(afterFlush.generation) : 0ull,
                    disconnected ? "OK" : "FAIL",
                    gotDisconnectState ? (afterDisconnect.connected ? 1 : 0) : -1);
    }
    else
    {
        std::printf("driver: not connected (app-only) - no control-plane teardown needed\n");
    }

    asioStream.dispose();
    std::printf("hardware control close...\n");
    hw.close();
    pcMeter.stop();
    UnmapViewOfFile(view);
    CloseHandle(section);
    timeEndPeriod(1);

    const audient::virtual_audio::VirtualMicFeeder::Snapshot finalFeeder = feeder.snapshot();
    const DemoAsioStream::Status finalSnap = asioStream.snapshotStatus();
    std::printf("final: asio callbacks=%llu xruns=%llu overloads=%llu "
                "| feeder pulled=%llu pushed=%llu rejected=%llu gaps=%llu "
                "| device losses=%llu recoveries=%llu reconfigures=%llu failed-attempts=%llu\n",
                static_cast<unsigned long long>(finalSnap.callbacks),
                static_cast<unsigned long long>(finalSnap.xruns),
                static_cast<unsigned long long>(finalSnap.overloads),
                static_cast<unsigned long long>(finalFeeder.pulls),
                static_cast<unsigned long long>(finalFeeder.pushedBlocks),
                static_cast<unsigned long long>(finalFeeder.rejectedBlocks),
                static_cast<unsigned long long>(finalFeeder.engineGaps),
                static_cast<unsigned long long>(recovery.lostCount()),
                static_cast<unsigned long long>(recovery.recoveryCount()),
                static_cast<unsigned long long>(recovery.reconfigureCount()),
                static_cast<unsigned long long>(recovery.failedAttempts()));
    std::printf("RESULT: %s\n", watchdogThread.joinable() ? "CHECK" : "PASS");
    return 0;
}