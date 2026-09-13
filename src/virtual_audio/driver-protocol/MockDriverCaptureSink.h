#pragma once

#include "virtual_audio/driver-protocol/IDriverCaptureSink.h"

#include "transport/VirtualCaptureTransport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audient::virtual_audio
{

// Slice Q3 — test double AND software fallback for the driver capture sink.
// Faithfully models the OS-side capture buffer a Phase-8 driver bridge will
// expose:
//  - a bounded, drop-new, freshness-aware SPSC ring (reuses the Q1 transport);
//  - feeder-attach GENERATION flush: when writeMono carries a new generation,
//    any buffered audio from the previous epoch is discarded (never served to
//    an OS client after reconnect);
//  - disconnect simulation: writeMono rejects while not connected;
//  - a client read side (readMono) that follows the Q1 freshness policy so a
//    stalled OS client recovering never hears a stale backlog.
//
// NOTE (control-plane sequencing): the generation flush calls the ring's
// control-plane reset; tests/an app must not read concurrently from another
// thread exactly at a generation change (the flush happens on the first
// written block of the new epoch, before the client reads again).
class MockDriverCaptureSink final : public IDriverCaptureSink
{
public:
    MockDriverCaptureSink(transport::Format format, std::size_t capacityFrames);

    bool connected() const override;
    std::size_t capacityFrames() const override;
    std::size_t availableFrames() const override;
    bool writeMono(const float* mono, std::size_t frames, std::uint64_t generation, std::uint64_t sequence) override;

    // Test control: simulate the OS driver/endpoint detaching/attaching.
    void setConnected(bool connected);

    // OS capture client read side (single reader). Freshness-aware: on a
    // stalled backlog larger than staleThresholdFrames it discards and
    // resynchronizes before reading. Returns true when fresh audio was served.
    bool readMono(float* mono, std::size_t frames, std::size_t staleThresholdFrames);

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

private:
    transport::VirtualCaptureTransport m_ring;
    std::atomic<bool> m_connected{true};
    std::atomic<std::uint64_t> m_lastGeneration{0};
    std::atomic<std::uint64_t> m_lastAcceptedSequence{0};
    std::atomic<std::uint64_t> m_acceptedWrites{0};
    std::atomic<std::uint64_t> m_rejectedDisconnected{0};
    std::atomic<std::uint64_t> m_rejectedStall{0};
    std::atomic<std::uint64_t> m_generationFlushes{0};
};

} // namespace audient::virtual_audio