# ADR-010 — 48 kHz sample rate is locked; ASIO buffer size is the only user timing parameter

- Status: Accepted (2026-09-13)
- Scope: Daily GUI audio settings (top-bar status capsule + Audio Settings popover) and
  the app-side ASIO reconfigure path.
- Related: `project guidelines` §3 (non-negotiable 48 kHz internal processing), §8 (ASIO/physical
  routing), §17 (testing); `project plan` Phase 6 worklog; ADR-009.

## Context

The iD.exe application exposes both "Set ASIO Buffer Size" and "Set Sample Rate"
(44.1/48/88.2/96 kHz). An earlier request was to expose both in Audient Console.
Investigation showed the current engine, VST preparation, the `VirtualCaptureTransport`
and `SharedRingCaptureSink` virtual-mic transport, and the Q5-A3B driver control plane are
all built and hardware-proven around **48 kHz** (mono float32). The non-negotiable product
decision already states internal processing is 32-bit float at 48 kHz.

Threading any ASIO rate other than 48 kHz through the engine would require a
sample-rate-conversion stage and/or a renegotiation of the frozen virtual-mic transport
contract — an audio-path change, not a GUI change.

## Decision

1. **Sample rate is locked to 48 kHz** for this phase. Audient Console does not initiate a
   sample-rate change and does not offer 44.1/88.2/96 kHz as selectable options.
2. **ASIO buffer size is the only user-configurable timing parameter.** The top-bar
   `48 kHz / NN samples` capsule opens an Audio Settings popover listing only the buffer
   candidates the driver actually advertises and the engine can honor
   (`[min,max]` ∩ powers-of-two/granularity ∩ `buffer ≤ kMaxBlock`; 512 is not offered
   because `kMaxBlock == 256`).
3. **The Audient ASIO driver / iD14 is authoritative for the buffer.** Both directions are
   supported:
   - Console-initiated: validated request → control-thread graceful teardown → rebuild at
     the requested buffer → read back the actual buffer → publish it.
   - External (iD.exe): the driver's buffer-size-change notification and a bounded poll of
     the driver's actual buffer detect the change; the app adopts it (if within the engine
     ceiling) and reconciles via a safe rebuild, then publishes the real active buffer.
   The requested value is never assumed to have succeeded until driver readback confirms it.
4. **48 kHz guard.** If the driver externally reports a rate other than 48 kHz, the app must
   not silently continue as if the mode were supported. It logs an explicit warning and
   restores 48 kHz through the proven rebuild path; general multi-rate reconciliation is not
   implemented.
5. **Diagnostics distinguish planned vs genuine.** Planned reconfigurations are counted
   separately (`reconfigures`) from device-loss recoveries (`losses`/`recoveries`) so the two
   can never be conflated.

## Consequences

- Buffer changes are an explicit, user-initiated action with a brief (~0.75 s) stream gap;
  VST chain, transport, meters, monitor routing, and hardware volume state are preserved by
  the existing re-wire path, and the actual buffer is read back and shown.
- Sample-rate changes can only come from the device/iD.exe; the app reflects and, if needed,
  restores 48 kHz. Sample-rate switching remains out of scope pending a future ADR that also
  addresses SRC and the virtual-mic transport contract.
- No new realtime work: all reconfigure logic runs on the control/watchdog thread; the
  driver notification handler performs atomic stores only.
