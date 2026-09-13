# ADR-011 — Selectable single-source virtual microphone (Input 1 or Input 2)

- Status: Accepted (2026-09-13)
- Scope: User-selectable virtual-microphone source, the Settings panel Audio section, and
  persistent application preferences.
- Related: `project guidelines` §3 (product decisions), §4 (canonical signal paths/terminology),
  §7 (realtime rules), §8 (ASIO/physical routing), §14 (state), §15 (UI), §17 (testing);
  ADR-010 (48 kHz locked); the accepted Pico v0.1.1 virtual-mic transport integration.

## Context

Through the accepted Daily/Pico work the virtual microphone was hard-wired to physical
Input 1 (runtime slot 0): the adapter's per-channel `virtualMicSend` had channel 0 enabled
and channel 1 disabled, and the mic uplink was the processed slot 0 signal. The earlier
frozen wording ("CH0 is the ONLY Virtual Mic source; CH1 must never leak") was a v1 scope
decision made before the Phase 6 Settings surface.

The user requested a real Settings panel whose main new control is a user-selectable virtual
microphone source: exactly one of Input 1 / Input 2, default Input 1, with no both/mix mode
in this phase. This changes a previously frozen product decision, so it requires an explicit
ADR and the `project guidelines` revision recorded here.

The engine already contains the correct realtime seam: `AsioRoutingAdapter` sums per-channel
`inputProcessed[slot]` through the ramped `m_virtualMicSendRamp[slot]` gains
(`CaptureMixer::mixMonoRamped`) into the mono `m_micUplink`, and the gating is derived from
each channel's control-published `ChannelRuntimeSnapshot.virtualMicSend`. Selecting a single
source therefore needs no audio-path change: publish exactly one enabled send.

## Decision

1. **Single-source invariant.** Exactly one processed physical input channel feeds the
   virtual microphone at any time. The user selects the **Virtual Mic Source** as
   `Input 1` (runtime slot 0) or `Input 2` (runtime slot 1). The default is `Input 1`.
2. **Mutually exclusive sends.** The app publishes exactly one enabled `virtualMicSend`:
   - Input 1 selected: slot 0 `virtualMicSend.enabled = true`, slot 1 `= false`.
   - Input 2 selected: slot 0 `virtualMicSend.enabled = false`, slot 1 `= true`.
   Both channels are never enabled simultaneously and no summing/mix mode exists. The
   per-channel ramps make a live change click-free; the change bumps the owning channel
   revision so the adapter applies it at a block boundary.
3. **Availability.** `Input 2` is only selectable when two physical inputs are configured
   (dual plan). With a single configured input the effective source is Input 1 and the
   Input 2 option is presented as unavailable. A persisted Input 2 selection falls back to
   Input 1 when only one input is present.
4. **Selected signal is post-VST.** The selected channel's POST-VST processed signal is what
   reaches the existing feeder → Pico transport → Windows virtual microphone. Selection
   changes nothing else: RAW/POST meter semantics, per-channel VST chains, Local Monitor,
   physical monitor/downlink, the Pico WASAPI transport contract, ASIO lifetime, and
   buffer/rate behavior are unchanged.
5. **Presentation is not audio.** The Settings panel is presentation; all new controls
   validate on the WebView/UI thread and apply through the existing control-thread publish
   path. No realtime callback, USB, or device work is added.
6. **Persistent preferences.** A small user-preferences file
   (`%LOCALAPPDATA%\Audient Console\settings.json`) persists only user preferences
   (`virtualMicSource`, `closeToTray`, `startMinimized`). It never persists
   runtime/device source-of-truth state (actual rate/buffer, ASIO/Pico connection, xruns,
   overloads, hardware dB). Safe defaults on missing/corrupt file, unknown keys ignored,
   bounded schema, atomic temp+replace write, and persistence failure must never prevent
   audio startup. The retired `theme8bit` key (8-bit theme dropped from scope) is accepted
   and ignored so older files keep loading.

## Consequences

- The virtual microphone can be sourced from either analogue input while preserving
  single-source isolation (no leakage, no sum). The unselected channel never reaches the
  virtual mic or the Pico.
- A new invariant test surface is required: the send-selection policy is unit-tested, and the
  capture mixer's `[1,0]`/`[0,1]` ramped-mix seam is tested to yield exactly the selected
  channel and never the sum.
- `project guidelines` §3/§4 wording is revised; the old "CH0 only" statement is superseded.
- `Monitor HW / Headphones HW` restoration remains a separate future phase and is out of
  scope here.
