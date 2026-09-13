# ADR-006 — Windows virtual capture backend foundation

- Status: Accepted (2026-09-04, Phase 5 slice Q2)
- Scope: `src/virtual_audio/*` and the decision boundary between the user-space
  engine transports and the future Windows virtual microphone endpoint.
- Related: `project guidelines` §4, §11, §18; `project plan` Phase 7/8; Slice Q1
  (`transport::VirtualCaptureTransport`).

## Context

The processed mic uplink now leaves the ASIO <-> RoutingCore adapter through a
realtime-safe, freshness-aware `transport::VirtualCaptureTransport` (Slice Q1).
The next architectural fact needed is the Windows Virtual Microphone backend:
`VirtualCaptureTransport -> Windows virtual capture endpoint -> "Microphone
(Audient Console)"`. This ADR fixes the foundation decisions so the future
kernel-endpoint and app-bridge work has a stable boundary.

Two conflicts/decisions surfaced while starting the slice:
1. The Q2 request named the endpoint `"AudientConsole Mic FX"`, but `project guidelines` §4
   pins the v1 capture endpoint friendly name as **`Microphone (Audient Console)`**.
2. No WDK (km libs/toolsets) is installed on the daily machine, and `project guidelines`
   §18 forbids test-signed driver installs on it.

## Decision

- **Endpoint name**: keep the canonical `Microphone (Audient Console)`
  (user-confirmed 2026-09-04). `"AudientConsole Mic FX"` is not shipped.
- **Boundary**: all virtual-audio work lives under `src/virtual_audio/`,
  separated into `endpoint/` (user-space contract + software endpoint) and
  `driver/` (future kernel endpoint). No driver-specific code may be added to
  `src/routing`, `src/vst3`, or `src/transport`; the pull contract is the only
  seam (`src/transport` -> `src/virtual_audio`; never the reverse).
- **Base sample** (user priority: minimal capture endpoint): the Microsoft
  **Simple Audio Sample Device Driver** (`audio/simpleaudiosample` in
  `microsoft/Windows-driver-samples`, branch `main`, pinned by blob SHA in
  `src/virtual_audio/driver/README.md`). It is a port-class/WaveRT **virtual**
  audio device exposing an embedded speaker + mic array with no hardware —
  the smallest official capture surface. Chosen over `audio/sysvad` (larger)
  and the ACX framework sample. License: **MS-PL** (permissive; derived source
  must retain Microsoft notices and be distributed under MS-PL). Nothing is
  vendored in this slice; the pin is recorded for the Phase 8 adaptation.
- **Slice Q2 scope**: the user-space endpoint *contract*
  (`endpoint/VirtualCaptureEndpoint.h`) and a testable **software endpoint**
  (`endpoint/SoftwareCaptureEndpoint`) that implements the same pull behavior a
  WaveRT capture miniport must later honor over the Q1 transport (fresh-only
  delivery, digital silence on absence, counted drops). The kernel driver and
  its install path are Phase 8 and stay on a WDK/VM test machine.

## Consequences

- The endpoint contract is proven today (255/255 tests across the 3 configs),
  without the WDK, on the daily machine.
- The future driver adaptation is largely independent: the capture stream must
  source its "virtual mixer" buffer from the same transport-backed fresh policy.
- Any later distribution of the derived driver must satisfy MS-PL section 3
  (attribution notices retained; source distributed under MS-PL).
- No change to `project plan` Phase 7/8 gates: production kernel integration still
  requires the Phase 0 signing route and Phase 7 transport contract gates.