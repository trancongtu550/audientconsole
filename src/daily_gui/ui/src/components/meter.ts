const DB_MIN = -60;
const TICKS = [0, -6, -12, -18, -24, -36, -60];

export function linearToDb(linear: number): number {
  if (!isFinite(linear) || linear <= 0.0001) return -120;
  return 20 * Math.log10(linear);
}

export type ScaleSide = "left" | "right" | "none";

export class Meter {
  readonly el: HTMLDivElement;
  private readonly cover: HTMLDivElement;
  private readonly holdEl: HTMLDivElement;
  private readonly caps: HTMLDivElement;
  private targetDb = DB_MIN;
  private dispDb = DB_MIN;
  private holdDb = DB_MIN;
  private holdUntil = 0;
  private unavailable = false;

  constructor(label: string, scaleSide: ScaleSide = "none") {
    const scale =
      scaleSide === "none"
        ? ""
        : `<div class="meter-scale ${scaleSide}">${TICKS.map((db) => {
            const top = (-db / 60) * 100;
            const major = db === 0;
            return `<div class="tick ${major ? "major" : ""}" style="top:${top}%"><i></i><span>${db}</span></div>`;
          }).join("")}</div>`;
    const col = document.createElement("div");
    col.className = "meter-col";
    col.innerHTML = `
      <div class="meter-top-label">${label}</div>
      <div class="meter-body">
        ${scaleSide === "left" ? scale : ""}
        <div class="meter">
          <div class="grad"></div>
          <div class="cover"></div>
          <div class="seg-mask"></div>
          <div class="hold" style="top:100%"></div>
        </div>
        ${scaleSide === "right" ? scale : ""}
      </div>
      <div class="value-capsule">-inf dB</div>`;
    this.el = col;
    this.cover = col.querySelector(".cover") as HTMLDivElement;
    this.holdEl = col.querySelector(".hold") as HTMLDivElement;
    this.caps = col.querySelector(".value-capsule") as HTMLDivElement;
  }

  setPeak(linear: number): void {
    this.targetDb = Math.max(DB_MIN, Math.min(0, linearToDb(linear)));
  }

  setUnavailable(v: boolean): void {
    this.unavailable = v;
    if (v) {
      this.dispDb = DB_MIN;
      this.holdDb = DB_MIN;
      this.cover.style.height = "100%";
      this.holdEl.style.display = "none";
      this.caps.textContent = "--";
      this.caps.classList.remove("clip", "hot");
    }
  }

  tick(now: number): void {
    if (this.unavailable) return;
    const target = this.targetDb;
    if (target > this.dispDb) {
      this.dispDb = this.dispDb * 0.4 + target * 0.6;
    } else {
      this.dispDb = this.dispDb * 0.88 + target * 0.12;
    }
    if (target > this.holdDb) {
      this.holdDb = target;
      this.holdUntil = now + 900;
    } else if (now > this.holdUntil) {
      this.holdDb = Math.max(this.holdDb - 0.4, this.dispDb);
    }
    const norm = Math.max(0, Math.min(1, (this.dispDb - DB_MIN) / -DB_MIN));
    this.cover.style.height = `${((1 - norm) * 100).toFixed(2)}%`;
    const holdNorm = Math.max(0, Math.min(1, (this.holdDb - DB_MIN) / -DB_MIN));
    this.holdEl.style.top = `${((1 - holdNorm) * 100).toFixed(2)}%`;
    this.holdEl.style.display = this.holdDb > DB_MIN + 1 ? "block" : "none";

    const text = this.dispDb <= DB_MIN + 0.5 ? "-inf" : this.dispDb.toFixed(1);
    this.caps.textContent = `${text} dB`;
    this.caps.classList.toggle("clip", this.dispDb > -6);
    this.caps.classList.toggle("hot", this.dispDb <= -6 && this.dispDb > -18);
  }
}
