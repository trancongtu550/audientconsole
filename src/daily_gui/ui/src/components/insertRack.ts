import type { ChannelState } from "../types";

interface RackHandlers {
  onSelect: (idx: number) => void;
  onEdit: (idx: number) => void;
  onBypass: (idx: number) => void;
  onRemove: (idx: number) => void;
  onLoad: () => void;
  onWholeBypass: () => void;
}

function parseName(raw: string): { name: string; bypassed: boolean } {
  let name = raw.replace(/^\d+:\s*/, "");
  let bypassed = false;
  if (/\s*\[BYP\]\s*$/.test(name)) {
    bypassed = true;
    name = name.replace(/\s*\[BYP\]\s*$/, "");
  }
  return { name, bypassed };
}

const PLUG_ICON = `<svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="8"/><line x1="12" y1="4" x2="12" y2="12"/><line x1="12" y1="12" x2="17" y2="16"/></svg>`;

export class InsertRack {
  readonly el: HTMLDivElement;
  private readonly addBtn: HTMLButtonElement;
  private readonly wholeBtn: HTMLButtonElement;
  private readonly list: HTMLDivElement;
  private readonly handlers: RackHandlers;
  private signature = "";
  private visibleRows = 0;

  constructor(handlers: RackHandlers) {
    this.handlers = handlers;
    const el = document.createElement("div");
    el.className = "inserts";
    el.innerHTML = `
      <div class="inserts-head">
        <span class="inserts-title">INSERTS</span>
        <span class="inserts-rule"></span>
        <span class="inserts-actions">
          <button type="button" class="mini-btn" data-a="add" title="Add a VST3 insert">+ ADD INSERT</button>
          <button type="button" class="mini-btn ghost" data-a="whole" title="Bypass the whole insert chain">WHOLE BYPASS</button>
        </span>
      </div>
      <div class="slotlist"></div>`;
    this.el = el;
    this.addBtn = el.querySelector('[data-a="add"]') as HTMLButtonElement;
    this.wholeBtn = el.querySelector('[data-a="whole"]') as HTMLButtonElement;
    this.list = el.querySelector(".slotlist") as HTMLDivElement;
    this.addBtn.addEventListener("click", () => this.handlers.onLoad());
    this.wholeBtn.addEventListener("click", () => this.handlers.onWholeBypass());
  }

  // Shared visible row count across BOTH channels so rack heights (and thus panel
  // bottoms) always align regardless of insert count.
  setVisibleRows(rows: number): void {
    const n = Math.max(1, Math.min(3, Math.floor(rows)));
    if (this.visibleRows === n) return;
    this.visibleRows = n;
    this.list.classList.remove("rows-1", "rows-2", "rows-3");
    this.list.classList.add(`rows-${n}`);
  }

  update(state: ChannelState): void {
    this.wholeBtn.classList.toggle("on", state.wholeBypass);
    this.wholeBtn.textContent = "WHOLE BYPASS";

    const sig = JSON.stringify([state.names, state.sel, state.wholeBypass]);
    if (sig === this.signature) return;
    this.signature = sig;

    this.list.innerHTML = "";
    if (state.names.length === 0) {
      const empty = document.createElement("div");
      empty.className = "inserts-empty";
      empty.textContent = "No inserts";
      this.list.appendChild(empty);
      return;
    }

    state.names.forEach((raw, idx) => {
      const { name, bypassed } = parseName(raw);
      const selected = idx === state.sel;
      const slot = document.createElement("div");
      slot.className = `slot${selected ? " selected" : ""}${bypassed ? " bypassed" : ""}`;
      slot.innerHTML = `
        <span class="slot-icon">${PLUG_ICON}</span>
        <div class="slot-main">
          <div class="slot-name" title="${escapeHtml(name)}">${escapeHtml(name)}</div>
          <div class="slot-sub">VST3</div>
        </div>
        <div class="slot-actions">
          <button type="button" class="slot-btn" data-a="edit">EDIT</button>
          <button type="button" class="slot-btn${bypassed ? " active" : ""}" data-a="byp">BYPASS</button>
          <button type="button" class="slot-btn danger" data-a="rem">REMOVE</button>
        </div>
        <button type="button" class="slot-menu" disabled aria-label="More options" title="More (not available)">&#8942;</button>`;
      slot.addEventListener("click", (e) => {
        if ((e.target as HTMLElement).closest("button")) return;
        this.handlers.onSelect(idx);
      });
      (slot.querySelector('[data-a="edit"]') as HTMLButtonElement).addEventListener("click", () => this.handlers.onEdit(idx));
      (slot.querySelector('[data-a="byp"]') as HTMLButtonElement).addEventListener("click", () => this.handlers.onBypass(idx));
      (slot.querySelector('[data-a="rem"]') as HTMLButtonElement).addEventListener("click", () => this.handlers.onRemove(idx));
      this.list.appendChild(slot);
    });
  }
}

function escapeHtml(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}
