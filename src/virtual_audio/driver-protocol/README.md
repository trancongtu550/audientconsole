# driver-protocol/ — virtual microphone driver transport protocol (Slice Q3 + Q5-A3/A3B)

The application-side bridge that moves processed mic audio from the software
endpoint to the (future) Windows driver capture buffer:

```text
SoftwareCaptureEndpoint                       [endpoint/]
        ↓  captureMonoFresh()                  worker thread pulls FRESH blocks
VirtualMicFeeder                               [driver-protocol/]
        ↓  writeMono(mono, frames, generation, sequence)
IDriverCaptureSink                             [driver-protocol/]
        ↓  bounded drop-new + freshness ring; generation flush on reconnect
SharedRingCaptureSink        (Q5-A3 concrete sink, drop-in for the mock)
        ↓  mono float32 into the versioned shared region
CaptureRingContract.h                          [driver-protocol/  ← THE ABI]
        ↓  kernel capture source reads the SAME region
audientcapturesource.cpp                       [driver/ (A3B: region published by control plane)]
        ↓  mono float32 → clamped signed PCM32 L=R into the WaveRT capture DMA buffer
CaptureControlPlane.h / CaptureControlClient   [driver-protocol/  ← A3B control plane]
```

## Files

- `IDriverCaptureSink.h` — the Q3 sink abstraction (unchanged).
- `VirtualMicFeeder.{h,cpp}` — worker-thread feeder (unchanged).
- `MockDriverCaptureSink.{h,cpp}` — reference test double + software sink.
- `SharedRingCaptureSink.{h,cpp}` — **Q5-A3**: concrete `IDriverCaptureSink`
  backed by the shared capture ring. Holds (or adopts) a region initialized from
  `CaptureRingContract.h` and writes mono float32 blocks into it.
- `CaptureRingContract.h` — **Q5-A3**: the single canonical, versioned region
  layout + lock-free SPSC mono-float32 ring core. MUST compile unchanged in the
  user CMake build AND the WDK kernel build (no STL, no exceptions, x64
  volatile ordering). The driver is given this directory on its include path; no
  second copy exists. **Q5-A3B** added `CaptureRingFloatToPcm32` and
  `CaptureRingConsumerFillPcm32Stereo` (the Windows WaveRT endpoint is 2ch
  32-bit PCM; the ring stays mono float32 and the DMA fill converts/clamps).
- `CaptureControlPlane.h` — **Q5-A3B**: versioned control-plane ABI (interface
  GUID + IOCTLs QUERY_CAPS/CONNECT/DISCONNECT/FLUSH/QUERY_STATE), compiled
  unchanged in both builds. Enforces the single supported profile and one
  active producer.
- `CaptureControlClient.{h,cpp}` — **Q5-A3B**: user-mode control-plane client
  (SetupAPI discovery by GUID; connects a mapped shared-ring section to the
  driver, flushes, disconnects, queries counters). Never used from the ASIO
  realtime callback.

## Rules enforced here

- **No driver/IPC calls from the ASIO realtime thread.** The adapter only
  publishes into `VirtualCaptureTransport`; the feeder + sink are worker-side
  objects the adapter never references. Integration tests run a stalled and a
  disconnected sink while ASIO streams and assert 0 xruns.
- **Feeder runs on a normal worker thread** (`start/requestStop/stop`,
  `tick()` is unit-testable without a thread). Destructor joins.
- **Bounded memory, no stale replay.** The ring is a bounded drop-new
  transport; a stalled client rejects writes (counted) instead of growing.
- **Sink stall / disconnect never affects ASIO/VST.** writeMono is non-blocking
  accept/reject; the feeder counts rejects and keeps the endpoint drained.
- **Reconnect resumes with fresh audio, not old backlog.** `attachSink()`
  bumps the GENERATION and resets the endpoint to a fresh capture epoch; the
  sink flushes any buffered audio from a previous generation on change.
- **Generation / sequence / silence semantics from Q1/Q2.** Generation =
  attach/reconnect epoch; sequence = per-attach monotonic attempt index;
  silence = the feeder NEVER forwards synthesized silence (only real engine
  blocks) and the driver/client empty-read rule yields digital silence.
- **Counters, never silent drops:** feeder `rejectedBlocks` / `engineGaps`;
  sink `rejectedDisconnected` / `rejectedStall` / `generationFlushes`; ring
  `overflowDrops` / `staleCatchupDrops` / `underruns`. All are visible in the
  region header (monotonic) and in `SharedRingCaptureSink::snapshot()`.
- **One canonical ABI.** `CaptureRingContract.h` is the ONLY ring layout; it is
  compiled by the user-mode CMake target and by the WDK driver via an include
  path. If the layout must change, bump `REGION_VERSION` and migrate.

## Q5-A3B kernel boundary

The kernel side never depends on `IDriverCaptureSink`. The app-side sink writes
mono float32 into the region; the driver control plane
(`audientcontrolplane.cpp`) publishes a user-mode shared section, and the
capture source (`audientcapturesource.cpp`) reads it through
`CaptureRingContract.h` and converts mono → clamped L=R signed PCM32 into the
2-ch 32-bit PCM WaveRT capture DMA buffer. A3B publishes a REAL region (verified
end-to-end in the VM: known 1000 Hz patterns arrive at the WASAPI capture
client, silence before/after connect, exact-silence underrun, no-stale replay on
reconnect, generation reset, bounded counters). The consumer code path is proven
on the host by the same inline ring functions that the kernel TU links.

No WaveRT / WDK / INF / real driver / Discord code lives here.