import { onSnapshot, post } from "./bridge";
import { Channel } from "./components/channel";
import { Master } from "./components/master";
import { MiniMonitor } from "./components/miniMonitor";
import { SettingsPanel } from "./components/settings";
import { TopBar } from "./components/topbar";
import type { Snapshot } from "./types";

const app = document.getElementById("app") as HTMLElement;
const topbar = new TopBar(document.getElementById("topbar") as HTMLElement);
const settings = new SettingsPanel(document.getElementById("btnSettings") as HTMLButtonElement);

const topbarEl = document.getElementById("topbar") as HTMLElement;
const mainEl = document.getElementById("main") as HTMLElement;
const miniEl = document.getElementById("mini") as HTMLElement;

const panelCh0 = document.getElementById("panelCh0") as HTMLElement;
const panelCh1 = document.getElementById("panelCh1") as HTMLElement;
const panelMaster = document.getElementById("panelMaster") as HTMLElement;

const ch0 = new Channel(0, panelCh0);
const ch1 = new Channel(1, panelCh1);
const master = new Master(panelMaster);
const mini = new MiniMonitor(miniEl);

let lastDual = true;
let lastMode = -1;
let lastRows = 0;

onSnapshot((snap: Snapshot) => {
  topbar.update(snap);
  settings.update(snap);
  const chans = snap.ch || [];
  if (chans[0]) ch0.update(chans[0]);
  if (chans[1]) ch1.update(chans[1]);
  // Shared insert-rack height: both channels use the same visible row count
  // (1..3) so their panels stay geometrically aligned.
  const count0 = chans[0]?.names?.length ?? 0;
  const count1 = chans[1]?.names?.length ?? 0;
  const sharedRows = Math.max(1, Math.min(3, Math.max(count0, count1)));
  ch0.setVisibleRows(sharedRows);
  ch1.setVisibleRows(sharedRows);
  if (sharedRows !== lastRows) {
    lastRows = sharedRows;
    // Main window height hugs the shared insert-row count (host resizes only).
    post({ cmd: "setMainRows", rows: sharedRows });
  }
  const dual = !!snap.dual;
  if (dual !== lastDual) {
    panelCh1.style.display = dual ? "" : "none";
    mainEl.style.gridTemplateColumns = dual ? "" : "minmax(0, 1fr) minmax(0, 0.62fr)";
    lastDual = dual;
  }
  master.update(snap);
  mini.update(snap);

  // Presentation-only view switch: never restarts audio. The host resizes the
  // fixed window to the mode's DIP size.
  const mode = snap.uiMode === 1 ? 1 : 0;
  if (mode !== lastMode) {
    lastMode = mode;
    topbarEl.hidden = mode === 1;
    mainEl.hidden = mode === 1;
    miniEl.hidden = mode !== 1;
    app.classList.toggle("mode-mini", mode === 1);
  }
});

function frame(now: number): void {
  ch0.rawMeter.tick(now);
  ch0.postMeter.tick(now);
  ch1.rawMeter.tick(now);
  ch1.postMeter.tick(now);
  mini.sysMeter.tick(now);
  requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
