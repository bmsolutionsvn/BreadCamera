import { useEffect, useState } from 'react';

import type { EngineClient } from '../api/client';
import { Prop } from './Prop';

// One native capture mode, as listCaptureModes returns it.
export interface CaptureMode {
  width: number;
  height: number;
  fps: number;
}

// OBS-style: 16:9 sizes read "1080p59.94", others "1440x1080 30".
export const captureModeLabel = (m: CaptureMode) =>
  m.width * 9 === m.height * 16 ? `${m.height}p${m.fps || ''}` : `${m.width}x${m.height} ${m.fps || ''}`.trim();

// A device camera's mode as a select value: "auto" or "1920x1080@59.94".
export const captureModeValue = (p: { width?: unknown; height?: unknown; fps?: unknown }) =>
  Number(p.width) && Number(p.height) ? `${Number(p.width)}x${Number(p.height)}@${Number(p.fps ?? 0)}` : 'auto';

// The input params for a select value: {} for Auto.
export function captureModeParams(value: string): Partial<CaptureMode> {
  const m = /^(\d+)x(\d+)@([\d.]+)$/.exec(value);
  return m ? { width: Number(m[1]), height: Number(m[2]), fps: Number(m[3]) } : {};
}

// The native modes of `device`, fetched when it changes ([] until then, or when none is chosen).
export function useCaptureModes(client: EngineClient, device: string) {
  const [modes, setModes] = useState<CaptureMode[]>([]);
  useEffect(() => {
    setModes([]);
    if (!device) return;
    let live = true;
    client
      .request<CaptureMode[]>('listCaptureModes', { device })
      .then((m) => live && setModes(m))
      .catch(() => live && setModes([]));
    return () => {
      live = false;
    };
  }, [client, device]);
  return modes;
}

// The input Mode picker (spec §4.2): Auto first, then what the device or card offers. A saved mode
// the device does not list right now (unplugged, busy) stays shown so it is not silently lost.
export function ModeSelect({ value, options, hint, onChange }: {
  value: string;
  options: { value: string; label: string }[];
  hint: string;
  onChange: (value: string) => void;
}) {
  const list = value === 'auto' || options.some((o) => o.value === value) ? options : [...options, { value, label: value }];
  return (
    <Prop label="Mode" hint={hint}>
      {(id) => (
        <select id={id} className="ctl" value={value} onChange={(e) => onChange(e.target.value)}>
          <option value="auto">Auto</option>
          {list.map((o) => (
            <option key={o.value} value={o.value}>
              {o.label}
            </option>
          ))}
        </select>
      )}
    </Prop>
  );
}

export const captureModeOptions = (modes: CaptureMode[], value: string) => {
  const opts = modes.map((m) => ({ value: captureModeValue(m), label: captureModeLabel(m) }));
  const cur = captureModeParams(value);
  if (cur.width && !opts.some((o) => o.value === value)) opts.push({ value, label: captureModeLabel(cur as CaptureMode) });
  return opts;
};

export const CAPTURE_MODE_HINT = 'Auto takes the largest size at 25 fps or more. A smaller size or lower rate is made by the device, which saves work here.';
export const DECKLINK_MODE_HINT = 'Auto follows the signal. A fixed mode must match the signal: the card does not scale.';
