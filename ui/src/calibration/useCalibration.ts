import { useCallback, useEffect, useRef, useState } from 'react';

import type { EngineClient } from '../api/client';
import { DEFAULT_CORNERS, type Pt } from './chartGeometry';

export interface CalibrationState {
  cameraId: string;
  chart: 'card' | 'tablet';
  corners: Pt[];
  found: boolean;
  manual: boolean;
  detection: boolean | null; // null = not asked yet
  busy: boolean;
  error: string;
}

// One camera at a time (spec §6.4): find the chart, let the operator fix the corners, capture.
export function useCalibration(client: EngineClient) {
  const [state, setState] = useState<CalibrationState | null>(null);
  const live = useRef<CalibrationState | null>(null);
  live.current = state;

  useEffect(() => {
    if (!state || state.found || state.manual || state.busy || state.detection === false) return;
    let stop = false;
    const look = () =>
      client
        .request<{ available: boolean; found: boolean; corners?: Pt[] }>('findChart', { cameraId: state.cameraId })
        .then((r) => {
          if (stop || !live.current || live.current.manual) return;
          setState({ ...live.current, detection: r.available, found: r.found, corners: r.found && r.corners ? r.corners : live.current.corners });
        })
        .catch(() => {});
    look();
    const t = setInterval(look, 1000);
    return () => {
      stop = true;
      clearInterval(t);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [client, state?.cameraId, state?.found, state?.manual, state?.busy, state?.detection]);

  const start = useCallback(
    (cameraId: string) => setState({ cameraId, chart: 'card', corners: DEFAULT_CORNERS, found: false, manual: false, detection: null, busy: false, error: '' }),
    [],
  );
  const cancel = useCallback(() => setState(null), []);
  const setChart = useCallback((chart: 'card' | 'tablet') => setState((s) => (s ? { ...s, chart } : s)), []);
  const moveCorner = useCallback((i: number, p: Pt) => setState((s) => (s ? { ...s, manual: true, corners: s.corners.map((c, k) => (k === i ? p : c)) } : s)), []);
  const capture = useCallback(async () => {
    const s = live.current;
    if (!s) return;
    setState({ ...s, busy: true, error: '' });
    try {
      await client.request('calibrateColor', { cameraId: s.cameraId, chart: s.chart, corners: s.corners });
      setState(null);
    } catch (e) {
      setState({ ...s, busy: false, error: e instanceof Error ? e.message : String(e) });
    }
  }, [client]);

  return { state, start, cancel, setChart, moveCorner, capture };
}
export type Calibration = ReturnType<typeof useCalibration>;