import { Knob } from "./knob";
import { StereoMeter } from "./stereoMeter";
import { post } from "../bridge";
import type { Snapshot } from "../types";

// Muted-speaker glyph mirrors the signal-X form; the headphone glyph is plain,
// matching the iD.exe reference pair (mute-speaker + headphone).
const SPEAKER_MUTE_ICON = `<svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 5 6 9H2v6h4l5 4z"/><line x1="16" y1="9" x2="22" y2="15"/><line x1="22" y1="9" x2="16" y2="15"/></svg>`;
const HEADPHONE_ICON = `<svg viewBox="0 0 24 24" width="17" height="17" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 18v-6a9 9 0 0 1 18 0v6"/><path d="M21 19a2 2 0 0 1-2 2h-1a2 2 0 0 1-2-2v-3a2 2 0 0 1 2-2h3zM3 19a2 2 0 0 0 2 2h1a2 2 0 0 0 2-2v-3a2 2 0 0 0-2-2H3z"/></svg>`;
const MONO_ICON = `<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="9" cy="12" r="6"/><circle cx="15" cy="12" r="6"/></svg>`;
const DIM_ICON = `<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="4"/><line x1="12" y1="2" x2="12" y2="4"/><line x1="12" y1="20" x2="12" y2="22"/><line x1="2" y1="12" x2="4" y2="12"/><line x1="20" y1="12" x2="22" y2="12"/><line x1="4.9" y1="4.9" x2="6.3" y2="6.3"/><line x1="17.7" y1="17.7" x2="19.1" y2="19.1"/><line x1="4.9" y1="19.1" x2="6.3" y2="17.7"/><line x1="17.7" y1="6.3" x2="19.1" y2="4.9"/></svg>`;
const TB_ICON = `<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="2" width="6" height="12" rx="3"/><path d="M5 10v1a7 7 0 0 0 14 0v-1"/><line x1="12" y1="18" x2="12" y2="22"/></svg>`;

// Narrow iD.exe-style monitor utility. Directly-wired controls only: the two
// hardware volume knobs, the software Mono control, the real stereo SYSTEM
// meter, and ONE functional SYSTEM endpoint-mute control on that meter. The mute
// controls the Windows iD14 render endpoint via IAudioEndpointVolume (endpoint-
// wide) - it is NOT independent Speaker/Headphone mute. Speaker/Headphone Mute,
// Dim and Talkback have no approved iD14 MK1 semantics yet, so those icon slots
// stay disabled and are never wired to guessed vendor commands.
export class MiniMonitor {
  readonly el: HTMLElement;
  readonly sysMeter: StereoMeter;
  private readonly monitorHw: Knob;
  private readonly headphoneHw: Knob;
  private readonly monoBtn: HTMLButtonElement;
  private readonly sysMuteBtn: HTMLButtonElement;
  private lastMon = NaN;
  private lastHp = NaN;

  constructor(root: HTMLElement) {
    root.innerHTML = `
      <div class="mini-shell">
        <div class="mini-top">
          <span class="mini-title">Audient Console</span>
        </div>
        <div class="view-seg" role="radiogroup" aria-label="View mode">
          <button type="button" class="view-btn" data-view="main" role="radio" aria-checked="false">MAIN</button>
          <button type="button" class="view-btn active" data-view="mini" role="radio" aria-checked="true">MINI</button>
        </div>
        <div class="mini-sep"></div>
        <div class="mini-meter-wrap"></div>
        <div class="mini-sep"></div>
        <div class="mini-tools-row">
          <div class="mini-tool-wrap">
            <button type="button" class="mini-tool" id="miniMono" title="Software output mono (0.5 x (L+R))">${MONO_ICON}</button>
            <span class="mini-tool-label">MONO</span>
          </div>
          <div class="mini-tool-wrap">
            <button type="button" class="mini-tool" disabled title="Not available: iD14 dim is not proven yet">${DIM_ICON}</button>
            <span class="mini-tool-label">DIM</span>
          </div>
          <div class="mini-tool-wrap">
            <button type="button" class="mini-tool" disabled title="Not available: talkback is not proven yet">${TB_ICON}</button>
            <span class="mini-tool-label">TB</span>
          </div>
        </div>
        <div class="mini-knobs"></div>
        <div class="mini-footer">AUDIENT</div>
      </div>`;
    this.el = root;

    this.sysMeter = new StereoMeter("SYSTEM");
    (root.querySelector(".mini-meter-wrap") as HTMLElement).appendChild(this.sysMeter.el);

    // The ONE functional mute, associated with the SYSTEM meter. State is always
    // taken from the published Windows endpoint state (snapshot); the click only
    // requests a change.
    this.sysMuteBtn = document.createElement("button");
    this.sysMuteBtn.type = "button";
    this.sysMuteBtn.className = "sm-mute-btn";
    this.sysMuteBtn.textContent = "MUTE";
    this.sysMuteBtn.title = "Mute System Audio";
    this.sysMuteBtn.setAttribute("aria-label", "Mute System Audio");
    this.sysMuteBtn.addEventListener("click", () => {
      post({ cmd: "setSystemMute", on: !this.sysMuteBtn.classList.contains("on") });
    });
    (this.sysMeter.el.querySelector(".sm-label") as HTMLElement).appendChild(this.sysMuteBtn);

    this.monoBtn = root.querySelector("#miniMono") as HTMLButtonElement;
    this.monoBtn.addEventListener("click", () => {
      const on = !this.monoBtn.classList.contains("on");
      post({ cmd: "toggleMono", on });
    });

    this.monitorHw = new Knob({
      min: -127,
      max: -6,
      value: -18,
      label: "",
      kind: "hw",
      format: (v) => `${v.toFixed(1)} dB`,
      onChange: (v) => post({ cmd: "setHwDb", hp: false, db: Number(v.toFixed(2)) }),
    });
    this.headphoneHw = new Knob({
      min: -127,
      max: -6,
      value: -18,
      label: "",
      kind: "hw",
      format: (v) => `${v.toFixed(1)} dB`,
      onChange: (v) => post({ cmd: "setHwDb", hp: true, db: Number(v.toFixed(2)) }),
    });
    const knobs = root.querySelector(".mini-knobs") as HTMLElement;
    knobs.appendChild(this.knobBlock("MONITOR", this.monitorHw, SPEAKER_MUTE_ICON, "Mute Speakers"));
    knobs.appendChild(this.knobBlock("HEADPHONES", this.headphoneHw, HEADPHONE_ICON, "Mute Headphones"));

    root.querySelectorAll<HTMLButtonElement>(".view-btn").forEach((btn) => {
      btn.addEventListener("click", () => post({ cmd: "setUiMode", mode: btn.dataset.view === "mini" ? 1 : 0 }));
    });
  }

  private knobBlock(title: string, knob: Knob, muteIcon: string, muteTitle: string): HTMLElement {
    const block = document.createElement("div");
    block.className = "mini-knob-block";
    // iD.exe-like vertical stack: mute icon, then label, then the knob.
    const mute = document.createElement("button");
    mute.type = "button";
    mute.className = "mini-mute-btn";
    mute.disabled = true;
    mute.title = muteTitle;
    mute.setAttribute("aria-label", muteTitle);
    mute.innerHTML = muteIcon;
    block.appendChild(mute);
    const titleEl = document.createElement("div");
    titleEl.className = "mini-knob-title";
    titleEl.textContent = title;
    block.appendChild(titleEl);
    block.appendChild(knob.el);
    return block;
  }

  update(snap: Snapshot): void {
    this.sysMeter.setPeaks(snap.systemPeakL, snap.systemPeakR);

    // SYSTEM endpoint mute: reflect Windows state only (never the click). The
    // meter stays live while muted (it is PRE-mute) - do not freeze it.
    const sm = snap.systemMuted;
    const sysMuteAvailable = sm === true || sm === false;
    this.sysMuteBtn.disabled = !sysMuteAvailable;
    this.sysMuteBtn.classList.toggle("on", sm === true);
    this.sysMuteBtn.setAttribute("aria-pressed", String(sm === true));
    this.sysMuteBtn.title = sysMuteAvailable ? "Mute System Audio" : "Mute System Audio (unavailable)";
    this.sysMuteBtn.setAttribute("aria-label", this.sysMuteBtn.title);

    this.monoBtn.classList.toggle("on", snap.mono);
    this.monoBtn.setAttribute("aria-pressed", String(snap.mono));

    if (snap.hwMonDb === null || snap.hwMonDb === undefined) {
      this.monitorHw.setEnabled(false);
      this.monitorHw.setCapsText("--");
      this.lastMon = NaN;
    } else {
      this.monitorHw.setEnabled(true);
      if (!this.monitorHw.isDragging && (!Number.isFinite(this.lastMon) || Math.abs(snap.hwMonDb - this.lastMon) > 0.001)) {
        this.monitorHw.setValue(snap.hwMonDb, false);
        this.lastMon = snap.hwMonDb;
      }
    }
    if (snap.hwHpDb === null || snap.hwHpDb === undefined) {
      this.headphoneHw.setEnabled(false);
      this.headphoneHw.setCapsText("--");
      this.lastHp = NaN;
    } else {
      this.headphoneHw.setEnabled(true);
      if (!this.headphoneHw.isDragging && (!Number.isFinite(this.lastHp) || Math.abs(snap.hwHpDb - this.lastHp) > 0.001)) {
        this.headphoneHw.setValue(snap.hwHpDb, false);
        this.lastHp = snap.hwHpDb;
      }
    }
  }
}
