#pragma once

#include "virtual_audio/driver-protocol/IDriverCaptureSink.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audient::virtual_audio
{

// Fan-out driver capture sink: forwards every processed-mic block from the
// VirtualMicFeeder to several independent sinks at once.
//
// This is how the accepted driver path (SharedRingCaptureSink) and the Pico
// transport (PicoVirtualMicSink) coexist without either controlling the other:
//  - a block is ACCEPTED when AT LEAST ONE sink accepts it (so a disconnected
//    Pico never causes the feeder to see a reject and never disturbs the driver
//    path, and vice-versa);
//  - connected() is true when any sink is connected; capacity/available report
//    the tightest bound across connected sinks;
//  - per-sink accept/reject are counted distinctly (never a silent drop).
//
// Control-plane only for add(): register all sinks before the feeder attaches.
// writeMono lives on the feeder worker thread and never blocks (each sink's
// writeMono is itself bounded/non-blocking).
class FanoutCaptureSink final : public IDriverCaptureSink
{
public:
    static constexpr std::size_t kMaxSinks = 4;

    // Control plane (before attach): register a sink. Excess sinks are ignored
    // (counted via snapshot().overflowRegistrations) rather than growing.
    void add(IDriverCaptureSink* sink);

    // IDriverCaptureSink ----------------------------------------------------
    bool connected() const override;
    std::size_t capacityFrames() const override;
    std::size_t availableFrames() const override;
    bool writeMono(const float* mono, std::size_t frames, std::uint64_t generation,
                   std::uint64_t sequence) override;

    struct Snapshot
    {
        std::size_t sinks = 0;
        std::uint64_t fanoutWrites = 0;
        std::uint64_t allRejected = 0; // no registered sink accepted the block
        std::uint64_t overflowRegistrations = 0;
        std::uint64_t acceptedBy[kMaxSinks] = {};
        std::uint64_t rejectedBy[kMaxSinks] = {};
    };
    Snapshot snapshot() const;

private:
    IDriverCaptureSink* m_sinks[kMaxSinks] = {};
    std::size_t m_count = 0;
    std::atomic<std::uint64_t> m_writes{0};
    std::atomic<std::uint64_t> m_allRejected{0};
    std::atomic<std::uint64_t> m_overflowRegistrations{0};
    std::atomic<std::uint64_t> m_acceptedBy[kMaxSinks] = {};
    std::atomic<std::uint64_t> m_rejectedBy[kMaxSinks] = {};
};

} // namespace audient::virtual_audio
