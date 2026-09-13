# src/virtual_audio — Windows Virtual Microphone backend boundary

Slice Q2. This module owns everything between the realtime engine and the future
Windows virtual microphone endpoint:

```text
RoutingCore::MicUplink
        ↓
AsioRoutingAdapter (publish)          [src/asio]
        ↓
VirtualCaptureTransport              [src/transport  — untouched by Q2/Q3]
        ↓
VirtualCaptureEndpoint contract      [src/virtual_audio/endpoint]
        ↓
VirtualMicFeeder                     [src/virtual_audio/driver-protocol]
        ↓
IDriverCaptureSink -> SharedRing     [src/virtual_audio/driver-protocol]
        ↓
Shared capture-ring region           [driver-protocol/CaptureRingContract.h]
        ↓
Windows virtual capture endpoint     [src/virtual_audio/driver — capture source]
```

`SharedRingCaptureSink` is the concrete Q5-A3 producer sink over the versioned
shared capture ring; it is a drop-in behavioral equivalent of
`MockDriverCaptureSink` for the feeder (proven by the parametrized
VirtualMicFeederProtocolTest suite). The kernel capture source
(`driver/Source/Main/audientcapturesource.cpp`) consumes the SAME
`CaptureRingContract.h` region. From Q5-A3B a control plane
(`driver-protocol/CaptureControlPlane.h` + `audientcontrolplane.cpp`) publishes a
user-mode shared section into the driver and the DMA fill converts mono float32 →
clamped signed PCM32 L/R for the Windows WaveRT endpoint; verified end-to-end on
the test VM with a WASAPI shared-mode capture client.

## Boundary rules (user decisions 2026-09-04)

- **No driver-specific code** may be added to `src/routing`, `src/vst3`, or
  `src/transport`. The endpoint contract is the only seam (forward direction:
  `src/transport` -> `src/virtual_audio`; never the reverse).
- No Windows virtual-device implementation enters the routing core.
- No Discord-specific code anywhere in this module.
- Kernel-mode code lives only under `driver/` and is built/test-signed only on a
  WDK/VM test machine (AGENTS §11/§18) — never installed on this daily PC.

## Layout

| Path | Purpose |
|---|---|
| `endpoint/VirtualCaptureEndpoint.h` | The pure pull contract a Windows capture endpoint MUST honor (format, identity, freshness, silence-on-absence, counted drops). |
| `endpoint/SoftwareCaptureEndpoint.{h,cpp}` | User-mode implementation of the contract over `VirtualCaptureTransport` (testable today, no WDK). |
| `driver-protocol/IDriverCaptureSink.h` | Driver capture sink abstraction (bounded, generation-flush, non-blocking writes). |
| `driver-protocol/VirtualMicFeeder.{h,cpp}` | Worker-thread feeder: endpoint -> sink; fresh-only, bounded, reconnect-fresh, shutdown-safe. |
| `driver-protocol/MockDriverCaptureSink.{h,cpp}` | Test double + software driver sink (bounded ring, connect/disconnect, generation flush, client read side). |
| `driver-protocol/SharedRingCaptureSink.{h,cpp}` | Q5-A3 concrete producer sink over the shared capture ring (drop-in equivalent of the mock for the feeder). |
| `driver-protocol/CaptureRingContract.h` | THE versioned app↔driver ring contract (mono float32 SPSC POD + kernel/user-safe inline core; A3B adds `CaptureRingFloatToPcm32` + `CaptureRingConsumerFillPcm32Stereo`). Single canonical copy, compiled by user CMake AND the WDK driver. |
| `driver-protocol/CaptureControlPlane.h` | Q5-A3B control-plane ABI (stable interface GUID, IOCTLs, negotiated profile): the app hands the driver a locked shared ring. |
| `driver-protocol/CaptureControlClient.{h,cpp}` | Q5-A3B user-mode control-plane client (open/connect/disconnect/flush/query-state). |
| `driver/README.md` | The future kernel endpoint: pinned Microsoft base sample, capture-stream adaptation points, WDK/VM build+test gate. |

## Identity

The v1 friendly name is `Microphone (Audient Console)` (AGENTS §4; confirmed by
the user 2026-09-04 — the "AudientConsole Mic FX" wire-art label is not shipped).