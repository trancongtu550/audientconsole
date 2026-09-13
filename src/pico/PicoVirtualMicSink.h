#pragma once

#include "virtual_audio/driver-protocol/IDriverCaptureSink.h"

#include "transport/VirtualCaptureTransport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audient::pico
{

// Pico v0.1.1 downstream sink: the IDriverCaptureSink the existing
// VirtualMicFeeder pushes processed CH0 into, feeding the Pico USB transport.
//
// It is a bounded, drop-new, freshness-aware SPSC ring exactly like the driver
// sink (it reuses the tested transport::VirtualCaptureTransport), and shares the
// Q3 sink behavioral contract so the feeder protocol is unchanged:
//  - writeMono() is the feeder (RT-published) side: bounded, non-blocking, and
//    rejects when disconnected or the ring is full (never blocks ASIO);
//  - a generation change flushes the previous epoch (never replay stale PCM
//    after a Pico reconnect);
//  - the PicoUsbWorker drains the ring at its OWN cadence with the freshness
//    policy, so the Pico USB block size is INDEPENDENT of the ASIO buffer size.
//
// Pico presence NEVER controls the ASIO engine: when this sink is disconnected
// the fan-out still delivers to the driver sink, and the engine is unaffected.
class PicoVirtualMicSink final : public virtual_audio::IDriverCaptureSink
{
public:
    PicoVirtualMicSink(transport::Format format, std::size_t capacityFrames);

    bool connected() const override;
    std::size_t capacityFrames() const override;
    std::size_t availableFrames() const override;
    bool writeMono(const float* mono, std::size_t frames, std::uint64_t generation,
                   std::uint64_t sequence) override;

    // Worker/control side: mark the WASAPI endpoint present/absent. A change in
    // either direction opens a fresh epoch (the ring is flushed) so a reconnect
    // never replays pre-disconnect audio.
    void setConnected(bool connected);

    // PicoUsbWorker drain side (single reader). Freshness-aware: a stalled
    // backlog larger than staleThresholdFrames is discarded before serving.
    // Returns true when `frames` fresh samples were served.
    bool readMono(float* mono, std::size_t frames, std::size_t staleThresholdFrames);

    struct Snapshot
    {
        std::uint64_t lastGeneration = 0;
        std::uint64_t lastAcceptedSequence = 0;
        std::uint64_t acceptedWrites = 0;
        std::uint64_t rejectedDisconnected = 0;
        std::uint64_t rejectedStall = 0; // ring full while the worker is not draining
        std::uint64_t generationFlushes = 0;
        std::uint64_t producedSamples = 0;
        std::uint64_t consumedSamples = 0;
        std::uint64_t staleCatchupDrops = 0;
        std::uint64_t underruns = 0;
    };
    Snapshot snapshot() const;

    PicoVirtualMicSink(const PicoVirtualMicSink&) = delete;
    PicoVirtualMicSink& operator=(const PicoVirtualMicSink&) = delete;

private:
    transport::VirtualCaptureTransport m_ring;
    // Defaults connected so the sink honors the Q3 feeder contract by default;
    // PicoUsbWorker corrects this to false on its first step when no endpoint is
    // present (and toggles it on device loss/reconnect).
    std::atomic<bool> m_connected{true};
    std::atomic<std::uint64_t> m_lastGeneration{0};
    std::atomic<std::uint64_t> m_lastAcceptedSequence{0};
    std::atomic<std::uint64_t> m_acceptedWrites{0};
    std::atomic<std::uint64_t> m_rejectedDisconnected{0};
    std::atomic<std::uint64_t> m_rejectedStall{0};
    std::atomic<std::uint64_t> m_generationFlushes{0};
};

} // namespace audient::pico
