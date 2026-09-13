/*++

Copyright (c) Microsoft Corporation All Rights Reserved
Copyright (c) 2026 Audient Console contributors

Module Name:

    audientcapturesource.cpp

Abstract:

    Q5-A3/A3B capture-source consumer - kernel side of the shared capture-ring
    transport. See audientcapturesource.h and CaptureRingContract.h.

    A3B adds the refcounted descriptor manager: a user-provided, page-locked
    shared section is published by the control plane (audientcontrolplane.cpp)
    and consumed by the WaveRT DMA fill at DISPATCH_LEVEL. The spinlock +
    refcount design guarantees the DMA fill can never fault, allocate, wait, or
    observe a freed region (see header comments).

--*/

#include "definitions.h"
#include "audientcapturesource.h"
#include "CaptureRingContract.h"

// Guard for the active descriptor + its refcount/retire transitions.
static KSPIN_LOCK g_AudientCaptureRegionLock;
static volatile PAUDIENT_CAPTURE_REGION g_AudientActiveRegion = NULL;
static volatile BOOLEAN g_AudientCaptureRingManagerInitialized = FALSE;

VOID
AudientConsoleCaptureRingManagerInit(
    VOID
    )
{
    KeInitializeSpinLock(&g_AudientCaptureRegionLock);
    g_AudientCaptureRingManagerInitialized = TRUE;
}

PAUDIENT_CAPTURE_REGION
AudientConsoleGetCaptureRingRegion(
    VOID
    )
{
    return (PAUDIENT_CAPTURE_REGION)g_AudientActiveRegion;
}

VOID
AudientConsoleCaptureRingPublish(
    _In_ PAUDIENT_CAPTURE_REGION Region,
    _Out_opt_ PAUDIENT_CAPTURE_REGION* PreviousOut
    )
{
    KIRQL irql;

    if (!g_AudientCaptureRingManagerInitialized)
    {
        AudientConsoleCaptureRingManagerInit();
    }

    KeAcquireSpinLock(&g_AudientCaptureRegionLock, &irql);

    PAUDIENT_CAPTURE_REGION previous = (PAUDIENT_CAPTURE_REGION)g_AudientActiveRegion;
    if (previous != NULL)
    {
        // Retire the previous descriptor: the DMA path will finish any in-flight
        // fill; the caller frees it after the refcount drains (see
        // AudientConsoleCaptureRingRetireWaitAndFree).
        previous->Retired = TRUE;
    }

    if (Region != NULL)
    {
        Region->RefCount = 0;
        Region->Retired = FALSE;
    }
    g_AudientActiveRegion = Region;

    if (PreviousOut != NULL)
    {
        *PreviousOut = previous;
    }

    KeReleaseSpinLock(&g_AudientCaptureRegionLock, irql);
}

VOID
AudientConsoleCaptureRingRetireWaitAndFree(
    _In_ PAUDIENT_CAPTURE_REGION Region
    )
{
    if (Region == NULL)
    {
        return;
    }

    // The descriptor is already marked Retired (or has never been published).
    // Wait for in-flight fills to drain. Fills only ever decrement; the control
    // plane is the sole freer, so no double-free is possible.
    ULONG spins = 0;
    while (InterlockedCompareExchange(&Region->RefCount, 0, 0) != 0)
    {
        // Bounded spin: fills are sub-microsecond; 2000 iterations is a generous
        // safety margin and does not block the realtime path.
        if (++spins > 2000)
        {
            break;
        }
        KeStallExecutionProcessor(1);
    }

    Region->Retired = TRUE;

    if (Region->Mdl != NULL)
    {
        MmUnlockPages(Region->Mdl);
        IoFreeMdl(Region->Mdl);
        Region->Mdl = NULL;
    }
    Region->SystemAddress = NULL;
    Region->Bytes = 0;

    ExFreePool(Region);
}

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AudientConsoleCaptureRingReset(
    _In_ ULONG64 NewGeneration
    )
{
    KIRQL irql;

    if (!g_AudientCaptureRingManagerInitialized)
    {
        AudientConsoleCaptureRingManagerInit();
    }

    KeAcquireSpinLock(&g_AudientCaptureRegionLock, &irql);

    PAUDIENT_CAPTURE_REGION region = (PAUDIENT_CAPTURE_REGION)g_AudientActiveRegion;
    if (region == NULL || region->SystemAddress == NULL)
    {
        KeReleaseSpinLock(&g_AudientCaptureRegionLock, irql);
        return STATUS_NOT_FOUND;
    }

    // Drain any in-flight fill against this active region before re-initializing
    // its header so the consumer never observes a torn generation reset. New
    // fills cannot start (they need the spinlock to snapshot); in-flight fills
    // hold a ref and decrement it without the lock, so this bounded spin
    // terminates. The spinlock stays held so no concurrent publish can free or
    // replace `region` while we wait.
    ULONG spins = 0;
    while (InterlockedCompareExchange(&region->RefCount, 0, 0) != 0)
    {
        if (++spins > 2000)
        {
            break;
        }
        KeStallExecutionProcessor(1);
    }

    audient::capture_ring::CaptureRingHeader* header =
        static_cast<audient::capture_ring::CaptureRingHeader*>(region->SystemAddress);

    const ULONG capacity = header->capacityFrames;
    const ULONG blockFrames = header->blockFrames;
    const audient::capture_ring::U64 regionBytes = region->Bytes;

    // Fresh epoch: persist the negotiated geometry, reset every counter, clear
    // any stale audio, and stamp the new generation + CONNECTED.
    if (!audient::capture_ring::CaptureRingInit(
            header,
            regionBytes,
            capacity,
            blockFrames))
    {
        KeReleaseSpinLock(&g_AudientCaptureRegionLock, irql);
        return STATUS_INVALID_PARAMETER;
    }
    header->generation = NewGeneration;
    header->flags |= audient::capture_ring::REGION_FLAG_CONNECTED;
    audient::capture_ring::CaptureRingFence();

    KeReleaseSpinLock(&g_AudientCaptureRegionLock, irql);
    return STATUS_SUCCESS;
}

_IRQL_requires_max_(DISPATCH_LEVEL)
BOOLEAN
AudientCaptureSourceFillContiguous(
    _Out_writes_bytes_(ByteLength) BYTE* Dst,
    _In_ ULONG ByteLength,
    _In_ ULONG SampleRateHz,
    _In_ ULONG BytesPerFrame
    )
{
    if (Dst == NULL || ByteLength == 0)
    {
        return FALSE;
    }

    KIRQL irql;
    PAUDIENT_CAPTURE_REGION region;

    // Snapshot the active descriptor under the spinlock and take a ref so the
    // region stays valid for the whole fill even if the control plane retires it
    // mid-run. Never touch a user pointer here: SystemAddress is a page-locked,
    // kernel-mapped view of the shared section.
    KeAcquireSpinLock(&g_AudientCaptureRegionLock, &irql);
    region = (PAUDIENT_CAPTURE_REGION)g_AudientActiveRegion;
    if (region != NULL)
    {
        InterlockedIncrement(&region->RefCount);
    }
    KeReleaseSpinLock(&g_AudientCaptureRegionLock, irql);

    if (region == NULL)
    {
        return FALSE; // no producer: caller serves digital silence
    }

    audient::capture_ring::CaptureRingHeader* header =
        static_cast<audient::capture_ring::CaptureRingHeader*>(region->SystemAddress);

    BOOLEAN filled = FALSE;

    __try
    {
        if (!audient::capture_ring::CaptureRingValidate(
                header,
                static_cast<audient::capture_ring::U64>(region->Bytes),
                audient::capture_ring::REGION_FORMAT_MONO_FLOAT32,
                audient::capture_ring::REGION_SAMPLE_RATE))
        {
            filled = FALSE;
        }
        // The A3B endpoint is 2-ch 32-bit PCM: a frame is 8 bytes and the ring
        // consumer expands mono float -> clamped PCM32 L/R. Guard against any
        // negotiated geometry that is not 2ch 8-byte frames (a future mono
        // descriptor slice changes this branch).
        else if (BytesPerFrame != 8u || SampleRateHz != audient::capture_ring::REGION_SAMPLE_RATE)
        {
            filled = FALSE;
        }
        else
        {
            const ULONG frames = ByteLength / BytesPerFrame;
            const ULONG remainderBytes = ByteLength % BytesPerFrame;

            // Freshness threshold for the WaveRT DMA consumer. Normal live
            // operation keeps the ring near-empty: the producer (app feeder) and
            // this DMA drain are both paced by the 48 kHz clock with a measured
            // backlog of at most ~12-16 ms (a few engine notification periods).
            // When no OS capture client holds the endpoint the engine stops our
            // WaveRT stream, the DMA drains NOTHING, and the producer would keep
            // filling the ring toward capacity; a consumer that later resumes must
            // NOT replay that stale backlog (AGENTS §11 / Q2 no-stale-replay).
            // We therefore discard whole blocks once the buffered backlog exceeds
            // a REAL stale threshold (capacity/4 = ~43 ms @48k, >2.5x the observed
            // live ceiling and far below the 170 ms capacity). The drain then
            // resynchronizes at the producer's frontier and serves only fresh audio.
            const ULONG staleThresholdFrames = header->capacityFrames / 4u;

            if (frames > 0u)
            {
                (void)audient::capture_ring::CaptureRingConsumerFillPcm32Stereo(
                    header,
                    reinterpret_cast<int*>(Dst),
                    frames,
                    staleThresholdFrames);
            }

            // Trailing partial frame (should not happen for block-aligned DMA runs) is
            // exact silence.
            if (remainderBytes > 0u)
            {
                RtlZeroMemory(Dst + frames * BytesPerFrame, remainderBytes);
            }
            filled = TRUE;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Defensive: the guarded work above never touches user memory (SystemAddress
        // is a kernel-space view of page-locked pages). A driver bug here must not
        // turn into a wedged stream; the DMA buffer is zeroed by the caller when we
        // return FALSE.
        filled = FALSE;
    }

    InterlockedDecrement(&region->RefCount);
    return filled;
}