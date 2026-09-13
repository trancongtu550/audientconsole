/*++

Copyright (c) Microsoft Corporation All Rights Reserved
Copyright (c) 2026 Audient Console contributors

Module Name:

    audientcontrolplane.h

Abstract:

    Q5-A3B control-plane API for the AudientConsoleMic capture-only WaveRT
    driver. See audientcontrolplane.cpp and CaptureControlPlane.h.

    The control plane exposes a stable device-interface GUID to user mode. A
    dedicated DEVICE_OBJECT (created by this module) owns the interface; IRPs
    targeted at it are handled here, every other IRP is forwarded to the
    original portcls dispatch. It implements:

      IOCTL_AUDIENT_CAPTURE_QUERY_CAPS   profile negotiation (single profile)
      IOCTL_AUDIENT_CAPTURE_CONNECT      lock+init+publish the shared ring
      IOCTL_AUDIENT_CAPTURE_DISCONNECT   unpublish + staged free + flush
      IOCTL_AUDIENT_CAPTURE_FLUSH        generation reset (fresh epoch)
      IOCTL_AUDIENT_CAPTURE_QUERY_STATE  transport counters snapshot

    Realtime rules (AGENTS §7): no control-plane call runs inside the ASIO
    callback or the DISPATCH DMA fill; the fill path (audientcapturesource.cpp)
    only reads page-locked kernel mappings and never issues any IOCTL.

--*/

#ifndef _AUDIENTCONTROLPLANE_H_
#define _AUDIENTCONTROLPLANE_H_

// Install the four dispatch entries that matter for the control device
// (CREATE/CLOSE/CLEANUP/DEVICE_CONTROL). Saves the original portcls handlers so
// non-control IRPs are forwarded untouched. Call once from DriverEntry AFTER
// PcInitializeAdapterDriver.
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AudientControlPlaneInitialize(
    _In_ PDRIVER_OBJECT DriverObject
    );

// Register + enable the stable control-plane device-interface GUID on the
// PHYSICAL device object (the actual PDO, available from the adapter start
// path). Call from StartDevice after the adapter common object exists. The
// control device object itself exposes the "AudientConsoleControl" symlink; the
// PDO interface is used by SetupAPI-based discovery.
_IRQL_requires_max_(PASSIVE_LEVEL)
NTSTATUS
AudientControlPlaneRegisterInterface(
    _In_ PDEVICE_OBJECT PhysicalDeviceObject
    );

// Unpublish + staged-free any connected producer region WITHOUT deleting the
// control device/interface (so the interface survives a PnP STOP and a later
// START can CONNECT again). Called from the adapter's stop/remove handling.
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
AudientControlPlanePause(
    VOID
    );

// Full teardown: pause, then delete the control device object + interface.
// Called from the adapter driver unload path (after portcls has torn down).
_IRQL_requires_max_(PASSIVE_LEVEL)
VOID
AudientControlPlaneShutdown(
    VOID
    );

#endif // _AUDIENTCONTROLPLANE_H_