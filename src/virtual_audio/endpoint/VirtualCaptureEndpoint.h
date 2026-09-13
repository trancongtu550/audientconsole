#pragma once

#include "transport/TransportFormat.h"

#include <cstddef>
#include <cstdint>

namespace audient::virtual_audio
{

// v1 capture endpoint identity (project guidelines §4). The user confirmed 2026-09-04
// that the canonical friendly name stays "Microphone (Audient Console)"; the
// "AudientConsole Mic FX" label from the Q2 wire-art is not the shipped name.
inline constexpr const char* kVirtualMicFriendlyName = "Microphone (Audient Console)";

// Per-endpoint delivery diagnostics. Every drop class is counted distinctly
// (AGENTS §11 / Slice Q1 freshness policy); the endpoint never drops silently.
struct EndpointCounters
{
    std::uint64_t producedSamples = 0;    // processed mic samples committed by the engine
    std::uint64_t overflowDrops = 0;      // producer rejected a full ring (drop-new)
    std::uint64_t staleCatchupDrops = 0;  // stale backlog discarded on consumer resync
    std::uint64_t underruns = 0;          // empty-read frames (transport level)
    std::uint64_t freshServedFrames = 0;  // processed mic frames handed to clients
    std::uint64_t silenceServedFrames = 0; // digital silence frames handed to clients
};

// Contract every Windows virtual microphone capture endpoint MUST honor. The
// future WaveRT capture miniport (Phase 8, based on the Microsoft "Simple
// Audio Sample Device Driver", MS-PL) and the in-tree SoftwareCaptureEndpoint
// both implement this pull contract over a transport::VirtualCaptureTransport.
//
// Endpoint rules (project guidelines §11 capture/uplink behavior):
//  - Serve ONLY processed mic audio (mono, 48 kHz float32) — never raw, never
//    system/downlink audio.
//  - Never replay stale/uninitialized samples: honor the transport freshness
//    policy (readProcessedMicFresh) so a recovering consumer does not hear a
//    stale backlog.
//  - App absent/crashed/late => output DIGITAL SILENCE. The pull never blocks
//    and never invents or replays audio.
//  - All drops are counted in the probe-visible counters.
//
// This contract is the ONLY seam between the user-space engine and the Windows
// endpoint: no driver-specific code exists in src/routing, src/vst3, or
// src/transport.
class VirtualCaptureEndpoint
{
public:
    virtual ~VirtualCaptureEndpoint() = default;

    virtual transport::Format format() const = 0;
    virtual const char* friendlyName() const = 0;

    // Pull `frames` samples of processed mic mono. Always fills `frames`
    // samples (fresh audio or digital silence) and returns the number of
    // samples written (== frames on success; 0 only for null/empty input).
    // `staleThresholdFrames` is the freshness threshold forwarded to the
    // transport reader. Never blocks, never throws.
    virtual std::size_t captureMono(float* mono, std::size_t frames, std::size_t staleThresholdFrames) = 0;

    // Same fill semantics as captureMono, but returns whether the block was
    // served from FRESH engine data (true) or is synthesized DIGITAL SILENCE
    // because the transport was empty (false). A driver feeder MUST use this to
    // avoid pushing invented silence blocks into the driver capture buffer.
    virtual bool captureMonoFresh(float* mono, std::size_t frames, std::size_t staleThresholdFrames) = 0;

    // True once the engine producer has committed at least one block since the
    // last reset. Informational only: silence-on-absence is guaranteed by the
    // full-frames rule in captureMono, not by this flag.
    virtual bool producerActive() const = 0;

    // Control plane (never from a realtime thread): clear the endpoint +
    // underlying transport into a FRESH capture epoch so the next pulls are
    // current engine output (used on stream start / device reconnect).
    virtual void reset() = 0;

    virtual EndpointCounters counters() const = 0;
};

} // namespace audient::virtual_audio