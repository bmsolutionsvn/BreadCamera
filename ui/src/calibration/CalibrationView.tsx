import { useEffect, useRef, type PointerEvent } from 'react';

import type { EngineClient } from '../api/client';
import type { CameraConfig } from '../api/protocol';
import { Icon } from '../components/Icon';
import { PreviewCanvas } from '../preview/PreviewCanvas';
import { chartPoint, type Pt } from './chartGeometry';
import type { CalibrationState } from './useCalibration';

// The calibrating camera alone, with the chart corners on top (spec §6.4).
export function CalibrationView({ client, cam, n, state, onCorner }: { client: EngineClient; cam: CameraConfig; n: number; state: CalibrationState; onCorner: (i: number, p: Pt) => void }) {
  const box = useRef<HTMLDivElement>(null);
  const drag = useRef<number | null>(null);
  useEffect(() => client.setPreviewSubscription([cam.id], 'large'), [client, cam.id]);

  function at(e: PointerEvent): Pt {
    const r = box.current!.getBoundingClientRect();
    return [Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)), Math.min(1, Math.max(0, (e.clientY - r.top) / r.height))];
  }
  const q = state.corners;
  const squares: Pt[] = [];
  for (let p = 0; p < 24; p++) squares.push(chartPoint(q, ((p % 6) + 0.5) / 6, (Math.floor(p / 6) + 0.5) / 4));

  return (
    <section className="center" aria-label="Calibrating">
      <div className="solo-bar">
        <Icon name="chart" size={14} />
        <b>
          CAM {n} · {cam.name}
        </b>
        <span>Calibrating colour</span>
        <span className="right">Other cameras keep running</span>
      </div>
      <div className="cal-view">
        <div
          className="pic cal-pic"
          ref={box}
          onPointerMove={(e) => drag.current !== null && onCorner(drag.current, at(e))}
          onPointerUp={() => (drag.current = null)}
          onPointerLeave={() => (drag.current = null)}
        >
          <PreviewCanvas client={client} cameraId={cam.id} size="large" />
          <svg className="chart-overlay" viewBox="0 0 1 1" preserveAspectRatio="none" aria-label="Chart corners">
            <polygon points={q.map((p) => p.join(',')).join(' ')} />
            {squares.map(([x, y], i) => (
              <rect key={i} x={x - 0.006} y={y - 0.009} width={0.012} height={0.018} className="sample" />
            ))}
          </svg>
          {q.map(([x, y], i) => (
            <button
              key={i}
              className="corner"
              aria-label={`Chart corner ${i + 1}`}
              style={{ left: `${x * 100}%`, top: `${y * 100}%` }}
              onPointerDown={(e) => {
                (e.target as Element).setPointerCapture?.(e.pointerId);
                drag.current = i;
              }}
            />
          ))}
        </div>
      </div>
    </section>
  );
}