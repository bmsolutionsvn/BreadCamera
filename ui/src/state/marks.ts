import type { CameraState } from '../api/protocol';

// Per camera: when holding started (the strip's countdown) and when the last fault
// recovered on its own (the strip's mark), worked out from successive states.
export interface Marks {
  holdingSince?: number;
  recoveredAt?: number;
  faulted: boolean;
}

export function nextMarks(prev: Marks | undefined, cam: CameraState | undefined, now: number): Marks {
  const m: Marks = { ...(prev ?? { faulted: false }) };
  if (cam?.phase === 'holding') m.holdingSince ??= now;
  else m.holdingSince = undefined;
  const faulty =
    cam?.phase === 'holding' ||
    cam?.phase === 'slate' ||
    cam?.input?.health === 'degraded' ||
    Object.values(cam?.outputs ?? {}).some((o) => !!o.error);
  if (faulty) m.faulted = true;
  else if (m.faulted) {
    m.faulted = false;
    m.recoveredAt = now;
  }
  return m;
}

const two = (n: number) => String(n).padStart(2, '0');

export function holdLeft(m: Marks | undefined, holdMs: number, now: number): string | null {
  if (m?.holdingSince === undefined) return null;
  const s = Math.max(0, Math.ceil((holdMs - (now - m.holdingSince)) / 1000));
  return `${two(Math.floor(s / 60))}:${two(s % 60)}`;
}

export function clock(t: number): string {
  const d = new Date(t);
  return `${two(d.getHours())}:${two(d.getMinutes())}`;
}
