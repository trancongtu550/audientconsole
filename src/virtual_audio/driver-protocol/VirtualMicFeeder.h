#pragma once

#include "virtual_audio/driver-protocol/IDriverCaptureSink.h"
#include "virtual_audio/endpoint/VirtualCaptureEndpoint.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace audient::virtual_audio
{

// Slice Q3 — the driver feeder: pulls processed mic from the endpoint on a NORMAL
// worker thread and pushes it into an IDriverCaptureSink (the future matrix:
// SoftwareCaptureEndpoint -> VirtualMicFeeder -> IDriverCaptureSink -> driver).
//
// Hard rules:
//  - NEVER called from the ASIO realtime callback / routing path: the adapter
//    only publishes into the transport; this object and its sink live on the
//    app/worker side (enforced by construction; verified in integration tests
//    with a stalled/disconnected sink running while ASIO streams at 0 xruns).
//  - Bounded memory and no stale replay: attached only through a fresh-enough
//    endpoint (Q1/Q2 freshness policy); on attach the endpoint is RESET so the
//    feeder starts from the current engine output, and each attach bumps the
//    GENERATION the sink uses to flush old-epoch audio.
//  - Only real engine blocks are pushed: a tick that finds no fresh audio does
//    NOT invent a silence block (counted as an engine gap). The driver's own
//    empty-read rule (Q1) yields digital silence for OS clients when absent.
//  - A stalled or disconnected sink never blocks the feeder: writeMono is a
//    bounded, non-blocking accept/reject and rejects are counted.
//  - Shutdown-safe: requestStop()/stop() join the worker; the destructor joins
//    too. tick() is unit-testable without the thread.
class VirtualMicFeeder
{
public:
    VirtualMicFeeder(VirtualCaptureEndpoint& endpoint, std::size_t blockFrames);

    // Control plane (app thread): attach to a driver sink. When
    // `controlPlaneGeneration` is non-zero it is used AS the feeder epoch: the
    // app obtained it from the driver CONNECT (CaptureControlClient::connect) so
    // the app and the driver agree on the ring generation - the kernel's
    // CONNECT/FLUSH generations are preserved and the first write does NOT reset
    // the kernel-initialized epoch (no header clobber, see the Q5-A3B seam note
    // in project plan). When it is 0 (app-only or mock sinks) the feeder bumps its own
    // per-attach epoch exactly as before. Either way the endpoint is RESET so the
    // first pushed blocks are CURRENT engine output, and a (re)attach flushes any
    // old-epoch audio in the sink. The sink remains owned by the caller; detach
    // BEFORE the sink is destroyed (single-writer lifetime contract).
    void attachSink(IDriverCaptureSink* sink, std::uint64_t controlPlaneGeneration = 0);
    void detachSink();

    // Re-align the epoch with an updated driver generation (a control-plane FLUSH
    // or a re-CONNECT after reconnect) WITHOUT re-attaching. Resets the endpoint
    // so the next pushed block opens a fresh epoch at `generation`. Called on the
    // app/control thread after the driver reports the new generation. Ignored for
    // generation 0.
    void setControlPlaneGeneration(std::uint64_t generation);

    bool sinkAttached() const;
    std::uint64_t generation() const;

    // One deterministic worker step: pull one endpoint block; if it is fresh
    // engine audio, push it to the sink. Returns true when a block was pushed.
    // Never blocks.
    bool tick(std::size_t staleThresholdFrames);

    // Worker thread (normal priority). Must not be started from a realtime
    // thread. setStaleThresholdFrames before start.
    //
    // PACING (live-mic correctness): the worker must drain the realtime 48 kHz
    // stream (750 blocks/s). A fixed-interval poll is NOT reliable: Windows (and
    // hypervisors) coalesce short sleeps to the scheduler tick (>= ~1.5 ms even
    // after timeBeginPeriod(1)), which is coarser than the 1.33 ms block period
    // and starves the ring. run() therefore ADAPTS: while a realtime producer is
    // feeding, it drains with short yields (no sleep) so it always keeps up; only
    // when the endpoint is empty does it back off by `pollInterval` (default
    // 1 ms) to bound idle CPU. `pollInterval` is now the IDLE backoff, not the
    // streaming cadence.
    void setStaleThresholdFrames(std::size_t frames);
    void start(std::chrono::microseconds pollInterval = std::chrono::microseconds(1000));
    void requestStop();
    void stop();
    bool running() const;

    struct Snapshot
    {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0; // last ATTEMPTED block index since attach
        std::uint64_t pulls = 0;
        std::uint64_t pushedBlocks = 0;
        std::uint64_t rejectedBlocks = 0; // sink stall or disconnect
        std::uint64_t engineGaps = 0;     // ticks with no fresh engine block (never synthesized)
        std::uint64_t noSinkTicks = 0;
    };
    Snapshot snapshot() const;

    ~VirtualMicFeeder();

private:
    void run();

    VirtualCaptureEndpoint& m_endpoint;
    std::vector<float> m_block;
    std::atomic<IDriverCaptureSink*> m_sink{nullptr};
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::atomic<std::uint64_t> m_generation{0};
    std::atomic<std::uint64_t> m_sequence{0};
    std::chrono::microseconds m_pollInterval{1000};
    std::size_t m_staleThresholdFrames = 128;

    std::atomic<std::uint64_t> m_pulls{0};
    std::atomic<std::uint64_t> m_pushed{0};
    std::atomic<std::uint64_t> m_rejected{0};
    std::atomic<std::uint64_t> m_engineGaps{0};
    std::atomic<std::uint64_t> m_noSinkTicks{0};
};

} // namespace audient::virtual_audio