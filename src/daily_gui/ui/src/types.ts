export interface ChannelState {
  local: boolean;
  mute: boolean;
  gainDb: number;
  raw: number;
  post: number;
  sel: number;
  wholeBypass: boolean;
  names: string[];
}

export interface AudioSettings {
  currentRate: number;
  actualRate: number;
  currentBuffer: number;
  requestedBuffer: number;
  rebuilding: boolean;
  supportedRates: number[];
  supportedBuffers: number[];
}

export interface Snapshot {
  type: string;
  dual: boolean;
  globalMonitor: boolean;
  mono: boolean;
  ch: ChannelState[];
  asioText: string;
  xrunsText: string;
  picoConnected: boolean;
  picoText: string;
  virtualMicSource: number;
  closeToTray: boolean;
  startMinimized: boolean;
  appVersion: string;
  audioDevice: string;
  hardwareControl: string;
  webview2Version: string;
  uiMode: number;
  systemPeakL: number;
  systemPeakR: number;
  // Windows SYSTEM render-endpoint mute state (endpoint-wide). null = unavailable.
  systemMuted: boolean | null;
  hwMonDb: number | null;
  hwHpDb: number | null;
  hwMonText: string;
  hwHpText: string;
  audio: AudioSettings;
}

export type Command = Record<string, unknown> & { cmd: string };
