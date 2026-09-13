#pragma once

#include "pico/PicoRenderDevice.h"
#include "pico/PicoVirtualMicSink.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audient::pico
{

struct PicoWorkerConfig
{
    // Backoff between discovery attempts while the Pico is absent.
    std::chrono::milliseconds reconnectBackoff{500};
    // Freshness threshold handed to the sink: a stalled backlog larger than this
    // is discarded rather than sent late. Must exceed the largest ASIO buffer
    // (1024) so a legitimate full-buffer publish is never mistaken for stale
    // backlog; 2048 frames == 42.7 ms.
    std::size_t staleThresholdFrames = 2048;
    // The worker drains the sink in bounded chunks (the feeder block size) so
    // the ASIO callback block size is irrelevant to the Pico transport cadence.
    std::size_t readChunkFrames = 64;
    // Upper bound on frames rendered per step (scratch buffer capacity).
    std::size_t maxRenderFrames = 4096;
    // Emit connection/diagnostic lines on stdout (disabled in tests).
    bool log = true;
};

// PicoUsbWorker — the USB/render side of the virtual-mic transport.
//
// Runs on its OWN normal-priority worker thread and owns the render device
// (WASAPI COM/device lifetime) plus the sink->device block adaptation:
//
//   VirtualMicFeeder (bounded SPSC) -> PicoVirtualMicSink
//       -> PicoUsbWorker.stepOnce()  [pull fresh mono, interleave L=R]
//       -> IPicoRenderDevice.submit() -> WASAPI -> RP2040 UAC2 loopback
//
// Hard rules (AGENTS §7/§11):
//  - never invoked by the ASIO realtime callback; the realtime side only
//    publishes into the sink's bounded ring;
//  - the worker never blocks the engine: the ring drops-new on overflow and the
//    worker synthesizes exact digital silence on underrun (never replays);
//  - a Pico disconnect only marks this transport disconnected; ASIO/VST/Local
//    Monitor/GUI are untouched and no AsioRecoveryController path is involved;
//  - reconnect flushes the stale epoch and resumes, with NO ASIO restart.
class PicoUsbWorker
{
public:
    PicoUsbWorker(PicoVirtualMicSink& sink, std::unique_ptr<IPicoRenderDevice> device,
                  PicoWorkerConfig config = {});
    ~PicoUsbWorker();

    PicoUsbWorker(const PicoUsbWorker&) = delete;
    PicoUsbWorker& operator=(const PicoUsbWorker&) = delete;

    void start();
    void requestStop();
    void stop();
    bool running() const;

    struct Status
    {
        bool deviceOpen = false;
        std::string deviceName;
        std::string lastError;
        std::uint64_t opens = 0;
        std::uint64_t failedOpens = 0;
        std::uint64_t reconnects = 0;
        std::uint64_t deviceLost = 0;
        std::uint64_t submitCalls = 0;
        std::uint64_t renderedFrames = 0;
        std::uint64_t silenceFrames = 0;
        std::uint64_t underruns = 0;
    };
    Status status() const;

    // One deterministic worker iteration (does not sleep): ensures the device is
    // open, drains the sink with the freshness policy, interleaves mono to
    // L=R, and submits. Returns true when frames were committed. Public so tests
    // and the run() loop drive the exact same code path.
    bool stepOnce();

private:
    void run();
    void closeDevice();

    PicoVirtualMicSink& m_sink;
    std::unique_ptr<IPicoRenderDevice> m_device;
    PicoWorkerConfig m_config;

    std::size_t m_maxFrames = 1;
    std::vector<float> m_mono;
    std::vector<float> m_stereo;

    std::atomic<bool> m_running{false};
    std::thread m_thread;

    // Fast path counters (worker writes, control/GUI reads).
    std::atomic<bool> m_deviceOpen{false};
    std::atomic<std::uint64_t> m_opens{0};
    std::atomic<std::uint64_t> m_failedOpens{0};
    std::atomic<std::uint64_t> m_reconnects{0};
    std::atomic<std::uint64_t> m_deviceLost{0};
    std::atomic<std::uint64_t> m_submitCalls{0};
    std::atomic<std::uint64_t> m_renderedFrames{0};
    std::atomic<std::uint64_t> m_silenceFrames{0};
    std::atomic<std::uint64_t> m_underruns{0};

    // Strings are only touched on open/close (worker) and status() (GUI thread).
    mutable std::mutex m_statusMutex;
    std::string m_deviceName;
    std::string m_lastError;
};

} // namespace audient::pico
