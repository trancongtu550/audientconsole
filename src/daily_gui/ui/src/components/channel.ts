import { Meter } from "./meter";
import { Fader } from "./fader";
import { InsertRack } from "./insertRack";
import { post } from "../bridge";
import type { ChannelState } from "../types";

const MUTE_ICON = `<svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M1 1l22 22"/><path d="M9 9v3a3 3 0 0 0 5.12 2.12M15 9.34V4a1 1 0 0 0-2 0v1.34"/><path d="M17 16.95A7 7 0 0 1 5 12v-2m14 0v2a7 7 0 0 1-.11 1.23"/><line x1="12" y1="19" x2="12" y2="23"/><line x1="8" y1="23" x2="16" y2="23"/></svg>`;
const LOCAL_ICON = `<svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M10 13a5 5 0 0 0 7.54.54l3-3a5 5 0 0 0-7.07-7.07l-1.72 1.71"/><path d="M14 11a5 5 0 0 0-7.54-.54l-3 3a5 5 0 0 0 7.07 7.07l1.71-1.71"/></svg>`;
const PENCIL_ICON = `<svg viewBox="0 0 24 24" width="13" height="13" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 20h9"/><path d="M16.5 3.5a2.12 2.12 0 0 1 3 3L7 19l-4 1 1-4z"/></svg>`;

export class Channel {
  readonly el: HTMLElement;
  readonly rawMeter: Meter;
  readonly postMeter: Meter;
  private readonly gain: Fader;
  private readonly muteBtn: HTMLButtonElement;
  private readonly localBtn: HTMLButtonElement;
  private readonly rack: InsertRack;
  private lastGain = NaN;

  constructor(private readonly ch: number, panel: HTMLElement) {
    const inputLabel = ch === 0 ? "Mic/Line" : "Instrument";
    panel.innerHTML = `
      <div class="ch-head">
        <div class="ch-head-left">
          <span class="ch-title">CH ${ch + 1}</span>
          <button type="button" class="ch-edit" disabled title="Rename (not available)" aria-label="Rename channel">${PENCIL_ICON}</button>
        </div>
        <button type="button" class="input-select" disabled title="Input type (not yet configurable)">
          <span>${inputLabel}</span>
          <svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><polyline points="6 9 12 15 18 9"></polyline></svg>
        </button>
      </div>
      <div class="panel-body"></div>`;
    const body = panel.querySelector(".panel-body") as HTMLElement;

    const meterRow = document.createElement("div");
    meterRow.className = "meter-row";
    this.rawMeter = new Meter("RAW", "left");
    this.postMeter = new Meter("POST", "right");
    this.gain = new Fader({
      min: -60,
      max: 0,
      value: -6,
      label: "MON",
      format: (v) => `${v.toFixed(1)} dB`,
      onChange: (v) => post({ cmd: "setGain", ch: this.ch, db: Number(v.toFixed(2)) }),
    });
    meterRow.appendChild(this.rawMeter.el);
    meterRow.appendChild(this.gain.el);
    meterRow.appendChild(this.postMeter.el);
    body.appendChild(meterRow);

    const actions = document.createElement("div");
    actions.className = "channel-actions";
    this.muteBtn = document.createElement("button");
    this.muteBtn.type = "button";
    this.muteBtn.className = "act-btn";
    this.muteBtn.title = "Mute channel";
    this.muteBtn.innerHTML = `${MUTE_ICON}<span>MUTE</span>`;
    this.localBtn = document.createElement("button");
    this.localBtn.type = "button";
    this.localBtn.className = "act-btn";
    this.localBtn.title = "Local monitor send";
    this.localBtn.innerHTML = `${LOCAL_ICON}<span>LOCAL</span>`;
    actions.appendChild(this.muteBtn);
    actions.appendChild(this.localBtn);
    body.appendChild(actions);

    this.muteBtn.addEventListener("click", () => {
      const on = !this.muteBtn.classList.contains("on-mute");
      post({ cmd: "setMute", ch: this.ch, on });
    });
    this.localBtn.addEventListener("click", () => {
      const on = !this.localBtn.classList.contains("on-local");
      post({ cmd: "setLocal", ch: this.ch, on });
    });

    this.rack = new InsertRack({
      onSelect: (idx) => post({ cmd: "setSel", ch: this.ch, idx }),
      onEdit: (idx) => post({ cmd: "openEditor", ch: this.ch, idx }),
      onBypass: (idx) => post({ cmd: "bypassSel", ch: this.ch, idx }),
      onRemove: (idx) => post({ cmd: "remove", ch: this.ch, idx }),
      onLoad: () => post({ cmd: "load", ch: this.ch }),
      onWholeBypass: () => post({ cmd: "bypassWhole", ch: this.ch }),
    });
    body.appendChild(this.rack.el);

    this.el = panel;
  }

  setVisibleRows(rows: number): void {
    this.rack.setVisibleRows(rows);
  }

  update(state: ChannelState): void {
    this.rawMeter.setPeak(state.raw);
    this.postMeter.setPeak(state.post);
    if (
      !this.gain.isDragging &&
      (!Number.isFinite(this.lastGain) || Math.abs(state.gainDb - this.lastGain) > 0.001)
    ) {
      this.gain.setValue(state.gainDb, false);
      this.lastGain = state.gainDb;
    }
    this.muteBtn.classList.toggle("on-mute", state.mute);
    this.muteBtn.setAttribute("aria-pressed", String(state.mute));
    this.localBtn.classList.toggle("on-local", state.local);
    this.localBtn.setAttribute("aria-pressed", String(state.local));
    this.rack.update(state);
  }
}
