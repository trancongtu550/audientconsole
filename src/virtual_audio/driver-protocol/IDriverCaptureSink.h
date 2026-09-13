#pragma once

#include <cstddef>
#include <cstdint>

namespace audient::virtual_audio
{

// Slice Q3 — the driver capture sink abstraction: the OS-side capture buffer a
// Windows virtual microphone driver bridge consumes. A driver bridge (Phase 8)
// or the in-tree MockDriverCaptureSink implements it. The application pushes
// processed mic blocks into it from the VirtualMicFeeder WORKER thread — never
// from the ASIO callback.
//
// Protocol invariants (shared with Q1/Q2 freshness + AGENTS §11):
//  - Bounded memory: a stalled OS client can never grow the sink; a full
//    internal buffer REJECTS the write instead of blocking (counted).
//  - No stale replay: the sink flushes any buffered audio when the feeder
//    generation changes (attach/reconnect epoch), so an OS client that resumes
//    after a reconnect never hears pre-reconnect backlog.
//  - Silence: when the sink is empty its read side (implemented by the sink,
//    e.g. MockDriverCaptureSink::readMono) serves digital silence.
//  - Sequence: blocks carry the feeder's per-attach monotonic attempt index so
//    the sink/diagnostics can detect gaps caused by rejected writes.
class IDriverCaptureSink
{
public:
    virtual ~IDriverCaptureSink() = default;

    // True while the OS driver/endpoint side is present. When false,
    // writeMono() must reject (never block, never grow).
    virtual bool connected() const = 0;

    virtual std::size_t capacityFrames() const = 0;
    virtual std::size_t availableFrames() const = 0;

    // Push one processed-mic mono block. Returns true when accepted. Rejected
    // when the sink is disconnected or its bounded buffer is full (stall). The
    // feeder's generation is the attach/reconnect epoch; the sink flushes stale
    // data from previous epochs on change. Never blocks, never throws.
    virtual bool writeMono(const float* mono, std::size_t frames,
                           std::uint64_t generation, std::uint64_t sequence) = 0;
};

} // namespace audient::virtual_audio