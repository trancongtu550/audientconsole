#pragma once

// CaptureRingContract.h - versioned app<->driver capture transport contract.
//
// Q5-A3 / plan "§9 step 3" seam. This is the SINGLE canonical, versioned layout
// and lock-free SPSC mono-float32 ring shared between:
//   - the user-mode producer  (VirtualMicFeeder -> IDriverCaptureSink -> ring),
//   - the kernel capture source consumer (WaveRT DMA fill, later install slice).
//
// HARD COMPILE CONTRACT:
//   * This header must compile unchanged in BOTH the user-mode CMake/MSVC C++20
//     build AND the WDK kernel-mode C++ build (files in src/virtual_audio/driver
//     are compiled as C++, /W4 /WX). Therefore:
//       - NO <atomic>, NO STL, NO exceptions, NO RTTI, NO dynamic allocation.
//       - Only fundamental types whose sizes are identical in user and kernel
//         MSVC x64 builds: unsigned int (32) and unsigned long long (64) and
//         float (32). Layout is fixed by the struct field order below.
//       - Memory ordering is x64-volatile based (MSVC /volatile gives
//         acquire/release for volatile accesses on x64) plus a compiler barrier
//         (_ReadWriteBarrier, <intrin.h>) around frontier publishes. This is a
//         deliberate x64-only design (the product is Windows 11 x64 only).
//   * This header is the ONLY file that describes the ring ABI. Do not add a
//     second copy anywhere; the driver project gets this path via an include
//     directory, it is never re-declared.
//
// Ring semantics (must match Q1/Q3 drop-new + freshness + generation rules):
//   - Producer (the app feeder worker thread) writes WHOLE blocks of mono
//     float32. Bounded and DROP-NEW: a full ring REJECTS the block (counted in
//     overflowDrops), it never grows and never blocks.
//   - Producer owns the WRITE head; the consumer owns the READ head (SPSC).
//     A producer-side epoch reset (generation change) is a control-plane
//     operation only valid while the consumer is quiescent (same documented
//     constraint as MockDriverCaptureSink / Q3). A live consumer recovering
//     from a stall discards stale whole blocks itself (freshness resync), so it
//     never replays a pre-recovery backlog.
//   - Consumer read: returns up to `frames` of FRESH mono float32. When fewer
//     than `frames` are buffered the shortfall is digital silence (the caller
//     writes zeros) and the shortfall is counted in underruns.
//   - Format: mono float32, 48 kHz, producer block == 64 frames (the transport
//     ::Format pinned by the Q1/Q3 slices).
//   - Counters are monotonic and never silently dropped. generationFlushes is
//     bumped by the producer when it opens a new attach epoch; the consumer
//     detects generation changes and discards stale whole blocks before reading.
//
// Region layout (a caller-provided, suitably aligned allocation of at least
// CaptureRingRegionBytes(capacityFrames) bytes):
//   [0 .. headerBytes-1]  CaptureRingHeader  (versioned POD)
//   [headerBytes ...]     float samples[capacityFrames]  (mono)

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace audient
{
namespace capture_ring
{

// ---------------------------------------------------------------------------
// Versioning and format tags.
// ---------------------------------------------------------------------------
enum RegionConstants
{
    REGION_MAGIC = 0x41554331u,                // '1CUA' little-endian
    REGION_VERSION = 1u,
    REGION_FORMAT_MONO_FLOAT32 = 1u,
    REGION_SAMPLE_RATE = 48000u,
    REGION_CHANNELS = 1u,
    REGION_FLAG_CONNECTED = 0x00000001u
};

// ---------------------------------------------------------------------------
// Fixed-width aliases (fundamental types only - kernel and user MSVC agree).
// ---------------------------------------------------------------------------
typedef unsigned int       U32;
typedef unsigned long long U64;

// ---------------------------------------------------------------------------
// Versioned region header. Field order below IS the ABI: never reorder, never
// repurpose a reserved field, and bump REGION_VERSION when the
// layout changes.
// ---------------------------------------------------------------------------
struct CaptureRingHeader
{
    U32 magic;              // +0x00  REGION_MAGIC
    U32 version;            // +0x04  REGION_VERSION
    U32 headerBytes;        // +0x08  offset of the sample array (= sizeof rounded up to 8)
    U32 regionBytes;        // +0x0c  total validated region size in bytes
    U32 formatTag;          // +0x10  mono float32
    U32 sampleRateHz;       // +0x14  48000
    U32 channels;           // +0x18  1 (the ring carries mono; L/R expansion is DMA-side)
    U32 blockFrames;        // +0x1c  producer block size in frames (64)
    U32 capacityFrames;     // +0x20  ring capacity in frames (power of two)
    volatile U32 flags;     // +0x24  REGION_FLAG_*
    U32 reserved[2];        // +0x28
    // 8-byte aligned from here. Cross-side fields are volatile: a producer
    // writes its own frontier and a consumer writes its own, and both must
    // observe the other side's publishes fresh (ordering via CaptureRingFence).
    volatile U64 generation;       // +0x30  last attach/epoch opened by the producer
    volatile U64 producerSequence; // +0x38  last ACCEPTED producer block index
    volatile U64 writePos;         // +0x40  producer frontier (monotonic frames)
    volatile U64 readPos;          // +0x48  consumer frontier (monotonic frames)
    volatile U64 producedSamples;  // +0x50  producer committed frames
    volatile U64 consumedSamples;  // +0x58  consumer served frames
    volatile U64 overflowDrops;    // +0x60  producer rejected frames (ring full / drop-new)
    volatile U64 staleCatchupDrops;// +0x68  consumer discarded stale frames (resync)
    volatile U64 underruns;        // +0x70  consumer shortfall frames (silence served)
    volatile U64 generationFlushes;// +0x78  producer-opened epochs (control plane)
    U64 reserved2[1];       // +0x80
};

static_assert(sizeof(CaptureRingHeader) == 0x88, "CaptureRingHeader layout");

// ---------------------------------------------------------------------------
// Pure geometry/validation helpers (no atomics; safe in every TU).
// ---------------------------------------------------------------------------

// Byte size required for a region holding `capacityFrames` mono frames.
inline U64 CaptureRingRegionBytes(U32 capacityFrames)
{
    return static_cast<U64>(sizeof(CaptureRingHeader)) + static_cast<U64>(capacityFrames) * 4u;
}

// Byte offset of the sample array (== headerBytes).
inline U32 CaptureRingDataOffset(const CaptureRingHeader* h)
{
    return (h == 0) ? 0u : h->headerBytes;
}

// Monotonic frame position -> wrapped index into the capacity window.
inline U32 CaptureRingIndex(const CaptureRingHeader* h, U64 pos)
{
    // capacityFrames is validated to be a power of two at init; the mask is the
    // (validated) capacity minus one. Compute defensively for zero capacity.
    const U32 capacity = (h == 0) ? 0u : h->capacityFrames;
    if (capacity == 0u)
    {
        return 0u;
    }
    return static_cast<U32>(pos & (static_cast<U64>(capacity) - 1u));
}

// Is `capacityFrames` a legal ring capacity (>= blockFrames, power of two)?
inline int CaptureRingCapacityIsLegal(U32 capacityFrames, U32 blockFrames)
{
    if (capacityFrames < blockFrames || capacityFrames == 0u)
    {
        return 0;
    }
    return (capacityFrames & (capacityFrames - 1u)) == 0u;
}

// Validate that a caller-provided region is a well-formed contract region.
// `regionBytes` is the mapping size the caller actually owns. formatTag == 0
// means "any supported capture format" (used by the driver before it commits
// to a specific negotiated sample type); otherwise an exact match is required.
inline int CaptureRingValidate(const CaptureRingHeader* h, U64 regionBytes,
                               U32 formatTag, U32 sampleRateHz)
{
    if (h == 0)
    {
        return 0;
    }
    if (h->magic != REGION_MAGIC ||
        h->version != REGION_VERSION ||
        h->headerBytes != sizeof(CaptureRingHeader))
    {
        return 0;
    }
    if (regionBytes < CaptureRingRegionBytes(h->capacityFrames) ||
        regionBytes < static_cast<U64>(h->headerBytes))
    {
        return 0;
    }
    if (h->channels != REGION_CHANNELS ||
        h->sampleRateHz != sampleRateHz ||
        !CaptureRingCapacityIsLegal(h->capacityFrames, h->blockFrames))
    {
        return 0;
    }
    if (formatTag != 0u && h->formatTag != formatTag)
    {
        return 0;
    }
    return 1;
}

// Initialize a freshly allocated region (caller owns the memory; regionBytes is
// the allocation size). Only the control plane calls this (never a realtime or
// DISPATCH path). Returns 1 on success, 0 on invalid parameters.
inline int CaptureRingInit(CaptureRingHeader* h, U64 regionBytes, U32 capacityFrames,
                           U32 blockFrames)
{
    if (h == 0)
    {
        return 0;
    }
    if (!CaptureRingCapacityIsLegal(capacityFrames, blockFrames))
    {
        return 0;
    }
    if (regionBytes < CaptureRingRegionBytes(capacityFrames))
    {
        return 0;
    }

    for (U32 i = 0; i < sizeof(CaptureRingHeader) / 4u; ++i)
    {
        reinterpret_cast<volatile U32*>(h)[i] = 0u;
    }

    h->magic = REGION_MAGIC;
    h->version = REGION_VERSION;
    h->headerBytes = static_cast<U32>(sizeof(CaptureRingHeader));
    h->regionBytes = static_cast<U32>(regionBytes);
    h->formatTag = REGION_FORMAT_MONO_FLOAT32;
    h->sampleRateHz = REGION_SAMPLE_RATE;
    h->channels = REGION_CHANNELS;
    h->blockFrames = blockFrames;
    h->capacityFrames = capacityFrames;
    h->flags = 0u;
    h->generation = 0u;
    h->producerSequence = 0u;
    h->writePos = 0u;
    h->readPos = 0u;
    h->producedSamples = 0u;
    h->consumedSamples = 0u;
    h->overflowDrops = 0u;
    h->staleCatchupDrops = 0u;
    h->underruns = 0u;
    h->generationFlushes = 0u;
    return 1;
}

// Compiler barrier between the data stores and the frontier publish/read.
inline void CaptureRingFence()
{
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#endif
}

// ---------------------------------------------------------------------------
// Ring core (SPSC; single producer, single consumer).
// ---------------------------------------------------------------------------

// Result of a producer write.
enum WriteResult
{
    WRITE_OK = 0,
    WRITE_DISCONNECTED = 1, // connected flag clear
    WRITE_STALLED = 2,      // ring full (drop-new; rejected)
    WRITE_INVALID = 3       // null mono/frames==0/format mismatch
};

// Ring data pointer for `h` when the region base is `regionBase`.
inline float* CaptureRingSamples(CaptureRingHeader* h)
{
    return h == 0 ? 0 : reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(h) + h->headerBytes);
}

// Number of committed mono frames currently buffered (clamped to capacity).
inline U64 CaptureRingAvailableFrames(const CaptureRingHeader* h)
{
    if (h == 0)
    {
        return 0u;
    }
    const U64 delta = h->writePos - h->readPos;
    return delta > static_cast<U64>(h->capacityFrames) ? static_cast<U64>(h->capacityFrames) : delta;
}

// Producer write of one whole mono block. Never blocks, never allocates.
// On a generation change this opens a NEW EPOCH and, because an attach is a
// control-plane event, discards any previously buffered audio so the consumer
// never replays a pre-reconnect backlog (same contract as the Q3 mock).
// generationFlushes is bumped exactly once per generation change.
inline WriteResult CaptureRingProducerWrite(CaptureRingHeader* h, const float* mono,
                                            U32 frames, U64 generation, U64 sequence)
{
    if (h == 0 || mono == 0 || frames == 0 || frames > h->blockFrames ||
        frames > h->capacityFrames)
    {
        return WRITE_INVALID;
    }
    if ((h->flags & REGION_FLAG_CONNECTED) == 0u)
    {
        return WRITE_DISCONNECTED;
    }

    // New attach epoch? Control-plane flush (consumer is quiescent at attach;
    // see header comment). Mirrors the Q3 mock's ring.reset(): discard any
    // pre-reconnect audio AND restart the per-epoch counters so snapshot()
    // reports only the current epoch (overflow/stale/underrun are transport
    // counters and reset exactly like the engine ring does). generationFlushes
    // stays monotonic.
    if (h->generation != generation)
    {
        h->readPos = 0u;
        h->writePos = 0u;
        h->producedSamples = 0u;
        h->consumedSamples = 0u;
        h->overflowDrops = 0u;
        h->staleCatchupDrops = 0u;
        h->underruns = 0u;
        h->generation = generation;
        h->generationFlushes += 1u;
        CaptureRingFence();
    }

    const U64 read = h->readPos;   // consumer-owned; read (acquire via volatile)
    CaptureRingFence();
    const U64 write = h->writePos; // producer-owned
    const U64 used = write - read;

    // Drop-new: reject the whole block when the ring cannot hold it.
    if (used + static_cast<U64>(frames) > static_cast<U64>(h->capacityFrames))
    {
        h->overflowDrops += static_cast<U64>(frames);
        return WRITE_STALLED;
    }

    const U32 capacity = h->capacityFrames;
    float* data = CaptureRingSamples(h);
    U32 index = CaptureRingIndex(h, write);
    for (U32 i = 0; i < frames; ++i)
    {
        data[(index + i) & (capacity - 1u)] = mono[i];
    }

    // Release fence BEFORE publishing the frontier: the consumer must observe
    // the sample stores before it observes the advanced writePos.
    CaptureRingFence();
    h->writePos = write + static_cast<U64>(frames);
    h->producedSamples += static_cast<U64>(frames);
    h->producerSequence = sequence;
    return WRITE_OK;
}

// Consumer read of exactly `frames` mono frames with the Q1 freshness policy:
// when the buffered backlog exceeds `staleThresholdFrames`, the consumer
// discards stale WHOLE blocks (advancing only its own read head) before
// reading, so a recovering consumer never hears a pre-recovery backlog.
//
// Returns 1 when the FULL `frames` block was served fresh (read head advanced),
// 0 when fewer than `frames` were buffered. On 0 the read head is NOT advanced
// and the shortfall is counted in `underruns`; the caller MUST then serve
// digital silence (identical full-block-or-nothing semantics to the Q1
// transport that the Q3 mock is built on).
inline int CaptureRingConsumerRead(CaptureRingHeader* h, float* out, U32 frames,
                                   U32 staleThresholdFrames)
{
    if (h == 0 || out == 0 || frames == 0)
    {
        return 0;
    }

    CaptureRingFence();
    const U64 write = h->writePos; // producer-owned; read (acquire via volatile)
    CaptureRingFence();

    // Consumer-side stale resync: discard whole blocks of the backlog, then
    // require a full block or fail. A recovering consumer never replays a
    // pre-recovery backlog.
    const U64 buffered0 = write - h->readPos;
    if (buffered0 > static_cast<U64>(staleThresholdFrames))
    {
        const U32 block = h->blockFrames;
        const U64 wholeBlocks = (buffered0 / static_cast<U64>(block)) * static_cast<U64>(block);
        if (wholeBlocks > 0u)
        {
            h->readPos = h->readPos + wholeBlocks;
            h->staleCatchupDrops += wholeBlocks;
            CaptureRingFence();
        }
    }

    const U64 available = write - h->readPos;
    if (available < static_cast<U64>(frames))
    {
        h->underruns += static_cast<U64>(frames - available);
        return 0;
    }

    const U32 capacity = h->capacityFrames;
    const float* data = CaptureRingSamples(h);
    U32 index = CaptureRingIndex(h, h->readPos);
    for (U32 i = 0; i < frames; ++i)
    {
        out[i] = data[(index + i) & (capacity - 1u)];
    }

    h->readPos = h->readPos + static_cast<U64>(frames);
    h->consumedSamples += static_cast<U64>(frames);
    CaptureRingFence();
    return 1;
}

// Kernel/DMA convenience: read `frames` mono frames and expand each sample to
// an interleaved L/R float32 stereo pair (mono 1->2 on write, per Q5-A3
// decision). `stereoOut` must have room for 2*frames floats. When fewer than
// `frames` are buffered the buffer is filled with EXACT digital silence
// (0.0f,0.0f per frame) and the shortfall is counted in `underruns`; returns 1
// when the whole request was fresh, 0 otherwise. Never leaves uninitialized
// samples in the DMA buffer.
inline int CaptureRingConsumerFillStereo(CaptureRingHeader* h, float* stereoOut,
                                         U32 frames, U32 staleThresholdFrames)
{
    if (stereoOut == 0 || frames == 0)
    {
        return 0;
    }

    const int fresh = (h == 0) ? 0 : CaptureRingConsumerRead(h, stereoOut, frames,
                                                             staleThresholdFrames);
    if (fresh)
    {
        // Expand the mono block to L/R, walking backward so the in-place
        // expansion never overwrites a mono source still needed.
        for (U32 i = frames; i > 0u; --i)
        {
            const U32 src = i - 1u;
            const float s = stereoOut[src];
            stereoOut[2u * src] = s;
            stereoOut[2u * src + 1u] = s;
        }
    }
    else
    {
        // Fresh read failed (or no region): exact digital silence everywhere.
        for (U32 i = 0; i < frames; ++i)
        {
            stereoOut[2u * i] = 0.0f;
            stereoOut[2u * i + 1u] = 0.0f;
        }
    }
    return fresh;
}

// Signed 32-bit PCM. The endpoint advertises 2ch int32 PCM (the A3B boundary
// format); the ring stays mono float32. This converts one clamped float sample
// to the PCM32 value the DMA fill writes for BOTH L and R. The multiply is done
// in double so INT32_MAX is represented exactly and the cast is defined for the
// clamped [-1,+1] input (no float-rounding past 2^31).
inline int CaptureRingFloatToPcm32(float s)
{
    // Sanitize NaN/Inf first (never let a NaN reach the DMA buffer).
    if (s != s) { return 0; }             // NaN -> silence
    if (s > 1.0f)      { s = 1.0f; }      // clamp [ -1, +1 ]
    else if (s < -1.0f) { s = -1.0f; }
    // Exact int32 full-scale conversion: for |s| <= 1 the double product is in
    // (-2147483647, 2147483647] and the cast is well defined.
    return static_cast<int>(static_cast<double>(s) * 2147483647.0);
}

// Kernel/DMA convenience for the 2ch PCM32 endpoint: read `frames` mono frames
// from the ring and write an interleaved L/R INT32 PCM32 block (`pcm32Out` must
// have room for 2*frames int32s). Every mono sample is clamped to [-1,+1] and
// duplicated to L=R. Uses EXACTLY the same freshness/stale/underrun path as
// CaptureRingConsumerRead (single canonical implementation), so counters and
// no-stale-replay semantics are identical to the float fill. When fewer than
// `frames` are buffered the whole block is EXACT digital silence (0 L/R) and
// the shortfall is counted in `underruns`; returns 1 only when the whole block
// was fresh. Never leaves uninitialized samples in the DMA buffer.
inline int CaptureRingConsumerFillPcm32Stereo(CaptureRingHeader* h, int* pcm32Out,
                                              U32 frames, U32 staleThresholdFrames)
{
    if (pcm32Out == 0 || frames == 0)
    {
        return 0;
    }

    // Mono read needs a scratch area of `frames` floats. Reuse the FIRST half of
    // the output buffer (a frame is 8 output bytes but only the first 4 carry
    // the mono source at any time). This keeps the fill single-pass, allocation
    // free and DISPATCH_LEVEL safe: the source read is complete before any
    // destination write, and no source still needed is overwritten because we
    // convert backward.
    const int fresh = (h == 0) ? 0 : CaptureRingConsumerRead(
        h, reinterpret_cast<float*>(pcm32Out), frames, staleThresholdFrames);

    if (fresh)
    {
        // Convert mono -> PCM32 L/R, walking backward in-place (source frame i is
        // written to pcm32Out[i] = 4-byte slot; destination frame occupies
        // pcm32Out[2i] and pcm32Out[2i+1]). Convert from the tail so the mono
        // source for earlier frames is never overwritten.
        for (U32 i = frames; i > 0u; --i)
        {
            const U32 src = i - 1u;
            const float s = reinterpret_cast<const float*>(pcm32Out)[src];
            const int v = CaptureRingFloatToPcm32(s);
            pcm32Out[2u * src] = v;
            pcm32Out[2u * src + 1u] = v;
        }
    }
    else
    {
        // Fresh read failed (or no region): exact digital silence (0 int32 per
        // channel) everywhere.
        for (U32 i = 0; i < frames; ++i)
        {
            pcm32Out[2u * i] = 0;
            pcm32Out[2u * i + 1u] = 0;
        }
    }
    return fresh;
}

} // namespace capture_ring
} // namespace audient
