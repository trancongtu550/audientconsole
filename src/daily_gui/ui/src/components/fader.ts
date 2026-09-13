// Compact vertical fader for the per-channel software Monitor Gain (MON column).
// Audio semantics unchanged (still setGain); presentation only. Pointer handling
// is defensive: dragging is set BEFORE setPointerCapture, capture failures are
// ignored, and the drag continues even if capture is unavailable.
let uid = 0;

export interface FaderOptions {
  min: number;
  max: number;
  value: number;
  label?: string;
  format: (v: number) => string;
  onChange: (v: number) => void;
}

export class Fader {
  readonly el: HTMLDivElement;
  private readonly min: number;
  private readonly max: number;
  private readonly format: (v: number) => string;
  private readonly onChange: (v: number) => void;
  private readonly track: HTMLDivElement;
  private readonly fill: HTMLDivElement;
  private readonly cap: HTMLDivElement;
  private readonly caps: HTMLDivElement;
  private value: number;
  private dragging = false;
  private startY = 0;
  private startValue = 0;

  constructor(opts: FaderOptions) {
    this.min = opts.min;
    this.max = opts.max;
    this.value = opts.value;
    this.format = opts.format;
    this.onChange = opts.onChange;
    void uid;

    const col = document.createElement("div");
    col.className = "meter-col fader-col";
    col.innerHTML = `
      <div class="meter-top-label">${opts.label ?? "MON"}</div>
      <div class="fader-body">
        <div class="fader" tabindex="0" role="slider" aria-label="${opts.label ?? "Monitor Gain"}" aria-valuemin="${opts.min}" aria-valuemax="${opts.max}">
          <div class="fader-rail"></div>
          <div class="fader-fill"></div>
          <div class="fader-cap"></div>
        </div>
      </div>
      <div class="value-capsule">${this.format(this.value)}</div>`;
    this.el = col;
    this.track = col.querySelector(".fader") as HTMLDivElement;
    this.fill = col.querySelector(".fader-fill") as HTMLDivElement;
    this.cap = col.querySelector(".fader-cap") as HTMLDivElement;
    this.caps = col.querySelector(".value-capsule") as HTMLDivElement;

    this.track.addEventListener("pointerdown", this.onDown);
    this.track.addEventListener("pointermove", this.onMove);
    this.track.addEventListener("pointerup", this.onUp);
    this.track.addEventListener("pointercancel", this.onUp);
    this.track.addEventListener("wheel", this.onWheel, { passive: false });
    this.track.addEventListener("keydown", this.onKey);
    this.render();
  }

  get isDragging(): boolean {
    return this.dragging;
  }

  setValue(v: number, emit: boolean): void {
    const next = this.clamp(v);
    const changed = next !== this.value;
    this.value = next;
    this.render();
    if (emit && changed) this.onChange(this.value);
  }

  setEnabled(enabled: boolean): void {
    this.track.classList.toggle("disabled", !enabled);
    this.track.style.pointerEvents = enabled ? "auto" : "none";
    this.el.style.opacity = enabled ? "1" : "0.45";
  }

  setCapsText(text: string): void {
    this.caps.textContent = text;
  }

  private clamp(v: number): number {
    return v < this.min ? this.min : v > this.max ? this.max : v;
  }

  private render(): void {
    const norm = (this.value - this.min) / (this.max - this.min);
    const pct = (norm * 100).toFixed(1);
    this.fill.style.height = `${pct}%`;
    this.cap.style.bottom = `calc(${pct}% - 0.42rem)`;
    this.caps.textContent = this.format(this.value);
    this.track.setAttribute("aria-valuenow", String(this.value));
  }

  private onDown = (e: PointerEvent): void => {
    e.preventDefault();
    this.dragging = true;
    this.startY = e.clientY;
    this.startValue = this.value;
    try {
      this.track.setPointerCapture(e.pointerId);
    } catch {
      /* capture unavailable: drag still works while the pointer is over the track */
    }
    this.track.focus();
  };

  private onMove = (e: PointerEvent): void => {
    if (!this.dragging) return;
    const h = this.track.clientHeight || 1;
    const dy = this.startY - e.clientY;
    const perPx = (this.max - this.min) / h;
    this.setValue(this.startValue + dy * perPx, true);
  };

  private onUp = (e: PointerEvent): void => {
    if (!this.dragging) return;
    this.dragging = false;
    try {
      this.track.releasePointerCapture(e.pointerId);
    } catch {
      /* ignore */
    }
  };

  private onWheel = (e: WheelEvent): void => {
    e.preventDefault();
    const range = this.max - this.min;
    const step = (range / 40) * (e.shiftKey ? 0.25 : 1);
    this.setValue(this.value + (e.deltaY < 0 ? step : -step), true);
  };

  private onKey = (e: KeyboardEvent): void => {
    const range = this.max - this.min;
    const step = ((e.shiftKey ? 0.25 : 1) * range) / 40;
    if (e.key === "ArrowUp" || e.key === "ArrowRight") {
      e.preventDefault();
      this.setValue(this.value + step, true);
    } else if (e.key === "ArrowDown" || e.key === "ArrowLeft") {
      e.preventDefault();
      this.setValue(this.value - step, true);
    }
  };
}
