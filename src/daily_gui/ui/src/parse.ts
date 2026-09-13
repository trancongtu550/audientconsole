export interface AsioInfo {
  state: string;
  rateK: number;
  buffer: number;
  connected: boolean;
}

export function parseAsio(text: string): AsioInfo {
  const m = /ASIO\s+(\S+)\s+(\d+)k\s*\/\s*(\d+)/i.exec(text || "");
  if (m) {
    const state = m[1];
    const lower = state.toLowerCase();
    return {
      state,
      rateK: Number(m[2]),
      buffer: Number(m[3]),
      connected: lower.includes("stream") || lower.includes("connect") || lower.includes("ready"),
    };
  }
  const lower = (text || "").toLowerCase();
  return {
    state: (text || "ASIO --").replace(/^ASIO\s*/i, "") || "--",
    rateK: 48,
    buffer: 0,
    connected: lower.includes("connect") || lower.includes("stream") || lower.includes("ready"),
  };
}

export interface XrunInfo {
  xruns: number;
  overloads: number;
}

export function parseXruns(text: string): XrunInfo {
  const m = /Xruns\s+(\d+)\s+Ovl\s+(\d+)/i.exec(text || "");
  if (m) return { xruns: Number(m[1]), overloads: Number(m[2]) };
  return { xruns: 0, overloads: 0 };
}
