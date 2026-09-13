let uid = 0;

const START = -135;
const SWEEP = 270;

function pt(cx: number, cy: number, r: number, degFromTop: number): [number, number] {
  const rad = ((degFromTop - 90) * Math.PI) / 180;
  return [cx + r * Math.cos(rad), cy + r * Math.sin(rad)];
}

function arcPath(cx: number, cy: number, r: number, a0: number, a1: number): string {
  const [x0, y0] = pt(cx, cy, r, a0);
  const [x1, y1] = pt(cx, cy, r, a1);
  const large = Math.abs(a1 - a0) > 180 ? 1 : 0;
  return `M ${x0.toFixed(2)} ${y0.toFixed(2)} A ${r} ${r} 0 ${large} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}

export interface KnobOptions {
  min: number;
  max: number;
  value: number;
  label: string;
  format: (v: number) => string;
  onChange: (v: number) => void;
  kind?: "gain" | "hw";
}

export class Knob {
  readonly el: HTMLDivElement;
  private readonly min: number;
  private readonly max: number;
  private readonly format: (v: number) => string;
  private readonly onChange: (v: number) => void;
  private value: number;
  private readonly arcEl: SVGPathElement;
  private readonly pointerLine: SVGLineElement;
  private readonly pointerDot: SVGCircleElement;
  private readonly caps: HTMLDivElement;
  private readonly dial: HTMLDivElement;
  private dragging = false;
  private startY = 0;
  private startValue = 0;

  constructor(opts: KnobOptions) {
    this.min = opts.min;
    this.max = opts.max;
    this.value = opts.value;
    this.format = opts.format;
    this.onChange = opts.onChange;

    const id = `knob${uid++}`;
    const wrap = document.createElement("div");
    wrap.className = "knob-wrap";
    wrap.innerHTML = `
      <div class="knob-label">${opts.label}</div>
      <div class="knob${opts.kind === "hw" ? " hw" : ""}" tabindex="0" role="slider" aria-label="${opts.label}">
        <svg class="dial" viewBox="0 0 120 120">
          <defs>
            <radialGradient id="${id}-bezel" cx="42%" cy="34%" r="72%">
              <stop offset="0%" stop-color="#3b414c"/>
              <stop offset="60%" stop-color="#23272e"/>
              <stop offset="100%" stop-color="#14171b"/>
            </radialGradient>
            <radialGradient id="${id}-face" cx="40%" cy="30%" r="80%">
              <stop offset="0%" stop-color="#33383f"/>
              <stop offset="55%" stop-color="#1e2228"/>
              <stop offset="100%" stop-color="#111316"/>
            </radialGradient>
          </defs>
          <circle cx="60" cy="60" r="57" fill="url(#${id}-bezel)" stroke="#0c0e11" stroke-width="1.5"/>
          <g class="ticks"></g>
          <circle cx="60" cy="60" r="45" fill="url(#${id}-face)" stroke="#3a414d" stroke-width="1"/>
          <path class="arc-bg" d="${arcPath(60, 60, 49, START, START + SWEEP)}" fill="none" stroke="#2b303a" stroke-width="4" stroke-linecap="round"/>
          <path class="arc-val" d="" fill="none" stroke="#e0a53a" stroke-width="4" stroke-linecap="round"/>
          <line class="pointer-line" x1="60" y1="60" x2="60" y2="60" stroke="#ffc766" stroke-width="3" stroke-linecap="round"/>
          <circle class="pointer" r="3.2" fill="#ffc766" stroke="#5d4213" stroke-width="0.8"/>
          <circle cx="60" cy="60" r="6" fill="#262b33" stroke="#454c58" stroke-width="1"/>
        </svg>
      </div>
      <div class="knob-caps">${this.format(this.value)}</div>`;
    this.el = wrap;
    this.arcEl = wrap.querySelector(".arc-val") as SVGPathElement;
    this.pointerLine = wrap.querySelector(".pointer-line") as SVGLineElement;
    this.pointerDot = wrap.querySelector(".pointer") as SVGCircleElement;
    this.caps = wrap.querySelector(".knob-caps") as HTMLDivElement;

    const ticks = wrap.querySelector(".ticks") as SVGGElement;
    for (let i = 0; i <= 10; i++) {
      const a = START + (i / 10) * SWEEP;
      const major = i % 5 === 0;
      const r0 = major ? 50 : 52;
      const [x0, y0] = pt(60, 60, r0, a);
      const [x1, y1] = pt(60, 60, 56, a);
      const line = document.createElementNS("http://www.w3.org/2000/svg", "line");
      line.setAttribute("x1", x0.toFixed(2));
      line.setAttribute("y1", y0.toFixed(2));
      line.setAttribute("x2", x1.toFixed(2));
      line.setAttribute("y2", y1.toFixed(2));
      line.setAttribute("stroke", major ? "#6a7280" : "#3b4250");
      line.setAttribute("stroke-width", major ? "2" : "1");
      ticks.appendChild(line);
    }

    this.dial = wrap.querySelector(".knob") as HTMLDivElement;
    this.dial.addEventListener("pointerdown", this.onDown);
    this.dial.addEventListener("pointermove", this.onMove);
    this.dial.addEventListener("pointerup", this.onUp);
    this.dial.addEventListener("pointercancel", this.onUp);
    this.dial.addEventListener("wheel", this.onWheel, { passive: false });
    this.dial.addEventListener("keydown", this.onKey);
    this.dial.addEventListener("dblclick", () => this.setValue((this.min + this.max) / 2, true));

    this.render();
  }

  get isDragging(): boolean {
    return this.dragging;
  }

  private clamp(v: number): number {
    return v < this.min ? this.min : v > this.max ? this.max : v;
  }

  setValue(v: number, emit: boolean): void {
    const next = this.clamp(v);
    const changed = next !== this.value;
    this.value = next;
    this.render();
    if (emit && changed) this.onChange(this.value);
  }

  setEnabled(enabled: boolean): void {
    this.dial.classList.toggle("disabled", !enabled);
    this.dial.style.pointerEvents = enabled ? "auto" : "none";
    this.el.style.opacity = enabled ? "1" : "0.45";
  }

  setCapsText(text: string): void {
    this.caps.textContent = text;
  }

  private render(): void {
    const norm = (this.value - this.min) / (this.max - this.min);
    const a = START + norm * SWEEP;
    this.arcEl.setAttribute("d", arcPath(60, 60, 49, START, a));
    const [px, py] = pt(60, 60, 38, a);
    this.pointerLine.setAttribute("x2", px.toFixed(2));
    this.pointerLine.setAttribute("y2", py.toFixed(2));
    this.pointerDot.setAttribute("cx", px.toFixed(2));
    this.pointerDot.setAttribute("cy", py.toFixed(2));
    this.caps.textContent = this.format(this.value);
  }

  private onDown = (e: PointerEvent): void => {
    e.preventDefault();
    this.dial.setPointerCapture(e.pointerId);
    this.dragging = true;
    this.startY = e.clientY;
    this.startValue = this.value;
    this.dial.focus();
  };

  private onMove = (e: PointerEvent): void => {
    if (!this.dragging) return;
    const dy = this.startY - e.clientY;
    const range = this.max - this.min;
    const perPx = range / (e.shiftKey ? 1500 : 300);
    this.setValue(this.startValue + dy * perPx, true);
  };

  private onUp = (e: PointerEvent): void => {
    if (!this.dragging) return;
    this.dragging = false;
    try {
      this.dial.releasePointerCapture(e.pointerId);
    } catch {
      /* noop */
    }
  };

  private onWheel = (e: WheelEvent): void => {
    e.preventDefault();
    const range = this.max - this.min;
    const step = (range / 100) * (e.shiftKey ? 0.5 : 2);
    this.setValue(this.value + (e.deltaY < 0 ? step : -step), true);
  };

  private onKey = (e: KeyboardEvent): void => {
    const range = this.max - this.min;
    const step = ((e.shiftKey ? 0.5 : 2) * range) / 100;
    if (e.key === "ArrowUp" || e.key === "ArrowRight") {
      e.preventDefault();
      this.setValue(this.value + step, true);
    } else if (e.key === "ArrowDown" || e.key === "ArrowLeft") {
      e.preventDefault();
      this.setValue(this.value - step, true);
    }
  };
}
