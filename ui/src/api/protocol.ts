export type SignalState = 'ready' | 'connecting' | 'live' | 'offline';
export type Phase = 'nosignal' | 'live' | 'holding' | 'slate';
export type InputHealth = 'receiving' | 'degraded' | 'down' | 'offline';
export interface ErrorInfo {
  code: string;
  message: string;
}
export interface OutputState {
  kind: string;
  state: SignalState;
  error: ErrorInfo | null;
}
export interface CameraState {
  name: string;
  phase?: Phase;
  input?: { state: SignalState; error: ErrorInfo | null; health?: InputHealth };
  outputs?: Record<string, OutputState>;
  stages?: Record<string, StageState>;
  tickError?: string;
}
export interface StageState {
  state: 'off' | 'active' | 'bypassed';
  error: ErrorInfo | null;
}
export interface StageField {
  key: string;
  type: 'number' | 'enum' | 'bool' | 'color' | 'path';
  default: unknown;
  label: string;
  min?: number;
  max?: number;
  step?: number;
  options?: string[];
  optionLabels?: string[];
  hidden?: boolean;
}
export interface StageSchemaJson {
  kind: string;
  label: string;
  description: string;
  icon: string;
  requires: string[];
  onAdd: Record<string, unknown>;
  fields: StageField[];
}
export interface StateTree {
  engine?: {
    gpu?: string;
    rate?: string;
    resolution?: string;
    overruns?: number;
    previewEncoder?: string;
    warnings?: Record<string, string>;
  };
  cameras?: Record<string, CameraState>;
}
export interface OutputFormat {
  width: number;
  height: number;
  halfRate: boolean;
}
export interface OutputConfig {
  id: string;
  kind: string;
  keyed: boolean;
  enabled: boolean;
  params: Record<string, unknown>;
  format?: OutputFormat;
}
export interface CameraConfig {
  id: string;
  name: string;
  input: { kind: string; params: Record<string, unknown> };
  outputs: OutputConfig[];
  stages?: Record<string, Record<string, unknown>>;
}
export interface ShowJson {
  schemaVersion?: number;
  settings?: { rate: string; resolution?: string; holdMs: number; slatePath: string };
  cameras: CameraConfig[];
}
export interface PatchOp {
  op: 'replace' | 'remove';
  path: string;
  value?: unknown;
}
export interface Stats {
  engine: { overruns: number };
  cameras: Record<string, { fps: number; drops: number; repeats: number; latencyMs: number }>;
}
export interface PreviewMessage {
  cameraId: string;
  keyframe: boolean;
  ptsUs: number;
  data: Uint8Array;
}
export type PreviewSize = 'small' | 'large';

// Binary layout from the engine (spec §6.3).
export function parsePreviewMessage(buf: ArrayBuffer): PreviewMessage {
  const v = new DataView(buf);
  if (buf.byteLength < 4 || v.getUint8(0) !== 1) throw new Error('unknown preview message');
  const idLen = v.getUint16(2, true);
  const ptsAt = 4 + idLen;
  if (buf.byteLength < ptsAt + 8) throw new Error('short preview message');
  return {
    cameraId: new TextDecoder().decode(new Uint8Array(buf, 4, idLen)),
    keyframe: (v.getUint8(1) & 1) === 1,
    ptsUs: Number(v.getBigInt64(ptsAt, true)),
    data: new Uint8Array(buf, ptsAt + 8),
  };
}
