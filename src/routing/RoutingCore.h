#pragma once

#include "routing/RoutingTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace audient::routing
{

// Realtime-clean fixed v1 routing graph, independent of any audio backend and
// any UI (project guidelines §4, §7; Phase 5 routing-core rescope 2026-09-03).
//
// The core owns only PREALLOCATED internal staging buffers. Every block:
//   inputs    : for each bound runtime slot `ch`: inputSource[ch] ->
//               [inputRawTap[ch]] -> that slot's input chain ->
//               inputProcessed[ch] (+ inputMonitorFeed[ch]); the slot selected
//               by binding.micUplinkChannel also drives the mono processed-
//               capture bus (micUplink).
//   downlink  : playback L/R -> output chain -> outputProcessed L/R
//               -> physical output Left/Right.
//
// Input channels are independent processing instances (own staging, own chain
// hook, own latency seam) indexed by runtime slot. The core never assumes
// slot index == physical identity == driver channel index (that mapping lives
// in the backend binding table, asio/AsioChannelMap.h: StreamInputBinding).
// Per-channel processed-monitor feeds are TAPPED only; mixing them into the
// physical output is the backend render branch's job (see AsioRoutingAdapter).
//
// The two paths are independent: nothing derived from the downlink ever feeds
// an input path (AGENTS §4 "isolated"), enforced structurally by this class.
//
// Chain hooks:
//  - setInputChain(channel, fn, context) / setOutputChain install a processor
//    callback + opaque context per processing element. The legacy
//    setInputChain(fn, context) overload installs channel 0 (the v1 uplink
//    channel). Hooks are installed on a control thread BEFORE stream start and
//    must not change while process() runs. An empty hook behaves as wire.
//
// Realtime contract: process() performs no allocation, no locking, no I/O, no
// unbounded loops, no exception. All internal buffers are sized in the
// constructor (maxBlockSamples x kMaxInputChannels); calls with frames > max
// are a hard no-op.
//
// Ownership contract: sources and sinks in BlockBinding are borrowed ONLY for
// the duration of one process() call; never retained. A null sink/source is
// legal and means "route produces/drops nothing there" (never written / no
// stale replay).
class RoutingCore
{
public:
    static constexpr std::size_t kMaxInputChannels = routing::kMaxInputChannels;

    using MonoChainFn = void (*)(const float* input, float* output, std::size_t frames, void* context);
    using StereoChainFn = void (*)(const float* leftIn, const float* rightIn, float* leftOut, float* rightOut,
                                   std::size_t frames, void* context);

    explicit RoutingCore(std::size_t maxBlockSamples);

    RoutingCore(const RoutingCore&) = delete;
    RoutingCore& operator=(const RoutingCore&) = delete;

    // Control-thread installation of processing hooks (before streaming).
    // Per-channel input hooks: each runtime slot is an independent seam.
    void setInputChain(std::size_t channel, MonoChainFn fn, void* context);
    void setInputChain(MonoChainFn fn, void* context); // legacy alias: channel 0 (the v1 uplink channel)
    void setOutputChain(StereoChainFn fn, void* context);
    void clearChains();

    // VST-015 latency model hooks (control-thread only, set before streaming).
    //  - setSourceLatency(BusId::PhysicalInput1 | BusId::PlaybackBus, samples):
    //    backend-reported source latency. Only source buses are accepted.
    //  - setInputChainLatency(channel, ...) wires the latency query of the
    //    SAME chain object installed on that channel. The legacy single-argument
    //    form targets channel 0. Null hook = 0 latency. Each slot's latency is
    //    queried independently (inputChannelProcessedLatency); the uplink
    //    LatencySnapshot continues to describe channel 0 during Phase B.
    void setSourceLatency(BusId bus, std::uint32_t samples);
    void setInputChainLatency(std::size_t channel, LatencyQueryFn fn, void* context);
    void setInputChainLatency(LatencyQueryFn fn, void* context); // legacy alias: channel 0
    void setOutputChainLatency(LatencyQueryFn fn, void* context);

    // Realtime entry: routes one block through the fixed topology.
    void process(BlockBinding& binding);

    // Per-channel processed latency seam (samples) = source latency + that
    // slot's chain latency. A wire/null chain reports the source latency.
    // Control-thread query; never the audio callback. Reports 0 for an
    // out-of-range slot.
    std::uint32_t inputChannelProcessedLatency(std::size_t channel) const;

    // VST-015: computes the current per-bus end-to-end latency model from the
    // configured source latencies and chain latency hooks. Control-thread only.
    // The mic-path fields describe the v1 uplink channel (runtime slot 0).
    LatencySnapshot latencySnapshot() const;

    std::size_t maxBlockSamples() const { return m_maxBlock; }

private:
    struct InputChannelStage
    {
        std::vector<float> raw;       // pre-chain raw tap staging (per slot)
        std::vector<float> processed; // chain staging / post-chain result (per slot)
        MonoChainFn chain = nullptr;
        void* chainContext = nullptr;
    };

    std::uint32_t channelChainLatency(std::size_t channel) const;

    void routeInputChannels(BlockBinding& binding);
    void routeDownlinkPath(BlockBinding& binding);
    void copyMono(const float* src, float* dst, std::size_t frames) const;
    void copyStereo(const float* ls, const float* rs, float* ld, float* rd, std::size_t frames) const;

    std::size_t m_maxBlock = 0;

    // Preallocated per-slot input staging. Fixed capacity, sized in the
    // constructor; never resized on the audio path.
    std::array<InputChannelStage, kMaxInputChannels> m_inputs;

    // Per-slot chain latency seams (control-thread installed).
    std::array<LatencyQueryFn, kMaxInputChannels> m_inputChainLatency{};
    std::array<void*, kMaxInputChannels> m_inputChainLatencyContext{};

    // Preallocated downlink staging (unchanged single stereo route).
    std::vector<float> m_outputL; // post-output-chain L
    std::vector<float> m_outputR; // post-output-chain R

    StereoChainFn m_outputChain = nullptr;
    void* m_outputChainContext = nullptr;

    LatencyQueryFn m_outputChainLatency = nullptr;
    void* m_outputChainLatencyContext = nullptr;

    std::uint32_t m_physicalInputLatency = 0;
    std::uint32_t m_playbackLatency = 0;
};

} // namespace audient::routing
