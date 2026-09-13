import type { Command, Snapshot } from "./types";

interface WebViewHost {
  postMessage: (message: string) => void;
}

function host(): WebViewHost | undefined {
  const chrome = (window as unknown as { chrome?: { webview?: WebViewHost } }).chrome;
  return chrome?.webview;
}

export function post(cmd: Command): void {
  const wv = host();
  if (!wv) return;
  try {
    wv.postMessage(JSON.stringify(cmd));
  } catch {
    /* presentation layer only; never throw */
  }
}

export function onSnapshot(handler: (snap: Snapshot) => void): void {
  (window as unknown as { __audientOnSnapshot?: (snap: Snapshot) => void }).__audientOnSnapshot = (snap) => {
    try {
      handler(snap);
    } catch {
      /* ignore render errors */
    }
  };
}
