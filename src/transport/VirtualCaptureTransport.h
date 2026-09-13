#pragma once

#include "engine/RingBuffer.h"
#include "transport/TransportFormat.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audient::transport
{

// Slice Q1 — the virtual capture transport: the user-space seam that carries
// the processed mic uplink from the routing core to a future Windows Virtual
// Mic backend.
//
//   RoutingCore::MicUplink -----------------------------------------------+
//        |                                                    (mono,      |
//        v                                                     processed)  |
//   engine adapter (AsioRoutingAdapter)                        writes here  |
//        | writeProcessedMic()  [realtime, SPSC producer]       +----------+
//        v                                                      v
//   VirtualCaptureTransport  <============================= ring buffer
//        ^
//        | readProcessedMic()                  [plain, low-level read]
//        | readProcessedMicFresh()             [freshness-aware read]
//   future Windows Virtual Mic backend / test harness
//
// Backend-agnostic on purpose (AGENTS §1/§11): no Windows virtual-device code
// and no Discord-specific code live here or in src/routing. The transport is
// the single bounded, realtime-safe boundary the future endpoint binds to.
//
// Capture/uplink contract (AGENTS §11):
//  - Mono processed-mic capture only (channels == 1).
//  - The producer never blocks and the ring has fixed capacity, so a lagging
//    capture client can never stall the engine or grow memory unboundedly.
//    Producer policy is DROP-NEW (Design B): a full ring REJECTS the new block
//    and counts the rejected frames in overflowDrops. The producer never
//    advances the consumer read head.
//  - Freshness policy (live-mic requirement: never serve a stale backlog after
//    a consumer recovers): the consumer read API has a freshness mode.
//    readProcessedMicFresh(frames, staleThresholdFrames) first checks the
//    buffered backlog; when backlog > staleThresholdFrames the consumer
//    DISCARDS the stale complete blocks and resynchronizes its OWN read head at
//    the write frontier (consumer-side discard — the producer is never
//    involved and never blocks). The discarded frames are counted in
//    staleCatchupDrops. After resynchronization the consumer serves ONLY blocks
//    produced afterwards, so no pre-recovery audio is ever delivered. Within
//    the threshold the buffered data is treated as normal in-flight latency and
//    served. A read that finds fewer than `frames` samples buffered fails and
//    counts underruns; the backend then emits digital silence (AGENTS §11).
//  - Separate counters, never a silent drop: overflowDrops (producer reject),
//    staleCatchupDrops (consumer resync discard), underruns (empty reads),
//    plus produced/consumed. All observable via stats().
//  - reset() is the control-plane "fresh capture epoch": it clears data and
//    counters so the next read fails until a NEW producer block arrives. Used
//    on stream start / device reconnect so stale or uninitialized samples are
//    never served.
//  - Independent from the downlink: this class only carries what the engine
//    publishes; it has no way to inject render audio.
//
// Realtime contract: writeProcessedMic()/readProcessedMic()/
// readProcessedMicFresh() allocate nothing, lock nothing, sleep nothing, and
// never throw (same SPSC ring as the UplinkTransport/DownlinkTransport pair).
// The producer path performs a single bounded ring write; the freshness path
// is consumer-side and performs a single bounded discard.
class VirtualCaptureTransport
{
public:
    struct CaptureStatistics
    {
        std::uint64_t produced = 0;          // committed producer samples (accepted)
        std::uint64_t consumed = 0;          // read-head position (served + discarded)
        std::uint64_t overflowDrops = 0;     // producer rejected a full ring (drop-new)
        std::uint64_t staleCatchupDrops = 0; // consumer discarded stale backlog frames
        std::uint64_t underruns = 0;         // underrun frames (requested but not buffered)
    };

    VirtualCaptureTransport(Format format, std::size_t capacityFrames);

    Format format() const;
    std::size_t capacityFrames() const;
    std::size_t availableFrames() const;

    // Realtime producer (single writer). Returns false when the block is
    // rejected on overflow (counted in overflowDrops); the caller continues.
    bool writeProcessedMic(const float* mono, std::size_t frames);

    // Realtime consumer (single reader), low-level: reads if enough committed
    // samples are buffered, otherwise fails (counted in underruns).
    bool readProcessedMic(float* mono, std::size_t frames);

    // Realtime consumer (single reader), freshness-aware: if the buffered
    // backlog exceeds staleThresholdFrames, discards the stale complete blocks
    // and resynchronizes at the write frontier (counted in staleCatchupDrops)
    // so a stalled consumer never serves pre-recovery audio; then reads
    // normally within the threshold. Backends for the live mic endpoint MUST
    // use this read (or enforce the same policy themselves).
    bool readProcessedMicFresh(float* mono, std::size_t frames, std::size_t staleThresholdFrames);

    // Monotonic accepted/consumed block counters. A frozen producerSequence()
    // tells the backend "the engine stopped producing", which it interprets
    // as silence (AGENTS §11) rather than replay.
    std::uint64_t producerSequence() const;
    std::uint64_t consumerSequence() const;

    CaptureStatistics stats() const;

    // Control plane only (never from the audio callback): clear data and
    // counters so the next read fails until a fresh block is produced.
    void reset();

private:
    Format m_format;
    engine::LockFreeRingBuffer m_ring;
    std::atomic<std::uint64_t> m_producerSeq{0};
    std::atomic<std::uint64_t> m_consumerSeq{0};
    std::atomic<std::uint64_t> m_staleCatchupDrops{0};
};

} // namespace audient::transport