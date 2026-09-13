/*++

Copyright (c) Microsoft Corporation All Rights Reserved
Copyright (c) 2026 Audient Console contributors

Module Name:

    audientcontrolplane.cpp

Abstract:

    Q5-A3B control plane for the shared capture ring transport.

    This module owns a dedicated control DEVICE_OBJECT that is NOT part of the
    audio filter stack. It exposes the stable device-interface GUID from
    CaptureControlPlane.h and implements the CONNECT / DISCONNECT / FLUSH /
    QUERY_STATE / QUERY_CAPS IOCTLs against the refcounted region manager in
    audientcapturesource.cpp.

    Lifecycle-safety design (see also docs/driver-buffer-mapping-safety.md and
    the CaptureControlPlane.h header):

      - The user-mode client creates a SECTION (CreateFileMapping), maps a view,
        and passes the user-mode base + byte length in the CONNECT request. The
        kernel NEVER dereferences that user pointer in the DMA path. CONNECT
        locks the client's pages into an MDL (MmProbeAndLockPages,
        MmGetSystemAddressForMdlSafe) and publishes only the KERNEL (system)
        address to the capture source.
      - Page lifetime / pinning: the MDL pins the region's physical pages while
        it exists. The driver holds the MDL until DISCONNECT, handle close
        (IRP_MJ_CLEANUP -> deterministic cleanup on process exit/handle close),
        or device removal, so DISPATCH_LEVEL fills always see resident pages.
      - Process exit / unload: when the client process exits, its handle to the
        control device closes and IRP_MJ_CLEANUP runs the same unpublish +
        staged-free path as an explicit DISCONNECT, so no stale ring is observed
        and no pages leak. On device removal the active region is unpublished
        before the adapter teardown completes.
      - Concurrent disconnect: the spinlock + refcount in audientcapturesource
        serializes publish/unpublish/free against in-flight DISPATCH fills;
        the fill path never frees, so a disconnect can never leave a dangling
        region behind a running fill.
      - Single active producer: CONNECT from a second handle while one is
        connected is rejected (STATUS_DEVICE_BUSY). Only the connected handle
        may DISCONNECT/FLUSH.
      - IRQL: all IOCTLs are METHOD_BUFFERED at PASSIVE_LEVEL. The DMA path
        never allocates, never waits, never takes a blocking lock, and only
        reads the locked system VA.

--*/

#include "definitions.h"
#include "audientcapturesource.h"
#include "audientcontrolplane.h"
#include "CaptureControlPlane.h"

#include <wdm.h>

// Control device name + symlink. The symbolic link is what SetupAPI hands to the
// app; we also keep a name so the DO is addressable.
#define AUDIENT_CONTROL_DEVICE_NAME L"\\Device\\AudientConsoleControl"
#define AUDIENT_CONTROL_SYMLINK_NAMES L"\\DosDevices\\AudientConsoleControl"

// Pool tag for control-plane allocations.
#define AUDIENT_CONTROL_POOLTAG 'LPCa'

// Forward declarations.
DRIVER_DISPATCH AudientControlCreate;
DRIVER_DISPATCH AudientControlClose;
DRIVER_DISPATCH AudientControlCleanup;
DRIVER_DISPATCH AudientControlDeviceControl;

static PDEVICE_OBJECT g_AudientControlDevice = NULL;

// Saved original dispatch routines (portcls). Non-control IRPs are forwarded.
static PDRIVER_DISPATCH g_OriginalCreate = NULL;
static PDRIVER_DISPATCH g_OriginalClose = NULL;
static PDRIVER_DISPATCH g_OriginalCleanup = NULL;
static PDRIVER_DISPATCH g_OriginalDeviceControl = NULL;

// The connected producer. All control-plane state is guarded by the region
// manager's spinlock where it touches the DMA path; this file-object pointer is
// control-plane only (single producer, serialized by the I/O manager).
static PDEVICE_OBJECT g_AudientConnectedDevice = NULL;
static PFILE_OBJECT g_AudientConnectedFile = NULL;
static volatile BOOLEAN g_AudientProducerConnected = FALSE;

// Monotonic attach-generation counter (kernel-assigned epoch for the ring).
// Only the low 32 bits are atomically incremented; the count is assigned into a
// ULONGLONG in each response. 2^31 connects before wraparound is far beyond the
// tested lifetime, and 0 is reserved for "never attached".
static volatile LONG g_AudientGeneration = 0;

// Bump and return the next attach generation (1-based).
static
ULONG64
AudientControlPlaneNextGeneration(
    VOID
    )
{
    const LONG raw = InterlockedIncrement(&g_AudientGeneration);
    return static_cast<ULONG64>(raw < 0 ? -static_cast<LONGLONG>(raw) : raw);
}

// Registered interface symbolic link (UNICODE_STRING allocated at init).
static UNICODE_STRING g_AudientControlSymbolicLink;
static BOOLEAN g_AudientControlSymbolicLinkValid = FALSE;

// Keep a copy of the interface GUID bytes for diagnostics.
static unsigned char g_AudientControlGuid[16];

// ---------------------------------------------------------------------------
// Device-interface registration (control device object).
// ---------------------------------------------------------------------------

#pragma code_seg("PAGE")
static
NTSTATUS
AudientControlPlaneCreateControlDevice(
    _In_ PDRIVER_OBJECT DriverObject
    )
/*++

Routine Description:

    Creates the dedicated control device object and registers the stable device
    interface GUID + symlink. Called once from DriverEntry after
    PcInitializeAdapterDriver. It is NOT attached to the audio stack; all IRPs
    to it are routed to our dispatch entries.

--*/
{
    PAGED_CODE();

    NTSTATUS ntStatus;
    UNICODE_STRING deviceName;
    UNICODE_STRING symlinkName;

    RtlInitUnicodeString(&deviceName, AUDIENT_CONTROL_DEVICE_NAME);
    RtlInitUnicodeString(&symlinkName, AUDIENT_CONTROL_SYMLINK_NAMES);

    ntStatus = IoCreateDevice(
        DriverObject,
        0,
        &deviceName,
        FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN,
        FALSE,
        &g_AudientControlDevice);
    if (!NT_SUCCESS(ntStatus))
    {
        DPF(D_ERROR, ("AudientControlPlane: IoCreateDevice failed 0x%x", ntStatus));
        return ntStatus;
    }

    // METHOD_BUFFERED IOCTLs: the I/O manager copies the small fixed-size
    // request/response structs into/out of the system buffer. No arbitrary user
    // pointers ever reach the DMA path.
    g_AudientControlDevice->Flags |= DO_BUFFERED_IO;
    g_AudientControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    ntStatus = IoCreateSymbolicLink(&symlinkName, &deviceName);
    if (!NT_SUCCESS(ntStatus))
    {
        DPF(D_ERROR, ("AudientControlPlane: IoCreateSymbolicLink failed 0x%x", ntStatus));
        IoDeleteDevice(g_AudientControlDevice);
        g_AudientControlDevice = NULL;
        return ntStatus;
    }

    audient::capture_control::CaptureControlGuidFill(g_AudientControlGuid);

    // The interface GUID is registered on the PHYSICAL device object (PDO) from
    // the adapter start path (StartDevice -> AudientControlPlaneRegisterInterface),
    // where a PnP PDO exists. A raw control DEVICE_OBJECT created by IoCreateDevice
    // is NOT a PDO and cannot host IoRegisterDeviceInterface (INVALID_DEVICE_REQUEST).

    // TODO(A3B): PDO interface registration wiring in adapter.cpp StartDevice.
    // The control device + Its symlink below are the primary transport; the PDO
    // interface discovery is wired once StartDevice can pass the PDO in.
    DPF(D_TERSE, ("AudientControlPlane: control device + symlink ready"));
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// PDO interface registration (stable device-interface GUID).
// ---------------------------------------------------------------------------

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AudientControlPlaneRegisterInterface(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
    )
/*++

Routine Description:

    Registers the stable control-plane interface GUID on the PHYSICAL device
    object and enables it. Called from the adapter StartDevice path (after the
    adapter common has been initialized and the actual PDO is available). User
    mode discovers the interface with SetupAPI by GUID; the symlink is exposed by
    the control device object's own name.

--*/
{
    PAGED_CODE();

    if (g_AudientControlSymbolicLinkValid && g_AudientControlSymbolicLink.Buffer != NULL)
    {
        // Already registered (StartDevice may run more than once on stop/start);
        // just re-enable so it survives a disable during pause.
        (void)IoSetDeviceInterfaceState(&g_AudientControlSymbolicLink, TRUE);
        return STATUS_SUCCESS;
    }

    audient::capture_control::CaptureControlGuidFill(g_AudientControlGuid);
    GUID interfaceGuid;
    RtlCopyMemory(&interfaceGuid, g_AudientControlGuid, sizeof(GUID));

    NTSTATUS ntStatus = IoRegisterDeviceInterface(
        PhysicalDeviceObject,
        &interfaceGuid,
        NULL,
        &g_AudientControlSymbolicLink);
    if (!NT_SUCCESS(ntStatus))
    {
        DPF(D_ERROR, ("AudientControlPlane: IoRegisterDeviceInterface(PDO) failed 0x%x", ntStatus));
        return ntStatus;
    }
    g_AudientControlSymbolicLinkValid = TRUE;

    ntStatus = IoSetDeviceInterfaceState(&g_AudientControlSymbolicLink, TRUE);
    if (!NT_SUCCESS(ntStatus))
    {
        DPF(D_ERROR, ("AudientControlPlane: IoSetDeviceInterfaceState(TRUE) failed 0x%x", ntStatus));
        RtlFreeUnicodeString(&g_AudientControlSymbolicLink);
        g_AudientControlSymbolicLinkValid = FALSE;
        return ntStatus;
    }

    DPF(D_TERSE, ("AudientControlPlane: PDO interface GUID registered + enabled"));
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Producer binding.
// ---------------------------------------------------------------------------

static
VOID
AudientControlPlaneUnbindProducer(
    VOID
    )
/*++

Routine Description:

    Control-plane-only producer teardown. Unpublishes the active region (marks
    it Retired), waits for the in-flight fill to drain, and frees its MDL. The
    DMA path observes silence immediately after the un-publish; the wait
    guarantees no fill is still touching the freed pages.

--*/
{
    PAUDIENT_CAPTURE_REGION previous = NULL;

    // Unpublish (retire) whatever is active. The manager hands us the previous
    // descriptor so we can stage its free after the refcount drains.
    AudientConsoleCaptureRingPublish(NULL, &previous);

    if (previous != NULL)
    {
        // Clear REGION_FLAG_CONNECTED on the SHARED header BEFORE freeing so the
        // app producer observes WRITE_DISCONNECTED on its next write instead of
        // silently filling an orphaned region after DISCONNECT / handle close /
        // device STOP. The DMA path already serves exact digital silence once
        // the descriptor is retired (no-stale replay / exact-silence semantics,
        // AGENTS §11). The flag is re-set by the next CONNECT's CaptureRingInit.
        if (previous->SystemAddress != NULL)
        {
            audient::capture_ring::CaptureRingHeader* h =
                static_cast<audient::capture_ring::CaptureRingHeader*>(previous->SystemAddress);
            h->flags &= ~audient::capture_ring::REGION_FLAG_CONNECTED;
            audient::capture_ring::CaptureRingFence();
        }
        AudientConsoleCaptureRingRetireWaitAndFree(previous);
    }

    g_AudientProducerConnected = FALSE;
    g_AudientConnectedDevice = NULL;
    g_AudientConnectedFile = NULL;
}

// ---------------------------------------------------------------------------
// Dispatch routines.
// ---------------------------------------------------------------------------

#pragma code_seg("PAGE")
NTSTATUS
AudientControlCreate(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
/*++

Routine Description:

    IRP_MJ_CREATE. Only IRPs targeting the control device are handled here;
    everything else (the audio filter stack) is forwarded to the saved portcls
    handler.

--*/
{
    PAGED_CODE();

    if (DeviceObject != g_AudientControlDevice)
    {
        if (g_OriginalCreate != NULL)
        {
            return g_OriginalCreate(DeviceObject, Irp);
        }
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
NTSTATUS
AudientControlClose(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
/*++

Routine Description:

    IRP_MJ_CLOSE for the control device. Nothing is freed here (all state is
    cleaned on CLEANUP so empty closes cannot leak). Forward non-control IRPs.

--*/
{
    PAGED_CODE();

    if (DeviceObject != g_AudientControlDevice)
    {
        if (g_OriginalClose != NULL)
        {
            return g_OriginalClose(DeviceObject, Irp);
        }
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
NTSTATUS
AudientControlCleanup(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
/*++

Routine Description:

    IRP_MJ_CLEANUP for the control device. This is the deterministic cleanup
    point on handle close / process exit: if the closing handle is the connected
    producer, the ring is unpublished and its pages are released (the transport
    then serves digital silence).

--*/
{
    PAGED_CODE();

    if (DeviceObject != g_AudientControlDevice)
    {
        if (g_OriginalCleanup != NULL)
        {
            return g_OriginalCleanup(DeviceObject, Irp);
        }
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    PFILE_OBJECT fileObject = IoGetCurrentIrpStackLocation(Irp)->FileObject;

    if (g_AudientProducerConnected &&
        fileObject != NULL &&
        fileObject == g_AudientConnectedFile)
    {
        AudientControlPlaneUnbindProducer();
    }

    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// IOCTL handlers.
// ---------------------------------------------------------------------------

static
NTSTATUS
AudientControlQueryCaps(
    _Inout_ PVOID SystemBuffer,
    _In_ ULONG InputBufferLength,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG BytesReturned
    )
{
    UNREFERENCED_PARAMETER(InputBufferLength);

    if (OutputBufferLength < sizeof(audient::capture_control::CaptureControlCaps))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    audient::capture_control::CaptureControlCaps* caps =
        static_cast<audient::capture_control::CaptureControlCaps*>(SystemBuffer);
    RtlZeroMemory(caps, sizeof(*caps));

    caps->magic = audient::capture_control::CONTROL_PROTOCOL_MAGIC;
    caps->protocolVersion = audient::capture_control::CONTROL_PROTOCOL_VERSION;
    caps->formatTag = audient::capture_control::CONTROL_FORMAT_TAG;
    caps->sampleRateHz = audient::capture_control::CONTROL_SAMPLE_RATE_HZ;
    caps->channels = audient::capture_control::CONTROL_CHANNELS;
    caps->blockFrames = audient::capture_control::CONTROL_BLOCK_FRAMES;
    caps->capacityFrames = audient::capture_control::CONTROL_CAPACITY_FRAMES;
    caps->regionBytes = audient::capture_control::CaptureControlRegionBytes();

    *BytesReturned = sizeof(*caps);
    return STATUS_SUCCESS;
}

static
NTSTATUS
AudientControlConnect(
    _Inout_ PVOID SystemBuffer,
    _In_ ULONG InputBufferLength,
    _In_ ULONG OutputBufferLength,
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PFILE_OBJECT FileObject,
    _Out_ PULONG BytesReturned
    )
/*++

Routine Description:

    IOCTL_AUDIENT_CAPTURE_CONNECT. Validates the negotiated profile, locks the
    client's mapped ring pages, initializes the shared region header for a fresh
    epoch, assigns a new generation, marks CONNECTED, and publishes the kernel
    address to the capture source. Single producer enforced.

--*/
{
    UNREFERENCED_PARAMETER(DeviceObject);

    if (g_AudientProducerConnected)
    {
        return STATUS_DEVICE_BUSY;
    }

    if (InputBufferLength < sizeof(audient::capture_control::CaptureControlConnectRequest))
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    if (OutputBufferLength < sizeof(audient::capture_control::CaptureControlConnectResponse))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    PAUDIENT_CAPTURE_REGION region = NULL;
    PMDL mdl = NULL;
    NTSTATUS ntStatus = STATUS_SUCCESS;
    BOOLEAN mdlLocked = FALSE;
    __try
    {
        audient::capture_control::CaptureControlConnectRequest* req =
            static_cast<audient::capture_control::CaptureControlConnectRequest*>(SystemBuffer);

        const int profileResult = audient::capture_control::CaptureControlValidateProfile(
            req, req->regionBytes);
        if (profileResult != audient::capture_control::CONTROL_PROFILE_OK)
        {
            DPF(D_TERSE, ("AudientControlPlane: CONNECT rejected profile code %d", profileResult));
            ntStatus = (profileResult == audient::capture_control::CONTROL_PROFILE_BAD_USER_BASE ||
                        profileResult == audient::capture_control::CONTROL_PROFILE_BAD_USER_BYTES)
                ? STATUS_INVALID_USER_BUFFER
                : STATUS_INVALID_PARAMETER;
            __leave;
        }

        const audient::capture_ring::U64 regionBytes =
            audient::capture_control::CaptureControlRegionBytes();
        const audient::capture_ring::U64 userBytes = req->userBytes;

        if (userBytes > static_cast<audient::capture_ring::U64>(ULONG_MAX))
        {
            ntStatus = STATUS_INVALID_PARAMETER;
            __leave;
        }

        // Build + probe + lock the user's mapped section. This is the ONLY place
        // a user-mode address is touched, and it happens at PASSIVE_LEVEL in the
        // control path. The published descriptor stores only the SYSTEM address.
        mdl = IoAllocateMdl(
            reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(req->userBase)),
            static_cast<ULONG>(userBytes),
            FALSE,
            FALSE,
            NULL);
        if (mdl == NULL)
        {
            ntStatus = STATUS_INSUFFICIENT_RESOURCES;
            DPF(D_TERSE, ("AudientControlPlane: IoAllocateMdl failed"));
            __leave;
        }

        MmProbeAndLockPages(mdl, UserMode, IoModifyAccess);
        mdlLocked = TRUE;

        // Allocate the descriptor in nonpaged pool: the DMA path reads it at
        // DISPATCH and the pages it references are locked by the MDL.
        region = static_cast<PAUDIENT_CAPTURE_REGION>(
            ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(AUDIENT_CAPTURE_REGION), AUDIENT_CONTROL_POOLTAG));
        if (region == NULL)
        {
            ntStatus = STATUS_INSUFFICIENT_RESOURCES;
            DPF(D_TERSE, ("AudientControlPlane: region descriptor allocation failed"));
            __leave;
        }
        RtlZeroMemory(region, sizeof(AUDIENT_CAPTURE_REGION));

        region->Mdl = mdl;
        region->Bytes = static_cast<ULONG>(regionBytes);
        region->SystemAddress = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
        if (region->SystemAddress == NULL)
        {
            ntStatus = STATUS_INSUFFICIENT_RESOURCES;
            ExFreePool(region);
            region = NULL;
            DPF(D_TERSE, ("AudientControlPlane: MmGetSystemAddressForMdlSafe failed"));
            __leave;
        }
        // The descriptor now owns the MDL; null the parallel local so the failure
        // cleanup below (which uses the descriptor's Mdl pointer) is not confused.
        mdl = NULL;

        // Initialize the shared region header: fresh empty epoch with the
        // negotiated geometry, a new generation, and CONNECTED set. This happens
        // BEFORE publishing so the DMA path can never see an uninitialized header.
        audient::capture_ring::CaptureRingHeader* header =
            static_cast<audient::capture_ring::CaptureRingHeader*>(region->SystemAddress);
        const audient::capture_ring::U64 regionBytesU64 =
            static_cast<audient::capture_ring::U64>(region->Bytes);

        if (!audient::capture_ring::CaptureRingInit(
                header,
                regionBytesU64,
                audient::capture_control::CONTROL_CAPACITY_FRAMES,
                audient::capture_control::CONTROL_BLOCK_FRAMES))
        {
            ntStatus = STATUS_INVALID_PARAMETER;
            __leave;
        }

        // Generation: 1-based attach epochs (0 == never attached). Monotonic.
        const ULONGLONG generation = AudientControlPlaneNextGeneration();

        header->generation = generation;
        header->flags |= audient::capture_ring::REGION_FLAG_CONNECTED;
        audient::capture_ring::CaptureRingFence();

        // Publish the descriptor. If a stale region somehow existed, it is
        // retired and staged for free; we free it right here (control plane).
        PAUDIENT_CAPTURE_REGION previous = NULL;
        AudientConsoleCaptureRingPublish(region, &previous);
        if (previous != NULL)
        {
            AudientConsoleCaptureRingRetireWaitAndFree(previous);
        }

        g_AudientProducerConnected = TRUE;
        g_AudientConnectedDevice = DeviceObject;
        g_AudientConnectedFile = FileObject;

        audient::capture_control::CaptureControlConnectResponse* response =
            static_cast<audient::capture_control::CaptureControlConnectResponse*>(SystemBuffer);
        RtlZeroMemory(response, sizeof(*response));
        response->status = STATUS_SUCCESS;
        response->generation = generation;
        response->connected = 1;
        response->activeProducers = 1;

        *BytesReturned = sizeof(*response);
        DPF(D_TERSE, ("AudientControlPlane: CONNECT ok gen=%llu", (unsigned long long)generation));
        return STATUS_SUCCESS;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ntStatus = STATUS_INVALID_USER_BUFFER;
    }

    // Cleanup on failure (the descriptor owns the MDL once it has one assigned;
    // the local `mdl` variable is a parallel alias we null once ownership moves).
    if (region != NULL)
    {
        if (region->Mdl != NULL)
        {
            if (mdlLocked)
            {
                MmUnlockPages(region->Mdl);
            }
            IoFreeMdl(region->Mdl);
            region->Mdl = NULL;
            mdl = NULL;
        }
        ExFreePool(region);
    }
    if (mdl != NULL && mdlLocked)
    {
        MmUnlockPages(mdl);
        IoFreeMdl(mdl);
    }
    else if (mdl != NULL)
    {
        IoFreeMdl(mdl);
    }

    DPF(D_TERSE, ("AudientControlPlane: CONNECT failed 0x%x", ntStatus));
    return ntStatus;
}

static
NTSTATUS
AudientControlDisconnect(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PFILE_OBJECT FileObject
    )
/*++

Routine Description:

    IOCTL_AUDIENT_CAPTURE_DISCONNECT. Only the connected producer may
    disconnect. Unpublishes + staged-frees the region; subsequent fills serve
    exact digital silence.

--*/
{
    UNREFERENCED_PARAMETER(DeviceObject);

    if (!g_AudientProducerConnected)
    {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (FileObject != g_AudientConnectedFile)
    {
        return STATUS_ACCESS_DENIED;
    }

    AudientControlPlaneUnbindProducer();
    return STATUS_SUCCESS;
}

static
NTSTATUS
AudientControlFlush(
    _In_ PFILE_OBJECT FileObject
    )
/*++

Routine Description:

    IOCTL_AUDIENT_CAPTURE_FLUSH. Generation reset while staying connected: the
    active region is re-initialized to a fresh empty epoch with a new
    generation. In-flight fills drain under the manager spinlock before the
    header is reset, so no stale audio survives a generator/mix flush.

--*/
{
    if (!g_AudientProducerConnected)
    {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (FileObject != g_AudientConnectedFile)
    {
        return STATUS_ACCESS_DENIED;
    }

    const ULONGLONG generation = AudientControlPlaneNextGeneration();

    NTSTATUS status = AudientConsoleCaptureRingReset(generation);
    if (NT_SUCCESS(status))
    {
        DPF(D_TERSE, ("AudientControlPlane: FLUSH ok gen=%llu", (unsigned long long)generation));
    }
    return status;
}

static
NTSTATUS
AudientControlQueryState(
    _Inout_ PVOID SystemBuffer,
    _In_ ULONG OutputBufferLength,
    _Out_ PULONG BytesReturned
    )
/*++

Routine Description:

    IOCTL_AUDIENT_CAPTURE_QUERY_STATE. Snapshot of the transport counters from
    the active region (or all-zero when disconnected).

--*/
{
    if (OutputBufferLength < sizeof(audient::capture_control::CaptureControlState))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    audient::capture_control::CaptureControlState* state =
        static_cast<audient::capture_control::CaptureControlState*>(SystemBuffer);
    RtlZeroMemory(state, sizeof(*state));

    state->protocolVersion = audient::capture_control::CONTROL_PROTOCOL_VERSION;

    PAUDIENT_CAPTURE_REGION region = AudientConsoleGetCaptureRingRegion();
    if (region != NULL && region->SystemAddress != NULL)
    {
        audient::capture_ring::CaptureRingHeader* header =
            static_cast<audient::capture_ring::CaptureRingHeader*>(region->SystemAddress);
        state->generation = header->generation;
        state->writePos = header->writePos;
        state->readPos = header->readPos;
        state->producedSamples = header->producedSamples;
        state->consumedSamples = header->consumedSamples;
        state->overflowDrops = header->overflowDrops;
        state->staleCatchupDrops = header->staleCatchupDrops;
        state->underruns = header->underruns;
        state->generationFlushes = header->generationFlushes;
        state->connected = ((header->flags & audient::capture_ring::REGION_FLAG_CONNECTED) != 0) ? 1 : 0;
    }

    *BytesReturned = sizeof(*state);
    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
NTSTATUS
AudientControlDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp
    )
/*++

Routine Description:

    IRP_MJ_DEVICE_CONTROL. Because the driver object's major-function table is
    shared by every device object (audio FDO and the control DO), the ioctl CODE
    is owned by the control plane regardless of which DO it targets. Our codes
    are handled here; every other code (audio/KS filter stack) is forwarded to
    the original portcls handler. METHOD_BUFFERED, PASSIVE_LEVEL; never blocks
    the realtime path.

--*/
{
    PAGED_CODE();

    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    ULONG ioctl = stack->Parameters.DeviceIoControl.IoControlCode;

    const BOOLEAN ours =
        ioctl == IOCTL_AUDIENT_CAPTURE_QUERY_CAPS ||
        ioctl == IOCTL_AUDIENT_CAPTURE_CONNECT ||
        ioctl == IOCTL_AUDIENT_CAPTURE_DISCONNECT ||
        ioctl == IOCTL_AUDIENT_CAPTURE_FLUSH ||
        ioctl == IOCTL_AUDIENT_CAPTURE_QUERY_STATE;

    if (!ours || g_OriginalDeviceControl == NULL)
    {
        if (DeviceObject == g_AudientControlDevice)
        {
            // Unknown code on the control device: reject rather than forward.
            Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
            Irp->IoStatus.Information = 0;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        if (g_OriginalDeviceControl != NULL)
        {
            return g_OriginalDeviceControl(DeviceObject, Irp);
        }
        Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    PVOID systemBuffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG inputLength = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outputLength = stack->Parameters.DeviceIoControl.OutputBufferLength;
    PFILE_OBJECT fileObject = stack->FileObject;
    ULONG bytesReturned = 0;

    NTSTATUS ntStatus;

    switch (ioctl)
    {
    case IOCTL_AUDIENT_CAPTURE_QUERY_CAPS:
        ntStatus = AudientControlQueryCaps(systemBuffer, inputLength, outputLength, &bytesReturned);
        break;

    case IOCTL_AUDIENT_CAPTURE_CONNECT:
        ntStatus = AudientControlConnect(
            systemBuffer, inputLength, outputLength, DeviceObject, fileObject, &bytesReturned);
        break;

    case IOCTL_AUDIENT_CAPTURE_DISCONNECT:
        ntStatus = AudientControlDisconnect(DeviceObject, fileObject);
        break;

    case IOCTL_AUDIENT_CAPTURE_FLUSH:
        ntStatus = AudientControlFlush(fileObject);
        break;

    case IOCTL_AUDIENT_CAPTURE_QUERY_STATE:
        ntStatus = AudientControlQueryState(systemBuffer, outputLength, &bytesReturned);
        break;

    default:
        ntStatus = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = ntStatus;
    Irp->IoStatus.Information = bytesReturned;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return ntStatus;
}

// ---------------------------------------------------------------------------
// Init / teardown.
// ---------------------------------------------------------------------------

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AudientControlPlaneInitialize(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    PAGED_CODE();

    NTSTATUS ntStatus = STATUS_SUCCESS;

    // Start the refcounted region manager (spinlock init) before anything else.
    AudientConsoleCaptureRingManagerInit();

    // Save the portcls handlers we replace. PcInitializeAdapterDriver must have
    // already run (it typically installs PcDispatchIrp for these majors).
    g_OriginalCreate = DriverObject->MajorFunction[IRP_MJ_CREATE];
    g_OriginalClose = DriverObject->MajorFunction[IRP_MJ_CLOSE];
    g_OriginalCleanup = DriverObject->MajorFunction[IRP_MJ_CLEANUP];
    g_OriginalDeviceControl = DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];

    if (g_OriginalCreate == NULL || g_OriginalDeviceControl == NULL)
    {
        DPF(D_ERROR, ("AudientControlPlane: portcls dispatch not installed yet"));
        return STATUS_UNSUCCESSFUL;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = AudientControlCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = AudientControlClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = AudientControlCleanup;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = AudientControlDeviceControl;

    ntStatus = AudientControlPlaneCreateControlDevice(DriverObject);
    if (!NT_SUCCESS(ntStatus))
    {
        // Restore the portcls handlers so the driver still works without a
        // control device; surface the error to DriverEntry.
        DriverObject->MajorFunction[IRP_MJ_CREATE] = g_OriginalCreate;
        DriverObject->MajorFunction[IRP_MJ_CLOSE] = g_OriginalClose;
        DriverObject->MajorFunction[IRP_MJ_CLEANUP] = g_OriginalCleanup;
        DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = g_OriginalDeviceControl;
        return ntStatus;
    }

    return STATUS_SUCCESS;
}

#pragma code_seg("PAGE")
VOID
AudientControlPlanePause(
    VOID
    )
/*++

Routine Description:

    Unpublish + staged-free any active producer region WITHOUT deleting the
    control device/interface. Called for PnP STOP_DEVICE (resource rebalance,
    sleep) so a later START can CONNECT again. The DMA path serves silence
    between pause and the next CONNECT.

--*/
{
    PAGED_CODE();

    if (g_AudientProducerConnected)
    {
        AudientControlPlaneUnbindProducer();
    }
}

#pragma code_seg("PAGE")
VOID
AudientControlPlaneShutdown(
    VOID
    )
/*++

Routine Description:

    Full teardown: pause, then delete the control device + interface. Called
    from the adapter driver unload path after portcls has finished tearing the
    device down.

--*/
{
    PAGED_CODE();

    AudientControlPlanePause();

    if (g_AudientControlSymbolicLinkValid && g_AudientControlSymbolicLink.Buffer != NULL)
    {
        (void)IoSetDeviceInterfaceState(&g_AudientControlSymbolicLink, FALSE);
        RtlFreeUnicodeString(&g_AudientControlSymbolicLink);
        g_AudientControlSymbolicLinkValid = FALSE;
    }

    if (g_AudientControlDevice != NULL)
    {
        UNICODE_STRING symlinkName;
        RtlInitUnicodeString(&symlinkName, AUDIENT_CONTROL_SYMLINK_NAMES);
        (void)IoDeleteSymbolicLink(&symlinkName);
        IoDeleteDevice(g_AudientControlDevice);
        g_AudientControlDevice = NULL;
    }
}