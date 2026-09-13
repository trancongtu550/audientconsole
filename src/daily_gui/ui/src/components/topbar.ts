import { post } from "../bridge";
import { parseAsio, parseXruns } from "../parse";
import { AudioSettingsPopover, fmtRate } from "./audioSettings";
import type { Snapshot } from "../types";

export class TopBar {
  private readonly led: HTMLElement;
  private readonly capAsio: HTMLElement;
  private readonly capRate: HTMLButtonElement;
  private readonly capXruns: HTMLElement;
  private readonly capOvl: HTMLElement;
  private readonly deviceName: HTMLButtonElement;
  private readonly popover: AudioSettingsPopover;
  private readonly viewBtns: HTMLButtonElement[];

  constructor(root: HTMLElement) {
    this.led = root.querySelector("#asioLed") as HTMLElement;
    this.capAsio = root.querySelector("#capAsio") as HTMLElement;
    this.capRate = root.querySelector("#capRate") as HTMLButtonElement;
    this.capXruns = root.querySelector("#capXruns") as HTMLElement;
    this.capOvl = root.querySelector("#capOvl") as HTMLElement;
    this.deviceName = root.querySelector("#deviceName") as HTMLButtonElement;
    this.popover = new AudioSettingsPopover(this.capRate);
    this.capRate.addEventListener("click", () => this.popover.toggle());
    this.viewBtns = Array.from(root.querySelectorAll<HTMLButtonElement>(".view-btn"));
    this.viewBtns.forEach((btn) => {
      btn.addEventListener("click", () => post({ cmd: "setUiMode", mode: btn.dataset.view === "mini" ? 1 : 0 }));
    });
  }

  update(snap: Snapshot): void {
    const asio = parseAsio(snap.asioText);
    const xr = parseXruns(snap.xrunsText);
    this.led.className = `led ${asio.connected ? "ok" : "warn"}`;
    this.capAsio.textContent = `ASIO ${capitalize(asio.state)}`;
    const a = snap.audio;
    const rate = a && a.actualRate > 0 ? a.actualRate : asio.rateK * 1000;
    const buf = a ? a.currentBuffer : asio.buffer;
    this.capRate.textContent = `${fmtRate(rate)} / ${buf || "--"} SAMPLES`;
    this.capRate.classList.toggle("rebuilding", !!(a && a.rebuilding));
    this.capXruns.textContent = `XRUNS ${xr.xruns}`;
    this.capOvl.textContent = `OVERLOADS ${xr.overloads}`;
    const dev = (snap.audioDevice || "").split(/\u2014|--/)[0].trim();
    this.deviceName.textContent = (dev || "iD14 MK1").toUpperCase();
    const isMini = snap.uiMode === 1;
    this.viewBtns.forEach((btn) => {
      const active = (btn.dataset.view === "mini") === isMini;
      btn.classList.toggle("active", active);
      btn.setAttribute("aria-checked", String(active));
    });
    if (a) this.popover.update(a);
  }
}

function capitalize(s: string): string {
  if (!s) return "";
  return s.charAt(0).toUpperCase() + s.slice(1).toLowerCase();
}
