#pragma once

#include "vst3/Vst3Processor.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace audient::vst3
{

// Realtime-clean chain of Vst3Processors (mono and stereo variants) with
// per-slot bypass and whole-chain bypass (project guidelines §7, §9).
//
// Realtime contract:
//  - The chain does NOT own processors; callers do (UI thread).
//  - The callback increments a relaxed block counter, loads one immutable
//    snapshot pointer (acquire) per block, and never allocates, locks,
//    or frees.
//  - publish() builds a brand-new immutable snapshot, atomically swaps the
//    pointer, and retires the old snapshot. The old snapshot is freed later
//    by the UI thread in reap() once the block counter has advanced two blocks
//    past the swap (grace >= 1 guarantees no in-flight ASIO callback can still
//    be touching the old pointer).
//  - Callers that removed a processor must not destroy it until the snapshot
//    still referencing it has been reclaimed: after publish(), wait for
//    chain.settled() (or chain.blockCounter() past the returned floor).
//  - Bypassed slots pass the buffer through untouched; whole-chain bypass
//    passes the input straight to the output. In-place processing is safe
//    because Vst3Processor::chainProcess* copy the input to preallocated
//    staging before writing the output, so output may alias input.
class Vst3Chain
{
public:
    struct Slot
    {
        Vst3Processor* processor = nullptr;
        bool bypass = false;
    }; // the slot's layout is read from processor->layout() (VST-006)

    static constexpr std::size_t kMaxSlots = 8;
    static constexpr std::uint64_t kGraceBlocks = 2;

    Vst3Chain();
    ~Vst3Chain();

    // VST-006: preallocate the mono<->stereo adapter staging for the maximum
    // block size. Must be called on a control thread BEFORE streaming when any
    // slot may be driven with the opposite layout to its prepared one. Never
    // allocate in the audio callback (an unconfigured chain simply treats
    // opposite-layout slots as in-place passthrough).
    void configure(std::size_t maxBlockSamples);

    // Publishes a fresh immutable snapshot. UI thread only. More than kMaxSlots
    // slots is rejected (returns false). On success returns the block-counter
    // floor (via floorOut when non-null) captured at swap time, which callers
    // may use to defer destruction of removed processors: destroy only once
    // blockCounter() >= floor + kGraceBlocks or settled() is true.
    bool publish(std::vector<Slot> slots, bool wholeChainBypass, std::uint64_t* floorOut = nullptr);

    // Frees retired snapshots whose grace has elapsed. UI thread only.
    void reap();

    // Control thread only, after the host has detached every RT callback that
    // can call processMono/processStereo. Clears the live and retired snapshots
    // immediately without waiting for another block. The chain does not own or
    // detect callback lifetime.
    void quiescentShutdown();

    // True when no retired snapshot is still waiting for grace.
    bool settled() const;

    // Processed-block counter, relaxed. UI thread only.
    std::uint64_t blockCounter() const;

    // VST-016: total latency of the ACTIVE processing path, in samples, summed
    // across the currently published slots. A slot counts only when its
    // processor is non-null AND not bypassed; whole-chain bypass contributes
    // zero. This matches the VST3 convention that latency is a property of the
    // path audio actually passes through (a bypassed/missing slot is out of the
    // path). UI/control thread only; never the audio callback.
    std::uint32_t totalLatencySamples() const;

    // VST-015 adapter: exposes totalLatencySamples through the routing core's
    // LatencyQueryFn contract (context = Vst3Chain*). Lets the routing graph
    // query chain latency without hard-coding any plug-in value.
    static std::uint32_t chainLatencyQuery(void* context);

    // Realtime entry points compatible with engine::EngineGraph chain hooks.
    static void processMono(const float* input, float* output, std::size_t frames, void* context);
    static void processStereo(const float* leftIn, const float* rightIn, float* leftOut, float* rightOut,
                              std::size_t frames, void* context);

private:
    struct State
    {
        std::uint64_t retiredAtBlocks = 0;
        std::vector<Slot> slots;
        bool wholeChainBypass = false;
    };

    std::atomic<State*> m_state; // owned until reclaim; published snapshots are heap-allocated
    std::atomic<std::uint64_t> m_processedBlocks{0};
    std::vector<State*> m_retired; // UI-thread ownership after a publish swap
    bool m_quiescent = false;

    // VST-006 mono<->stereo adapter staging (preallocated by configure()).
    std::vector<float> m_adapterLeft;   // mono drive -> stereo slot (L)
    std::vector<float> m_adapterRight;  // mono drive -> stereo slot (R)
    std::vector<float> m_adapterMono;   // stereo drive -> mono slot
};

} // namespace audient::vst3