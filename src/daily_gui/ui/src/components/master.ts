import { Knob } from "./knob";
import { post } from "../bridge";
import { parseAsio } from "../parse";
import type { Snapshot } from "../types";

const MONO_ICON = `<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="9" cy="12" r="6"/><circle cx="15" cy="12" r="6"/></svg>`;
const DIM_ICON = `<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="4"/><line x1="12" y1="2" x2="12" y2="4"/><line x1="12" y1="20" x2="12" y2="22"/><line x1="2" y1="12" x2="4" y2="12"/><line x1="20" y1="12" x2="22" y2="12"/><line x1="4.9" y1="4.9" x2="6.3" y2="6.3"/><line x1="17.7" y1="17.7" x2="19.1" y2="19.1"/><line x1="4.9" y1="19.1" x2="6.3" y2="17.7"/><line x1="17.7" y1="6.3" x2="19.1" y2="4.9"/></svg>`;

export class Master {
  readonly el: HTMLElement;
  private readonly mmGlobal: HTMLButtonElement;
  private readonly monoBtn: HTMLButtonElement;
  private readonly monitorHw: Knob;
  private readonly headphoneHw: Knob;
  private readonly sysRate: HTMLElement;
  private readonly sysBuf: HTMLElement;
  private readonly sysAsio: HTMLElement;
  private readonly sysDevice: HTMLElement;
  private readonly sysHw: HTMLElement;
  private readonly sysDot: HTMLElement;
  private readonly sysConnected: HTMLElement;
  private lastMon = NaN;
  private lastHp = NaN;

  constructor(panel: HTMLElement) {
    panel.innerHTML = `
      <div class="panel-head"><span class="ch-accent"></span><span class="ch-title">MASTER</span></div>
      <div class="panel-body"></div>`;
    const body = panel.querySelector(".panel-body") as HTMLElement;

    const select = document.createElement("div");
    select.className = "monitor-select";
    this.mmGlobal = document.createElement("button");
    this.mmGlobal.type = "button";
    this.mmGlobal.className = "mm-btn";
    this.mmGlobal.textContent = "GLOBAL LOCAL MONITOR";
    const mmDaw = document.createElement("button");
    mmDaw.type = "button";
    mmDaw.className = "mm-btn";
    mmDaw.textContent = "DAW";
    mmDaw.disabled = true;
    mmDaw.title = "Not available (DAW monitor source is not proven)";
    select.appendChild(this.mmGlobal);
    select.appendChild(mmDaw);
    body.appendChild(select);

    const toggles = document.createElement("div");
    toggles.className = "master-toggles";
    this.monoBtn = document.createElement("button");
    this.monoBtn.type = "button";
    this.monoBtn.className = "mt-btn";
    this.monoBtn.title = "Software output mono (0.5 x (L+R))";
    this.monoBtn.innerHTML = `${MONO_ICON}<span>MONO</span>`;
    const dimBtn = document.createElement("button");
    dimBtn.type = "button";
    dimBtn.className = "mt-btn";
    dimBtn.disabled = true;
    dimBtn.title = "Not available (iD14 dim is not proven)";
    dimBtn.innerHTML = `${DIM_ICON}<span>DIM</span>`;
    toggles.appendChild(this.monoBtn);
    toggles.appendChild(dimBtn);
    body.appendChild(toggles);

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
    const hwGrid = document.createElement("div");
    hwGrid.className = "hw-grid";
    hwGrid.appendChild(this.hwBlock("MONITOR (HW)", this.monitorHw));
    hwGrid.appendChild(this.hwBlock("HEADPHONES (HW)", this.headphoneHw));
    body.appendChild(hwGrid);

    const sys = document.createElement("div");
    sys.className = "system-card";
    sys.innerHTML = `
      <div class="sys-title">SYSTEM</div>
      <div class="sys-row"><span class="sys-key">Sample Rate</span><span class="sys-val" id="sysRate">--</span></div>
      <div class="sys-row"><span class="sys-key">Buffer Size</span><span class="sys-val" id="sysBuf">--</span></div>
      <div class="sys-row"><span class="sys-key">ASIO Driver</span><span class="sys-val" id="sysAsio">--</span></div>
      <div class="sys-row"><span class="sys-key">Device</span><span class="sys-val" id="sysDevice">--</span></div>
      <div class="sys-row"><span class="sys-key">Hardware Control</span><span class="sys-val" id="sysHw">--</span></div>
      <div class="sys-connected"><span class="dot" id="sysDot"></span><span id="sysConnected">Device Disconnected</span></div>`;
    body.appendChild(sys);
    this.sysRate = sys.querySelector("#sysRate") as HTMLElement;
    this.sysBuf = sys.querySelector("#sysBuf") as HTMLElement;
    this.sysAsio = sys.querySelector("#sysAsio") as HTMLElement;
    this.sysDevice = sys.querySelector("#sysDevice") as HTMLElement;
    this.sysHw = sys.querySelector("#sysHw") as HTMLElement;
    this.sysDot = sys.querySelector("#sysDot") as HTMLElement;
    this.sysConnected = sys.querySelector("#sysConnected") as HTMLElement;

    this.mmGlobal.addEventListener("click", () => {
      const on = !this.mmGlobal.classList.contains("active");
      post({ cmd: "toggleGlobal", on });
    });
    this.monoBtn.addEventListener("click", () => {
      const on = !this.monoBtn.classList.contains("on");
      post({ cmd: "toggleMono", on });
    });

    this.el = panel;
  }

  private hwBlock(title: string, knob: Knob): HTMLElement {
    const block = document.createElement("div");
    block.className = "hw-block";
    block.innerHTML = `<div class="hw-title">${title}</div>`;
    const line = document.createElement("div");
    line.className = "hw-knob-line";
    const lo = document.createElement("span");
    lo.className = "hw-extreme";
    lo.textContent = "-∞";
    const hi = document.createElement("span");
    hi.className = "hw-extreme";
    hi.textContent = "0 dB";
    line.appendChild(lo);
    line.appendChild(knob.el);
    line.appendChild(hi);
    block.appendChild(line);
    return block;
  }

  update(snap: Snapshot): void {
    this.mmGlobal.classList.toggle("active", snap.globalMonitor);
    this.mmGlobal.setAttribute("aria-pressed", String(snap.globalMonitor));
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

    const asio = parseAsio(snap.asioText);
    const a = snap.audio;
    const rate = a && a.actualRate > 0 ? a.actualRate : asio.rateK * 1000;
    this.sysRate.textContent = rate > 0 ? `${Math.round(rate / 1000)} kHz` : "--";
    this.sysBuf.textContent = a && a.currentBuffer ? `${a.currentBuffer} samples` : "--";
    this.sysAsio.textContent = asio.connected ? asio.state || "Streaming" : asio.state || "--";
    const dev = (snap.audioDevice || "").split(/\u2014|--/)[0].trim();
    this.sysDevice.textContent = dev || "iD14 MK1";
    this.sysHw.textContent = snap.hardwareControl || "--";
    this.sysDot.classList.toggle("ok", asio.connected);
    this.sysConnected.textContent = asio.connected ? "Device Connected" : "Device Disconnected";
  }
}
