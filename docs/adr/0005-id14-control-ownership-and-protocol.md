# ADR-005 — iD14 MK1 hardware-control ownership and protocol policy

- Status: Accepted (2026-09-03, Phase 3 slice A)
- Scope: `src/id14-control/*` and the decision boundary between semantic commands
  and verified MK1 protocol bytes.
- Related: `project guidelines` §10, `project plan` Phase 3.

## Context

The Audient iD14 MK1 exposes several dashboard controls (speaker/headphone level,
mute, dim, mono, cue/direct-monitor mix, output assignment, iD buttons) through the
official iD Mixer. Audient does not publish a documented MK1 control API for this
device, so the project must treat the protocol boundary with high-risk
interoperability rules (`project guidelines` §10): no speculative raw writes, no brute-force
discovery, no copying commands from another device revision without exact-tuple proof.

The engine (Phase 2) and future VST chain (Phase 4) must never produce device-control
bytes. Only a dedicated verified MK1 protocol adapter may.

## Decision

- All hardware control flows through a **typed semantic control model** that is
  independent of raw protocol bytes. UI/session code expresses
  `speakerVolume.set(0.5)`-style semantic commands only.
- A **strict command whitelist** sits between the semantic model and the transport.
  Unknown command IDs, payload shapes, and out-of-range values are rejected before
  any byte leaves the process.
- A **verified MK1 protocol adapter** translates whitelisted semantic commands into
  device writes. It is the only component allowed to emit raw bytes.
- `Id14ControlTransport` is the serialized, single-writer, non-realtime worker that
  owns the device handle, acknowledgment/timeout/retry correlation, and
  disconnect/reconnect identity revalidation. It is never callable from the ASIO
  callback (`project guidelines` §10.2).
- Persistence stores **semantic state**, never a UI-owned raw command stream.
- Raw protocol captures are test artifacts only; they are never logged continuously
  and never shipped.
- No production path may address firmware, calibration, serial, factory, bootloader,
  or unknown persistent-memory regions.

## Consequences

- Safe fallback: until a control is proven safe on the exact hardware/driver/firmware
  tuple, it remains `unverified`, `physical-only`, or `read-only` and is non-invokable.
- The simulator/transport layer is testable without the device and without protocol
  discovery, which decouples Phase 3 model work from field observation.
- Broad device writes still require: one read-only identity/state query proven safe,
  one low-risk reversible write proven safe, and explicit user consent on the
  physical device before custom-control runs.