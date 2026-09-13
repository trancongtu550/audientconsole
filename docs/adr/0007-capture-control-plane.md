# ADR-007 — Shared capture-ring control plane (Q5-A3B)

- Status: Accepted (2026-09-04, Phase 5 slice Q5-A3B)
- Scope: `src/virtual_audio/driver-protocol/CaptureControlPlane.h`,
  `src/virtual_audio/driver/Source/Main/audientcontrolplane.{h,cpp}`,
  `src/virtual_audio/driver/Source/Main/audientcapturesource.{h,cpp}`, and the
  user-mode client (`CaptureControlClient.{h,cpp}`) / VM capture tools.
- Related: `project guidelines` §7, §11, §18, §20; `project plan` Phase 5 (§9 step 3 →
  publish a real region); `ADR-006`; `CaptureRingContract.h` (Q5-A3).

## Context

Q5-A3 delivered the versioned, lock-free capture-ring contract and the concrete
user-mode producer sink, but the driver's capture source still served digital
silence behind a NULL region provider. Q5-A3B must expose the bounded ring to a
real app instance: the app opens a control channel, "connects" its shared
memory, and the kernel `AudientCaptureSourceFillContiguous` then consumes fresh
mono float32 from that memory into the WaveRT capture DMA buffer.

The dangerous part is thread/IRQL + lifetime safety. The DMA fill runs at
`DISPATCH_LEVEL` (timer DPC, `WriteBytes/TimerNotifyRT`); the app lives in a
normal priority user-mode process; the control plane runs at `PASSIVE_LEVEL` on
an app thread; and the region must outlive an arbitrary process exit / handle
close / USB-style disconnect / device removal without a use-after-free or a page
fault in the audio path.

## Decision

1. **Control plane = dedicated control `DEVICE_OBJECT` + IOCTLs**, not KS
   properties and not the audio filter stack. A separate device object (created
   in `DriverEntry` after `PcInitializeAdapterDriver`) exposes the stable
   interface GUID `{8ECC7B3A-4D29-4E7C-9611-A20C536E2140}` (single canonical copy
   in `CaptureControlPlane.h`). The driver's existing IRP dispatch is preserved:
   our four handlers (`CREATE/CLOSE/CLEANUP/DEVICE_CONTROL`) forward any IRP not
   targeting the control device to the saved portcls handler.

2. **Region memory is user-allocated but kernel-pinned via MDL.** The app creates
   a `CreateFileMapping` section of the negotiated byte size, maps a view, and
   passes the user-mode base + mapped length in the CONNECT request. The kernel
   never dereferences that user address in the audio path: CONNECT performs
   `IoAllocateMdl` + `MmProbeAndLockPages(UserMode, IoModifyAccess)` +
   `MmGetSystemAddressForMdlSafe`, and publishes **only the kernel (system)
   address** to the capture source. `DISPATCH_LEVEL` code reads a page-locked,
   system-space VA — no user pointer, no page fault.

3. **Lifetime safety = spinlock + refcounted descriptor.** All publish/unpublish/
   flush in the control plane and every DMA fill in the capture source serialize
   on one `KSPIN_LOCK`. The active region is an `AUDIENT_CAPTURE_REGION`
   descriptor (`RefCount`, `Retired`, `Mdl`, `SystemAddress`, `Bytes`). A fill
   takes a ref under the lock, does its bounded read through the locked system
   VA, then releases the ref; it never frees. Unpublish marks the descriptor
   `Retired` under the lock; the caller then bounds-spins until `RefCount == 0`
   (fills are sub-microsecond and the wait is on the control plane, not the
   realtime path) before `MmUnlockPages`/`IoFreeMdl`/`ExFreePool`. Therefore no
   in-flight fill can ever observe freed pages.

4. **Deterministic cleanup on process exit / handle close**: `IRP_MJ_CLEANUP`
   fires when the app's last handle to the control device closes (including
   process exit) and runs the same unpublish + staged-free path as DISCONNECT.
   Device removal (`IRP_MN_REMOVE_DEVICE` / surprise) calls
   `AudientControlPlanePause()` first, so the DMA path serves silence while the
   adapter tears down. Driver unload deletes the control device/interface after
   `gPCDriverUnloadRoutine`.

5. **Single active producer.** CONNECT from a second handle while one producer is
   connected returns `STATUS_DEVICE_BUSY`. Only the connected file object may
   DISCONNECT/FLUSH (else `STATUS_ACCESS_DENIED`).

6. **Negotiation and rejection** are deterministic and shared: the exact
   `CaptureControlValidateProfile` function in `CaptureControlPlane.h` is
   compiled UNCHANGED into the kernel CONNECT handler and the host unit tests
   (`CaptureControlPlaneTest.cpp`). A request with the wrong
   magic/version/format/rate/channels/block/capacity/regionBytes/userBytes/userBase
   is rejected before any page is locked or published.

7. **Generation model**: the kernel owns a monotonic attach-generation counter
   (1-based; 0 = never attached). CONNECT stamps the region header with the new
   generation and sets `CONNECTED`. FLUSH re-initializes the active region to a
   fresh empty epoch (under the manager spinlock, so the consumer never observes
   a torn reset) and returns the new generation. Reconnect therefore always
   produces a fresh epoch — no stale replay across app restarts or mixer
   flushes.

## Safety analysis (per required area)

- **Page lifetime / pinning**: `MmProbeAndLockPages` pins the section's physical
  pages for the MDL's lifetime. The driver holds the MDL until DISCONNECT / handle
  close / device removal, so every DISPATCH fill sees resident pages. The refcount
  guarantees a fill never reads pages the control plane is freeing.
- **Process exit**: closing the app handle triggers `IRP_MJ_CLEANUP` →
  unpublish + wait + free. If the app dies abruptly the I/O manager still delivers
  CLEANUP for its control-device handle. Even if it did not, the MDL keeps the
  pages resident and the ring simply underruns to silence — never a dangling
  system VA in the audio path.
- **Driver unload**: removal calls `AudientControlPlanePause()` (unpublish →
  silence) before `PcDispatchIrp`; the permanent teardown (delete device +
  interface + free) runs in the driver unload path after portcls has stopped all
  streams, so no DMA fill can be in flight at free time.
- **Concurrent disconnect**: unpublish and every fill serialize on the shared
  spinlock; the staged-free waits for `RefCount == 0`. A DISCONNECT that arrives
  mid-fill is deterministic: the in-flight fill completes against still-valid,
  locked pages, then the descriptor is freed.
- **IRQL**: all IOCTLs are `METHOD_BUFFERED` at `PASSIVE_LEVEL`; the only
  kernel-mode accesses to the region geometry/header happen in the control path
  (PASSIVE) and the fill path (DISPATCH), both against the pinned system VA. The
  fill path performs no allocation, no I/O, no blocking lock (bounded spinlock
  only), and no kernel transition.
- **Memory ordering**: the ring's frontier fields are `volatile` with
  `_ReadWriteBarrier()` fences (Q5-A3 contract), unchanged; the descriptor
  pointer + refcount + retire bit are guarded by the spinlock; the published
  generation/flag transitions use the same fence. This is an x64-only design (the
  product target is Windows 11 x64).
- **Bounds validation**: every IOCTL checks `METHOD_BUFFERED` sizes;
  `CaptureControlValidateProfile` rejects any non-negotiated geometry before
  `MmProbeAndLockPages`; the fill re-validates the header (`CaptureRingValidate`)
  on every call and refuses non-2ch-float32 stream geometry.

## Consequences

- The capture DMA is now backed by a real, pinned, bounded shared ring when the
  app connects; no app present / disconnected / crashed → exact digital silence.
- The ASIO realtime callback remains untouched: the app worker
  (`VirtualMicFeeder → SharedRingCaptureSink`) is still the producer boundary; the
  control-plane client is never called from the callback.
- Testability: the profile validator is host-tested; the control plane is
  exercised end-to-end in the VM (lifecycle probe + WASAPI shared-mode capture
  verifier). Kernel `main` on the daily PC is unaffected.
- Remaining (NOT this slice): mono-float32 endpoint identity (Q5-A4), the full
  app-bridge integration in `src/virtual_audio/endpoint`, driver install/signing
  evidence, and production Dev-Portal signing. The InfVerif x86-engine note is
  resolved by building with the amd64 MSBuild node (0 warnings / 0 errors).