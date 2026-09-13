/*++

Copyright (c) Microsoft Corporation All Rights Reserved
Copyright (c) 2026 Audient Console contributors

Module Name:

    audientcapturesource.h

Abstract:

    Q5-A3 capture-source consumer seam for the AudientConsoleMic capture-only
    WaveRT driver.

    This module is the kernel side of the shared capture-ring transport
    (CaptureRingContract.h in src/virtual_audio/driver-protocol). It exposes:

      - AudientConsoleGetCaptureRingRegion(): the region provider. In A3 the
        provider ALWAYS returns NULL (the driver is not installed and no ring
        region is mapped), so the WaveRT capture stream keeps serving exact
        digital silence exactly as in A2. The consumer path below is compiled
        (so the shared contract header is exercised in the WDK build) but is
        not reachable until a later slice publishes a region.

      - AudientCaptureSourceFillContiguous(): given a published, validated
        region, reads FRESH mono float32 from the ring and converts each sample
        to an interleaved L/R signed 32-bit PCM pair, clamped to [-1,+1]
        (mono 1->2 on write, per the Q5-A3B boundary format decision). When the
        ring is empty the destination is filled with exact digital silence and
        the shortfall is counted in the ring header's underruns counter.

    IRQL: designed to be callable at DISPATCH_LEVEL from the capture stream's
    DMA fill path (TimerNotifyRT/WriteBytes): it allocates nothing, locks
    nothing, sleeps nothing, and performs only bounded volatile reads + the
    clamped mono->PCM32 L/R conversion.

    The kernel never depends on the user-mode IDriverCaptureSink interface:
    it only reads the plain versioned memory contract.

--*/

#ifndef _AUDIENTCAPTURESOURCE_H_
#define _AUDIENTCAPTURESOURCE_H_

// NOTE: this header follows the Simple Audio Sample convention — it does NOT
// include WDK headers itself. It is included by TUs that already pull the
// kernel header chain ("definitions.h" -> portcls/wdf/ntddk), which supplies
// BYTE/ULONG/PVOID/NTSTATUS/SAL. Do not include it before definitions.h.

// ---------------------------------------------------------------------------
// Kernel-side capture region manager (Q5-A3B).
//
// In A3 the provider ALWAYS returned NULL. From A3B the control plane publishes
// a user-provided, PAGE-LOCKED shared section (app <-> driver capture ring).
// This module owns the SPINLOCK + refcounted descriptor that makes the region
// safe against concurrent publish / unpublish / free while a DISPATCH-level DMA
// fill is reading it:
//
//   - The control plane (audientcontrolplane.cpp) locks the user's pages into
//     an MDL (MmProbeAndLockPages), initializes the shared CaptureRingHeader
//     (CaptureRingInit), then publishes the SYSTEM address of the locked pages.
//   - The DISPATCH fill path snapshots the active descriptor under the spinlock,
//     increments its refcount, reads the ring through desc->SystemAddress, and
//     decrements. It NEVER touches a raw user pointer.
//   - Unpublish marks the descriptor Retired under the spinlock; the caller then
//     WAITS for the refcount to reach zero before freeing the MDL/descriptor, so
//     an in-flight fill always finishes against still-valid locked pages. The
//     fill path never allocates, never waits, and never frees.
// ---------------------------------------------------------------------------

typedef struct _AUDIENT_CAPTURE_REGION
{
    volatile LONG RefCount;      // fills in flight (0 == freeable after Retire)
    volatile BOOLEAN Retired;    // marked under the spinlock before staged free
    PMDL Mdl;                    // locked user pages (FALSE lifetime mgmt)
    PVOID SystemAddress;         // MmGetSystemAddressForMdlSafe result
    ULONG Bytes;                 // locked byte count (== negotiated regionBytes)
} AUDIENT_CAPTURE_REGION, *PAUDIENT_CAPTURE_REGION;

// Initialize the module (spinlock). PASSIVE; call once from the adapter start
// path before any publish/fill can run.
VOID
AudientConsoleCaptureRingManagerInit(
    VOID
    );

// Publish `Region` (a NONPAGED-locked descriptor built over an MDL by the
// control plane) as the active capture ring. Any previous active descriptor is
// marked Retired under the spinlock and returned via *PreviousOut (the caller
// must wait for its refcount and free it). Control plane only, PASSIVE_LEVEL.
VOID
AudientConsoleCaptureRingPublish(
    _In_ PAUDIENT_CAPTURE_REGION Region,
    _Out_opt_ PAUDIENT_CAPTURE_REGION* PreviousOut
    );

// Wait until `Region` has no in-flight fill and free its MDL + descriptor.
// Called by the control plane AFTER AudientConsoleCaptureRingPublish returned
// it as PreviousOut (or after the active descriptor was retired). PASSIVE_LEVEL.
VOID
AudientConsoleCaptureRingRetireWaitAndFree(
    _In_ PAUDIENT_CAPTURE_REGION Region
    );

// Re-initialize the active region header (generation reset / flush) under the
// spinlock. Waits for any in-flight fill to drain, then captures a fresh empty
// epoch with `NewGeneration` and the CONNECTED flag set. Control plane only.
// Returns STATUS_SUCCESS, STATUS_NOT_FOUND (no active region) or an error.
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AudientConsoleCaptureRingReset(
    _In_ ULONG64 NewGeneration
    );

// Region provider: returns a pointer to the active region descriptor (may be
// NULL). Only used by diagnostics/control; the DMA path goes through
// AudientCaptureSourceFillContiguous.
PAUDIENT_CAPTURE_REGION
AudientConsoleGetCaptureRingRegion(
    VOID
    );

// Fill `ByteLength` bytes of a CONTIGUOUS capture DMA run starting at `Dst`
// from the shared ring (mono float32 -> interleaved L/R signed PCM32). `SampleRateHz`
// and `BytesPerFrame` are the negotiated stream geometry used to validate the
// region (contract = 48000 Hz, 2ch PCM32 => BytesPerFrame == 8).
// Returns TRUE when a valid region was present and the buffer was filled from
// the ring (any shortfall already exact silence). Returns FALSE when no region
// is published or it does not match the stream; the caller then fills silence.
_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
AudientCaptureSourceFillContiguous(
    _Out_writes_bytes_(ByteLength) BYTE* Dst,
    _In_ ULONG ByteLength,
    _In_ ULONG SampleRateHz,
    _In_ ULONG BytesPerFrame
    );

#endif // _AUDIENTCAPTURESOURCE_H_
