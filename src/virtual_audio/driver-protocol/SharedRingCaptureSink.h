#pragma once

#include "virtual_audio/driver-protocol/IDriverCaptureSink.h"
#include "virtual_audio/driver-protocol/CaptureRingContract.h"
#include "transport/TransportFormat.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audient::virtual_audio
{

// Q5-A3 — a concrete IDriverCaptureSink backed by the versioned shared capture
// ring (CaptureRingContract.h). This is the producer half of the app->driver
// capture transport: VirtualMicFeeder -> SharedRingCaptureSink -> capture ring
// region -> (future) kernel capture source -> WaveRT capture buffer.
//
// It is deliberately a drop-in behavioral equivalent of MockDriverCaptureSink
// for the feeder-facing surface (connected / capacityFrames / availableFrames /
// writeMono / setConnected / readMono / snapshot) so the existing
// VirtualMicFeederProtocolTest runs unchanged against it — proving the concrete
// transport sink preserves the Q3 protocol (bounded drop-new, freshness,
// generation flush, counters) that the mock establishes.
//
// Region model (Q5-A3 region abstraction):
//  - A "region" is a caller-provided allocation laid out by the contract header
//    (CaptureRingRegionBytes(capacityFrames) bytes, aligned). In this slice no
//    driver is installed, so tests back the sink with a user-mode region that
//    stands in for the future kernel-mapped shared memory.
//  - SharedRingCaptureSink MAY own its region (constructor allocates and
//    initializes one) or adopt an externally initialized region (used when the
//    memory is provided by the mapping host). See the two constructors.
//
// Realtime/lifecycle rules (AGENTS §7/§9): writeMono/readMono allocate nothing,
// lock nothing, sleep nothing, and never throw. attach/detach (setConnected)
// and construction/destruction are control-plane only. Like the mock, a
// generation flush is a control-plane event: it must not race with a concurrent
// readMono (single-writer control plane — same documented constraint).
class SharedRingCaptureSink final : public IDriverCaptureSink
{
public:
    // Owns an internal, aligned user-mode region of `capacityFrames`.
    SharedRingCaptureSink(transport::Format format, std::size_t capacityFrames);

    // Adopts an externally initialized region (contract-validated in ctor).
    // `region` must point at a CaptureRingHeader with the requested format.
    SharedRingCaptureSink(transport::Format format, void* region, std::uint64_t regionBytes);

    ~SharedRingCaptureSink() override;

    // IDriverCaptureSink ----------------------------------------------------
    bool connected() const override;
    std::size_t capacityFrames() const override;
    std::size_t availableFrames() const override;
    bool writeMono(const float* mono, std::size_t frames, std::uint64_t generation,
                   std::uint64_t sequence) override;

    // Test/app control: simulate the OS driver/endpoint detaching/attaching.
    void setConnected(bool connected);

    // OS capture client read side (single reader) over the SAME shared region a
    // driver consumer would read. Freshness-aware (Q1 policy): a stalled
    // backlog larger than `staleThresholdFrames` is discarded (whole blocks)
    // and the read resynchronizes before serving. Returns true when the full
    // block was served FRESH.
    bool readMono(float* mono, std::size_t frames, std::size_t staleThresholdFrames);

    // Consumer/DMA-side mono -> L/R float fill over the region (same function
    // the kernel capture source calls). Returns fresh frames served; shortfall
    // is exact digital silence.
    std::uint32_t fillStereo(float* stereo, std::uint32_t frames, std::uint32_t staleThresholdFrames);

    // The adopted/allocation base (CaptureRingHeader*). For tests that want to
    // act as the mock kernel on the same memory.
    capture_ring::CaptureRingHeader* region();

    struct Snapshot
    {
        std::uint64_t lastGeneration = 0;
        std::uint64_t lastAcceptedSequence = 0;
        std::uint64_t acceptedWrites = 0;
        std::uint64_t rejectedDisconnected = 0;
        std::uint64_t rejectedStall = 0; // ring full while a client is not reading
        std::uint64_t generationFlushes = 0;
        std::uint64_t producedSamples = 0;
        std::uint64_t consumedSamples = 0;
        std::uint64_t staleCatchupDrops = 0;
        std::uint64_t underruns = 0;
    };
    Snapshot snapshot() const;

    SharedRingCaptureSink(const SharedRingCaptureSink&) = delete;
    SharedRingCaptureSink& operator=(const SharedRingCaptureSink&) = delete;

private:
    transport::Format m_format;
    void* m_ownedRegion; // non-null when this object allocated the region
    capture_ring::CaptureRingHeader* m_header; // always points at the live region
    std::uint64_t m_regionBytes;

    std::atomic<bool> m_connected{true};
    std::atomic<std::uint64_t> m_lastGeneration{0};
    std::atomic<std::uint64_t> m_lastAcceptedSequence{0};
    std::atomic<std::uint64_t> m_acceptedWrites{0};
    std::atomic<std::uint64_t> m_rejectedDisconnected{0};
    std::atomic<std::uint64_t> m_rejectedStall{0};
};

} // namespace audient::virtual_audio
