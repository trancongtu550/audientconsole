// Stereo SYSTEM meter for the Mini Monitor: two narrow, fixed-geometry bars
// driven by the iD14 render endpoint's real per-channel peaks (L/R). Audio
// activity only ever changes the fill height and the numeric text; the rails and
// the surrounding layout never change size. Shared dB scale on the right.
const DB_MIN = -60;

function linearToDb(linear: number): number {
  if (!isFinite(linear) || linear <= 0.0001) return -120;
  return 20 * Math.log10(linear);
}

export class StereoMeter {
  readonly el: HTMLDivElement;
  private readonly coverL: HTMLDivElement;
  private readonly coverR: HTMLDivElement;
  private readonly readL: HTMLElement;
  private readonly readR: HTMLElement;
  private targetL = DB_MIN;
  private targetR = DB_MIN;
  private dispL = DB_MIN;
  private dispR = DB_MIN;
  private holdL = DB_MIN;
  private holdR = DB_MIN;
  private holdUntil = 0;
  private unavailable = false;

  constructor(label: string) {
    const col = document.createElement("div");
    col.className = "stereo-meter";
    col.innerHTML = `
      <div class="sm-label">${label}</div>
      <div class="sm-body">
        <div class="sm-bar"><div class="sm-cover" style="height:100%"></div></div>
        <div class="sm-bar"><div class="sm-cover" style="height:100%"></div></div>
      </div>
      <div class="sm-readouts">
        <span class="sm-read" data-ch="l">L --</span>
        <span class="sm-read" data-ch="r">R --</span>
      </div>`;
    this.el = col;
    const covers = col.querySelectorAll<HTMLDivElement>(".sm-cover");
    this.coverL = covers[0];
    this.coverR = covers[1];
    this.readL = col.querySelector('[data-ch="l"]') as HTMLElement;
    this.readR = col.querySelector('[data-ch="r"]') as HTMLElement;
  }

  setPeaks(l: number, r: number): void {
    if (l < 0 || r < 0) {
      this.unavailable = true;
      return;
    }
    this.unavailable = false;
    this.targetL = Math.max(DB_MIN, Math.min(0, linearToDb(l)));
    this.targetR = Math.max(DB_MIN, Math.min(0, linearToDb(r)));
  }

  tick(now: number): void {
    if (this.unavailable) {
      this.coverL.style.height = "100%";
      this.coverR.style.height = "100%";
      this.readL.textContent = "L --";
      this.readR.textContent = "R --";
      this.readL.classList.remove("hot", "clip");
      this.readR.classList.remove("hot", "clip");
      return;
    }
    const upd = (
      target: number,
      disp: number,
      hold: number,
    ): [number, number] => {
      let d = disp;
      if (target > d) d = d * 0.4 + target * 0.6;
      else d = d * 0.88 + target * 0.12;
      let h = hold;
      if (target > h) {
        h = target;
        this.holdUntil = now + 900;
      } else if (now > this.holdUntil) {
        h = Math.max(h - 0.4, d);
      }
      return [d, h];
    };
    [this.dispL, this.holdL] = upd(this.targetL, this.dispL, this.holdL);
    [this.dispR, this.holdR] = upd(this.targetR, this.dispR, this.holdR);
    const normL = Math.max(0, Math.min(1, (this.dispL - DB_MIN) / -DB_MIN));
    const normR = Math.max(0, Math.min(1, (this.dispR - DB_MIN) / -DB_MIN));
    this.coverL.style.height = `${((1 - normL) * 100).toFixed(1)}%`;
    this.coverR.style.height = `${((1 - normR) * 100).toFixed(1)}%`;
    const fmt = (d: number) => (d <= DB_MIN + 0.5 ? "-inf" : d.toFixed(1));
    this.readL.textContent = `L ${fmt(this.dispL)}`;
    this.readR.textContent = `R ${fmt(this.dispR)}`;
    this.readL.classList.toggle("clip", this.dispL > -3);
    this.readR.classList.toggle("clip", this.dispR > -3);
    this.readL.classList.toggle("hot", this.dispL <= -3 && this.dispL > -12);
    this.readR.classList.toggle("hot", this.dispR <= -3 && this.dispR > -12);
  }
}
