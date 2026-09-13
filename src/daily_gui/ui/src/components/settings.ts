import { post } from "../bridge";
import { parseAsio, parseXruns } from "../parse";
import type { Snapshot } from "../types";

type Tab = "audio" | "behavior" | "about";

const TABS: { id: Tab; label: string }[] = [
  { id: "audio", label: "Audio" },
  { id: "behavior", label: "Behavior" },
  { id: "about", label: "About" },
];

// Settings modal. Purely presentation: every control posts the SAME validated
// commands the rest of the console uses (e.g. setBufferSize) and reads its state
// only from the cached 30 Hz snapshot. Reuses the proven popover discipline:
// stable delegated listeners, signature-guarded rendering, and a pointer-down
// gate so a render can never destroy a control between mousedown and mouseup.
export class SettingsPanel {
  private readonly overlay: HTMLDivElement;
  private readonly dialog: HTMLDivElement;
  private readonly button: HTMLButtonElement;
  private open = false;
  private tab: Tab = "audio";
  private snap: Snapshot | null = null;
  private signature = "";
  private pointerDown = false;
  private lastFocused: HTMLElement | null = null;

  constructor(button: HTMLButtonElement) {
    this.button = button;
    this.overlay = document.createElement("div");
    this.overlay.className = "settings-overlay";
    this.overlay.setAttribute("role", "presentation");
    this.dialog = document.createElement("div");
    this.dialog.className = "settings-dialog";
    this.dialog.setAttribute("role", "dialog");
    this.dialog.setAttribute("aria-modal", "true");
    this.dialog.setAttribute("aria-label", "Settings");
    this.overlay.appendChild(this.dialog);
    document.body.appendChild(this.overlay);

    button.addEventListener("click", () => this.toggle());
    this.overlay.addEventListener("pointerdown", () => {
      this.pointerDown = true;
    });
    window.addEventListener("pointerup", () => {
      this.pointerDown = false;
    });
    window.addEventListener("pointercancel", () => {
      this.pointerDown = false;
    });

    // Delegated click: tabs, close, backdrop, source/theme/behavior, buffer.
    this.overlay.addEventListener("click", (e) => this.onClick(e));
    // Delegated change: checkboxes.
    this.overlay.addEventListener("change", (e) => this.onChange(e));
    document.addEventListener("keydown", (e) => {
      if (this.open && e.key === "Escape") {
        e.preventDefault();
        this.close();
      }
    });
  }

  update(snap: Snapshot): void {
    this.snap = snap;
    if (this.open && !this.pointerDown) this.render();
  }

  toggle(): void {
    if (this.open) this.close();
    else this.show();
  }

  private show(): void {
    this.open = true;
    this.lastFocused = document.activeElement as HTMLElement | null;
    this.signature = "";
    this.overlay.classList.add("open");
    this.render();
    const first = this.dialog.querySelector<HTMLElement>("[data-tab].active");
    (first ?? this.dialog).focus();
  }

  private close(): void {
    this.open = false;
    this.overlay.classList.remove("open");
    if (this.lastFocused && this.lastFocused.focus) this.lastFocused.focus();
    this.button.focus();
  }

  private selectTab(tab: Tab): void {
    this.tab = tab;
    this.signature = "";
    this.render();
  }

  private onClick(e: MouseEvent): void {
    const target = e.target as HTMLElement;
    if (target === this.overlay) {
      this.close();
      return;
    }
    const closeBtn = target.closest<HTMLElement>("[data-close]");
    if (closeBtn) {
      this.close();
      return;
    }
    const tabBtn = target.closest<HTMLElement>("[data-tab]");
    if (tabBtn && tabBtn.dataset.tab) {
      this.selectTab(tabBtn.dataset.tab as Tab);
      return;
    }
    const srcBtn = target.closest<HTMLButtonElement>("[data-vmic]");
    if (srcBtn && !srcBtn.disabled && srcBtn.dataset.vmic !== undefined) {
      const ch = Number(srcBtn.dataset.vmic);
      if (this.snap && ch !== this.snap.virtualMicSource) {
        post({ cmd: "setVirtualMicSource", ch });
      }
      return;
    }
    const bufBtn = target.closest<HTMLButtonElement>("[data-buffer]");
    if (bufBtn && !bufBtn.disabled && bufBtn.dataset.buffer !== undefined) {
      const value = Number(bufBtn.dataset.buffer);
      if (this.snap && value !== this.snap.audio.currentBuffer) {
        post({ cmd: "setBufferSize", value });
      }
      return;
    }
    const viewBtn = target.closest<HTMLButtonElement>("[data-uimode]");
    if (viewBtn && !viewBtn.disabled && viewBtn.dataset.uimode !== undefined) {
      post({ cmd: "setUiMode", mode: Number(viewBtn.dataset.uimode) });
      return;
    }
    const copyBtn = target.closest<HTMLElement>("[data-copy]");
    if (copyBtn) {
      void this.copySystemInfo();
    }
  }

  private onChange(e: Event): void {
    const input = e.target as HTMLInputElement;
    if (!input || input.type !== "checkbox") return;
    const key = input.dataset.pref;
    if (key === "closeToTray") post({ cmd: "setCloseToTray", on: input.checked });
    else if (key === "startMinimized") post({ cmd: "setStartMinimized", on: input.checked });
  }

  private render(): void {
    const s = this.snap;
    if (!s) return;
    const signature = [
      this.tab,
      s.virtualMicSource,
      s.dual,
      s.audio.currentBuffer,
      s.audio.requestedBuffer,
      s.audio.rebuilding,
      s.audio.supportedBuffers.join(","),
      s.closeToTray,
      s.startMinimized,
      s.picoConnected,
      s.picoText,
      s.asioText,
      s.xrunsText,
      s.appVersion,
      s.audioDevice,
      s.hardwareControl,
      s.webview2Version,
      s.uiMode,
    ].join("|");
    if (signature === this.signature) return;
    this.signature = signature;

    const tabs = TABS.map(
      (t) =>
        `<button type="button" class="settings-tab${t.id === this.tab ? " active" : ""}" data-tab="${t.id}" role="tab" aria-selected="${t.id === this.tab}">${t.label}</button>`,
    ).join("");

    let body = "";
    if (this.tab === "audio") body = this.renderAudio(s);
    else if (this.tab === "behavior") body = this.renderBehavior(s);
    else body = this.renderAbout(s);

    const footer =
      this.tab === "about"
        ? `<div class="settings-actions"><button type="button" class="btn" data-copy>Copy System Info</button></div>`
        : "";

    this.dialog.innerHTML = `
      <div class="settings-head">
        <span class="settings-title">Settings</span>
        <button type="button" class="settings-close" data-close aria-label="Close settings" title="Close">&#10005;</button>
      </div>
      <div class="settings-tabs" role="tablist">${tabs}</div>
      <div class="settings-body">${body}</div>
      ${footer}`;
  }

  private renderAudio(s: Snapshot): string {
    const input2Disabled = !s.dual;
    const sources = `
      <div class="settings-seg" role="radiogroup" aria-label="Virtual Mic Source">
        <button type="button" class="seg-btn${s.virtualMicSource === 0 ? " active" : ""}" data-vmic="0" role="radio" aria-checked="${s.virtualMicSource === 0}">Input 1</button>
        <button type="button" class="seg-btn${s.virtualMicSource === 1 ? " active" : ""}" data-vmic="1" role="radio" aria-checked="${s.virtualMicSource === 1}"${input2Disabled ? " disabled" : ""}>Input 2</button>
      </div>
      <div class="settings-note">Exactly one input feeds the virtual mic. The other input is excluded (no mix).${
        input2Disabled ? " Input 2 is unavailable (single-input session)." : ""
      }</div>`;

    const buffers = (s.audio.supportedBuffers.length ? s.audio.supportedBuffers : [s.audio.currentBuffer])
      .map((n) => {
        const active = n === s.audio.currentBuffer;
        const disabled = s.audio.rebuilding || active;
        return `<button type="button" class="seg-btn${active ? " active" : ""}" data-buffer="${n}"${
          disabled ? " disabled" : ""
        }>${n}</button>`;
      })
      .join("");

    const reconfig = s.audio.rebuilding
      ? `<span class="settings-note warn">Reconfiguring${
          s.audio.requestedBuffer ? ` to ${s.audio.requestedBuffer}` : ""
        }\u2026</span>`
      : "";

    return `
      <div class="settings-section">
        <div class="settings-label">Virtual Mic Source</div>
        ${sources}
      </div>
      <div class="settings-section">
        <div class="settings-row">
          <span class="settings-label">Sample Rate</span>
          <span class="settings-value"><span class="mono">48 kHz</span> <span class="lock-badge">LOCKED</span></span>
        </div>
      </div>
      <div class="settings-section">
        <div class="settings-row">
          <span class="settings-label">Buffer Size</span>
          <span class="settings-value">${reconfig ? reconfig : `<span class="settings-seg">${buffers}</span>`}</span>
        </div>
      </div>
      <div class="settings-section">
        <div class="settings-row"><span class="settings-label">Pico</span><span class="settings-value ${
          s.picoConnected ? "ok" : "off"
        }">${escapeHtml(s.picoText)}</span></div>
        <div class="settings-row"><span class="settings-label">ASIO</span><span class="settings-value mono">${escapeHtml(
          s.asioText,
        )}</span></div>
      </div>`;
  }

  private renderBehavior(s: Snapshot): string {
    return `
      <div class="settings-section">
        <div class="settings-row">
          <span class="settings-label">View mode</span>
          <span class="settings-value"><span class="settings-seg">
            <button type="button" class="seg-btn${s.uiMode !== 1 ? " active" : ""}" data-uimode="0">Main Mixer</button>
            <button type="button" class="seg-btn${s.uiMode === 1 ? " active" : ""}" data-uimode="1">Mini Monitor</button>
          </span></span>
        </div>
        <div class="settings-note">Presentation only; switching views never restarts audio.</div>
      </div>
      <div class="settings-section">
        <div class="settings-row">
          <span class="settings-label">Close to tray</span>
          <label class="settings-toggle"><input type="checkbox" class="switch" data-pref="closeToTray"${s.closeToTray ? " checked" : ""} /><span class="settings-value">${s.closeToTray ? "On" : "Off"}</span></label>
        </div>
        <div class="settings-note">When on, closing the window keeps the engine running in the tray.</div>
      </div>
      <div class="settings-section">
        <div class="settings-row">
          <span class="settings-label">Start minimized to tray</span>
          <label class="settings-toggle"><input type="checkbox" class="switch" data-pref="startMinimized"${s.startMinimized ? " checked" : ""} /><span class="settings-value">${s.startMinimized ? "On" : "Off"}</span></label>
        </div>
        <div class="settings-note">Next launch: engine and tray start, main window stays hidden. Applies to a manual launch; no Windows autostart.</div>
      </div>`;
  }

  private renderAbout(s: Snapshot): string {
    const asio = parseAsio(s.asioText);
    const xr = parseXruns(s.xrunsText);
    const rate = s.audio.actualRate > 0 ? s.audio.actualRate : asio.rateK * 1000;
    const buffer = s.audio.currentBuffer || asio.buffer;
    const asioState = asio.connected ? asio.state || "Streaming" : asio.state || "--";
    const asioLine = `${asioState} ${rate ? rate / 1000 : "--"} kHz / ${buffer || "--"} samples`;
    return `
      <div class="about-grid">
        <span class="about-key">App version</span><span class="about-val">${escapeHtml(s.appVersion || "--")}</span>
        <span class="about-key">Audio Device</span><span class="about-val">${escapeHtml(s.audioDevice || "--")}</span>
        <span class="about-key">ASIO</span><span class="about-val">${escapeHtml(asioLine)}</span>
        <span class="about-key">Hardware Control</span><span class="about-val ${
          s.hardwareControl === "Connected" ? "ok" : ""
        }">${escapeHtml(s.hardwareControl || "--")}</span>
        <span class="about-key">Pico</span><span class="about-val ${
          s.picoConnected ? "ok" : ""
        }">${escapeHtml(s.picoText || "--")}</span>
        <span class="about-key">Sample rate</span><span class="about-val">${rate ? `${rate / 1000} kHz` : "--"}</span>
        <span class="about-key">Buffer size</span><span class="about-val">${buffer || "--"} samples</span>
        <span class="about-key">Xruns</span><span class="about-val">${xr.xruns}</span>
        <span class="about-key">Overloads</span><span class="about-val">${xr.overloads}</span>
        <span class="about-key">WebView2</span><span class="about-val">${escapeHtml(s.webview2Version || "--")}</span>
      </div>`;
  }

  private systemInfoText(): string {
    const s = this.snap;
    if (!s) return "";
    const asio = parseAsio(s.asioText);
    const xr = parseXruns(s.xrunsText);
    const rate = s.audio.actualRate > 0 ? s.audio.actualRate : asio.rateK * 1000;
    return [
      `Audient Console ${s.appVersion || "?"}`,
      `Audio Device: ${s.audioDevice || "?"}`,
      `Hardware Control: ${s.hardwareControl || "?"}`,
      `Pico: ${s.picoText || "?"}`,
      `Virtual Mic Source: Input ${s.virtualMicSource + 1}`,
      `ASIO: ${s.asioText || "?"}`,
      `Sample rate: ${rate ? `${rate / 1000} kHz` : "?"}`,
      `Buffer: ${s.audio.currentBuffer || "?"} samples`,
      `Xruns: ${xr.xruns}  Overloads: ${xr.overloads}`,
      `WebView2: ${s.webview2Version || "?"}`,
    ].join("\n");
  }

  private async copySystemInfo(): Promise<void> {
    const text = this.systemInfoText();
    try {
      if (navigator.clipboard && navigator.clipboard.writeText) {
        await navigator.clipboard.writeText(text);
        return;
      }
    } catch {
      // fall through to the legacy path
    }
    try {
      const area = document.createElement("textarea");
      area.value = text;
      area.style.position = "fixed";
      area.style.opacity = "0";
      document.body.appendChild(area);
      area.select();
      document.execCommand("copy");
      document.body.removeChild(area);
    } catch {
      // clipboard unavailable; silently ignore
    }
  }
}

function escapeHtml(value: string): string {
  return value.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c] ?? c);
}
