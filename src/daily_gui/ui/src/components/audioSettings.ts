import { post } from "../bridge";
import type { AudioSettings } from "../types";

function fmtRate(hz: number): string {
  if (hz <= 0) return "--";
  const kHz = hz / 1000;
  return `${Number.isInteger(kHz) ? kHz : kHz.toFixed(1)} kHz`;
}

export class AudioSettingsPopover {
  private readonly el: HTMLDivElement;
  private open = false;
  private settings: AudioSettings = {
    currentRate: 48000,
    actualRate: 0,
    currentBuffer: 0,
    requestedBuffer: 0,
    rebuilding: false,
    supportedRates: [],
    supportedBuffers: [],
  };
  private readonly anchor: HTMLElement;
  private outsideHandler: ((e: MouseEvent) => void) | null = null;
  private keyHandler: ((e: KeyboardEvent) => void) | null = null;
  // Re-render only when the meaningful state changes. Re-rendering the DOM on
  // every 30 Hz snapshot destroys an option button between mousedown/mouseup,
  // so a real click would never fire.
  private signature = "";

  constructor(anchor: HTMLElement) {
    this.anchor = anchor;
    this.el = document.createElement("div");
    this.el.className = "popover";
    this.el.style.display = "none";
    // Event delegation: one stable listener survives any re-render.
    this.el.addEventListener("click", (e) => {
      const target = (e.target as HTMLElement).closest<HTMLElement>("[data-buffer]");
      if (!target || this.settings.rebuilding) return;
      const value = Number(target.dataset.buffer);
      if (!value || value === this.settings.currentBuffer) return;
      post({ cmd: "setBufferSize", value });
    });
    document.body.appendChild(this.el);
  }

  toggle(): void {
    if (this.open) {
      this.close();
    } else {
      this.show();
    }
  }

  private show(): void {
    this.open = true;
    this.signature = ""; // force a fresh render on open
    this.render();
    this.el.style.display = "block";
    const r = this.anchor.getBoundingClientRect();
    this.el.style.top = `${Math.round(r.bottom + 6)}px`;
    this.el.style.left = `${Math.round(Math.max(8, r.right - this.el.offsetWidth))}px`;
    this.outsideHandler = (e: MouseEvent) => {
      if (!this.el.contains(e.target as Node) && !this.anchor.contains(e.target as Node)) {
        this.close();
      }
    };
    this.keyHandler = (e: KeyboardEvent) => {
      if (e.key === "Escape") this.close();
    };
    setTimeout(() => {
      if (this.outsideHandler) document.addEventListener("mousedown", this.outsideHandler);
      if (this.keyHandler) document.addEventListener("keydown", this.keyHandler);
    }, 0);
  }

  close(): void {
    this.open = false;
    this.el.style.display = "none";
    if (this.outsideHandler) document.removeEventListener("mousedown", this.outsideHandler);
    if (this.keyHandler) document.removeEventListener("keydown", this.keyHandler);
    this.outsideHandler = null;
    this.keyHandler = null;
  }

  update(settings: AudioSettings): void {
    this.settings = settings;
    if (this.open) this.render();
  }

  private render(): void {
    const s = this.settings;
    const signature = `${s.actualRate}|${s.currentBuffer}|${s.requestedBuffer}|${s.rebuilding}|${s.supportedBuffers.join(",")}`;
    if (signature === this.signature) return;
    this.signature = signature;

    const rate = s.actualRate > 0 ? s.actualRate : s.currentRate;
    const rateLocked = rate === 48000;
    const rateHtml = `<li class="opt locked${rateLocked ? "" : " warn"}">
        <span class="opt-label">${fmtRate(rate)}</span>
        <span class="lock-badge">${rateLocked ? "LOCKED" : "UNSUPPORTED"}</span>
      </li>`;
    const buffers = s.supportedBuffers.length ? s.supportedBuffers : [s.currentBuffer];
    const buffersHtml = buffers
      .map((n) => {
        const active = n === s.currentBuffer;
        const selectable = !s.rebuilding && !active;
        const cls = `opt${active ? " active" : ""}${selectable ? " selectable" : ""}`;
        return `<li class="${cls}" data-buffer="${n}">
          <span class="radio">${active ? "&#9679;" : "&#9675;"}</span>
          <span class="opt-label">${n}</span>
        </li>`;
      })
      .join("");
    this.el.innerHTML = `
      <div class="pop-title">Audio Settings</div>
      <div class="pop-section">Sample Rate</div>
      <ul class="pop-list" id="popRates">${rateHtml}</ul>
      <div class="pop-section">Buffer Size ${
        s.rebuilding
          ? `<span class="pop-hint reconfig">Reconfiguring${s.requestedBuffer ? ` to ${s.requestedBuffer}` : ""}\u2026</span>`
          : ""
      }</div>
      <ul class="pop-list" id="popBuffers">${buffersHtml}</ul>
      <div class="pop-foot">48 kHz is required by the engine and transport.</div>`;
  }
}

export { fmtRate };
