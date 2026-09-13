#pragma once

#include <cstddef>
#include <cstdint>

namespace audient::routing
{

// Fixed engine capacity for simultaneously bound input channels (runtime
// slots). This is the reusable per-channel processing ceiling of the routing
// graph. It is deliberately independent of audient::channel: that namespace's
// kMaxPhysicalInputChannels is the control-side domain ceiling. A unit test
// static_asserts the two stay equal so they cannot drift.
inline constexpr std::size_t kMaxInputChannels = 8;

// Logical endpoint and bus identifiers used by the v1 routing graph.
//
// These IDs are the ONLY contract between the routing core and any transport:
// Audient ASIO, a future virtual-ASIO driver, WASAPI, or a test harness binds
// its own buffers to these logical IDs, and the graph is never rewritten per
// backend (project guidelines §4, project plan Phase 5 routing-core rescope 2026-09-03).
//
// The IDs are explicitly NOT hardware-specific: "Physical Input 1" and
// "Physical Output 1/2" are logical roles. The iD14 mapping happens only in a
// backend adapter (e.g. the ASIO channel plan), never inside the core.
enum class BusId : std::uint8_t
{
    // Uplink (mic) path sources.
    PhysicalInput1 = 1, // mono: the one selected physical input
    MicUplink = 2,      // mono: processed mic capture bus (virtual mic soon)

    // Mic chain internal sub-buses (not sources, not final sinks).
    MicRaw = 3,         // mono: raw mic tap BEFORE the mic chain
    MicProcessed = 4,   // mono: mic AFTER the input VST chain

    // Downlink (system) path source and its processed capture.
    PlaybackBus = 5,    // stereo: the whole Windows system-mix bus
    OutputProcessed = 6, // stereo: downlink AFTER the output VST chain

    // Physical render sinks.
    PhysicalOutputLeft = 7,  // mono/single: physical output "1"
    PhysicalOutputRight = 8, // mono/single: physical output "2"
};

std::size_t channelCount(BusId bus); // 1 or 2 (stable, for buffer sizing)
const char* busName(BusId bus);      // stable logical name for diagnostics

// Query the current processing latency (in samples) of a route processing
// element (e.g. a VST3 chain). Returns the value as reported by the plug-in
// layer (Vst3Chain::totalLatencySamples); the routing core never hard-codes a
// plug-in-specific latency. Control-thread query; never the audio callback.
using LatencyQueryFn = std::uint32_t (*)(void* context);

// Per-block buffer binding. All pointers are borrowed for the duration of one
// process() call; the core never stores them beyond the call and never owns
// them. Sinks may be null when that output is not wired to anything (the core
// treats a null sink as "drop/output silence rather than write garbage" — it
// simply does not dereference a null destination).
//
// Input channels are indexed by RUNTIME SLOT. The core never assumes that a
// slot equals a physical identity, a driver channel index, or a collection
// position — that mapping is resolved once, on the control path, into the
// backend's binding table (see asio/AsioChannelMap.h: StreamInputBinding). The
// core only loops the slots `inputChannels` says are live.
struct BlockBinding
{
    std::size_t frames = 0;

    // Input (uplink) channels. A null inputSource[ch] means "no signal for this
    // channel this block": the core leaves that slot's staging untouched and
    // the caller must already have zeroed its own sinks for that slot (never
    // replay stale samples, AGENTS §7/§11).
    std::size_t inputChannels = 0;                   // 0..kMaxInputChannels
    const float* inputSource[kMaxInputChannels] = {}; // raw mono physical input per slot
    float* inputRawTap[kMaxInputChannels] = {};      // pre-chain raw tap per slot (may be null)
    float* inputProcessed[kMaxInputChannels] = {};   // post-input-chain per slot (may be null)
    float* inputMonitorFeed[kMaxInputChannels] = {}; // processed-monitor contribution per slot (may be null)

    // Mono processed-capture (virtual mic) bus. The core copies the PROCESSED
    // buffer of the selected slot (micUplinkChannel; v1/Phase B = channel 0)
    // here. Phase C replaces the single-channel selector with the mixer sum.
    std::size_t micUplinkChannel = 0;
    float* micUplink = nullptr;

    // Downlink path.
    const float* playbackLeft = nullptr;   // stereo source L
    const float* playbackRight = nullptr;  // stereo source R
    float* outputProcessedLeft = nullptr;  // post-output-chain L
    float* outputProcessedRight = nullptr; // post-output-chain R

    // Physical render sinks.
    float* physicalOutputLeft = nullptr;
    float* physicalOutputRight = nullptr;
};

// Latency model of the fixed v1 topology (VST-015 foundation).
//
// Invariant (Slice O goal):
//   destination bus latency = source bus latency + route processing latency
//
// The model holds per-bus end-to-end latency in samples:
//   PhysicalInput1          -> physical input source latency (backend-provided)
//   MicRaw                  -> = PhysicalInput1 (wire tap; no processing)
//   MicProcessed            -> = PhysicalInput1 + input chain latency
//   MicUplink               -> = MicProcessed (wire; uplink carries processed
//                               mic only)
//   PlaybackBus             -> playback source latency (backend-provided)
//   OutputProcessed         -> = PlaybackBus + output chain latency
//   PhysicalOutput Left/Right -> = OutputProcessed (wire)
//
// Source latencies are what the transport/backend reports for its own input and
// output paths (e.g. ASIO input/output latencies); route processing latencies
// come from the latency query hooks (e.g. Vst3Chain::totalLatencySamples). No
// plug-in-specific constant lives here.
struct LatencySnapshot
{
    std::uint32_t physicalInput = 0;
    std::uint32_t micRaw = 0;
    std::uint32_t micProcessed = 0;
    std::uint32_t micUplink = 0;
    std::uint32_t playbackBus = 0;
    std::uint32_t outputProcessed = 0;
    std::uint32_t physicalOutput = 0;
};

} // namespace audient::routing