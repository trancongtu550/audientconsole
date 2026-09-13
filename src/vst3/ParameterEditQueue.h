#pragma once

#include "pluginterfaces/vst/vsttypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace audient::vst3
{

// Parameter edits that flow from the UI/controller thread to the audio
// callback: a bounded single-producer/single-consumer lock-free ring.
//
// Contract (project guidelines §7, VST-009):
//  - producer: exactly one UI/controller thread that calls push() (the demo
//    editor's IComponentHandler::performEdit or host/session code). Never the
//    audio callback.
//  - consumer: exactly one audio callback thread that calls drain(). Nothing
//    in drain() allocates, locks, blocks, or grows.
//  - when full, push() drops the new edit and increments dropped() — a defined
//    overflow policy (latest-edit-dropped, bounded latency); no silent replay.
//  - positions increase monotonically; a slot is written only after the
//    consumer has consumed it, so no slot is ever wrapped while in flight.
class ParameterEditQueue
{
public:
    struct Edit
    {
        Steinberg::Vst::ParamID id = Steinberg::Vst::kNoParamId;
        Steinberg::Vst::ParamValue value = 0.0;
    };

    explicit ParameterEditQueue(std::size_t capacity = kDefaultCapacity);

    // Non-blocking, thread-safe on the single producer thread.
    bool push(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);

    // Drains up to maxEdits edits into out, oldest first. Realtime-safe:
    // no allocation, no lock, no block.
    std::size_t drain(Edit* out, std::size_t maxEdits);

    std::size_t capacity() const { return m_capacity; }
    std::size_t pending() const;

    // Relaxed counters for diagnostics/tests.
    std::uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }
    std::uint64_t pushed() const { return m_pushed.load(std::memory_order_relaxed); }

    void reset();

    static constexpr std::size_t kDefaultCapacity = 256;

private:
    std::size_t m_capacity = 0;
    std::size_t m_mask = 0;
    std::unique_ptr<Edit[]> m_memory;

    alignas(64) std::atomic<std::size_t> m_head{0}; // consumer position
    std::atomic<std::size_t> m_tail{0};             // producer position
    std::atomic<std::uint64_t> m_dropped{0};
    std::atomic<std::uint64_t> m_pushed{0};
};

} // namespace audient::vst3

#if defined(_MSC_VER)
#pragma warning(pop)
#endif