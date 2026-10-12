import { useState } from 'react';

import type { EngineClient } from '../api/client';
import type { CameraConfig } from '../api/protocol';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon } from '../components/Icon';
import { ConfirmDialog } from '../dialogs/ConfirmDialog';
import { clock } from '../state/marks';
import { downloadChartImage } from './chartImage';
import type { Calibration } from './useCalibration';

const WORDS = { good: ['Good match', 'ok-line'], fair: ['Fair match', 'warn-line'], poor: ['Poor match — check the lighting and glare', 'bad-line'] } as const;
const rating = (d: number) => (d <= 2 ? 'good' : d <= 4 ? 'fair' : 'poor');

// The Colour effect's body: the result in words, or the calibrate controls (spec §5.3, §6.4).
export function ColourPanel({ client, cam, n, calib }: { client: EngineClient; cam: CameraConfig; n: number; calib: Calibration }) {
  const [clearing, setClearing] = useState(false);
  const [error, setError] = useState('');
  const p = (cam.stages?.color ?? {}) as Record<string, unknown>;
  const s = calib.state?.cameraId === cam.id ? calib.state : null;

  if (s) {
    const status = s.found ? '✓ Chart found, corners placed' : s.detection === false || s.manual ? `Drag the four corners onto the chart` : 'Looking for the chart…';
    return (
      <>
        <ErrorBlock severity="info" title="Calibrating." message={`Hold the chart in front of CAM ${n} under show lighting.`} />
        {s.error && <ErrorBlock title="Capture failed." message={s.error} />}
        <div className="prop">
          <label>Chart</label>
          <span className="seg" role="radiogroup" aria-label="Chart">
            {(['card', 'tablet'] as const).map((c) => (
              <button key={c} role="radio" aria-checked={s.chart === c} className={s.chart === c ? 'on' : ''} onClick={() => calib.setChart(c)}>
                {c === 'card' ? 'Printed card' : 'Tablet'}
              </button>
            ))}
          </span>
        </div>
        <div className="prop">
          <label>Status</label>
          <span className={s.found ? 'ok-line' : 'ro'}>{status}</span>
        </div>
        <div className="row-btns">
          <button className="btn primary" disabled={s.busy} onClick={calib.capture}>
            <Icon name="check" size={14} />
            {s.busy ? 'Capturing…' : 'Capture'}
          </button>
          <button className="btn" onClick={calib.cancel}>
            Cancel
          </button>
        </div>
        <p className="note">Drag a corner only if it is wrong. Tablet: show the chart image at full brightness.</p>
        <button className="link" onClick={downloadChartImage}>
          Save chart image for a tablet…
        </button>
      </>
    );
  }

  const lut = String(p.lut ?? '');
  const at = Number(p.calibratedAt ?? 0) * 1000;
  const d = Number(p.deltaE ?? 0);
  const [word, cls] = WORDS[rating(d)];
  const tip = `Average colour error ${Number(p.deltaEBefore ?? 0).toFixed(1)} before, ${d.toFixed(1)} after (ΔE2000) · ${p.chart === 'tablet' ? 'tablet' : 'printed card'}`;
  async function run(cmd: string) {
    try {
      await client.request(cmd, { cameraId: cam.id });
      setError('');
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }
  return (
    <>
      {error && <ErrorBlock message={error} />}
      <div className="prop">
        <label>Calibration</label>
        {!lut ? (
          <span className="ro">Not calibrated</span>
        ) : p.reference ? (
          <span className="ok-line" title="The other tablet cameras are matched to this one">
            Tablet reference · {clock(at)}
          </span>
        ) : (
          <span className={cls} title={tip}>
            {rating(d) === 'good' && <Icon name="check" size={12} />}
            {word} · {clock(at)}
          </span>
        )}
      </div>
      <div className="row-btns">
        <button className="btn" disabled={calib.state !== null} onClick={() => calib.start(cam.id)}>
          <Icon name="chart" size={14} />
          {lut ? 'Calibrate again' : 'Calibrate'}
        </button>
        {String(p.previousLut ?? '') && (
          <button className="btn" onClick={() => run('undoColor')}>
            Undo
          </button>
        )}
        {lut && (
          <button className="btn ghost-danger" onClick={() => setClearing(true)}>
            Clear…
          </button>
        )}
      </div>
      {clearing && (
        <ConfirmDialog
          title="Clear calibration?"
          body={`CAM ${n} goes back to its uncorrected colours now.`}
          confirm="Clear calibration"
          onCancel={() => setClearing(false)}
          onConfirm={async () => {
            await run('clearColor');
            setClearing(false);
          }}
        />
      )}
    </>
  );
}