import { useState } from 'react';

import type { EngineClient, EngineSnapshot } from '../api/client';
import type { CameraConfig } from '../api/protocol';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon } from '../components/Icon';
import { Prop } from '../components/Prop';
import { ConfirmDialog } from '../dialogs/ConfirmDialog';
import { clock } from '../state/marks';
import { downloadChartImage } from './chartImage';
import type { Calibration } from './useCalibration';

const WORDS = { good: ['Good match', 'ok-line'], fair: ['Fair match', 'warn-line'], poor: ['Poor match — check the lighting and glare', 'bad-line'] } as const;
const rating = (d: number) => (d <= 2 ? 'good' : d <= 4 ? 'fair' : 'poor');

// spec D9/D12: the Match state the engine publishes, in words. `ref` is `CAM <n>` when it is known.
function matchLabel(match: string, ref: string): string {
  if (match === 'matching') return `Matching ${ref}`;
  if (match === 'auto') return 'Auto balance';
  if (match === 'holding-no-face') return 'Holding — no face found';
  if (match === 'holding-reference-down') return ref ? `Holding — ${ref} has no picture` : 'Holding — the reference camera is gone';
  return '';
}

// The Colour effect's body (spec §5.3, §6.4): Calibrate against a chart, or Match another camera
// (spec D9); the calibrate controls are the result in words, or the live calibration view.
export function ColourPanel({ client, snap, cam, n, calib }: { client: EngineClient; snap: EngineSnapshot; cam: CameraConfig; n: number; calib: Calibration }) {
  const [clearing, setClearing] = useState(false);
  const [error, setError] = useState('');
  const p = (cam.stages?.color ?? {}) as Record<string, unknown>;
  const s = calib.state?.cameraId === cam.id ? calib.state : null;
  const mode = String(p.mode ?? 'calibrate');

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
  // The other cameras as the Inspector numbers them (CAM 1, CAM 2 …), for the Match to picker.
  const others = (snap.show?.cameras ?? []).map((c, i) => ({ id: c.id, n: i + 1, name: c.name })).filter((c) => c.id !== cam.id);
  const ref = others.find((c) => c.id === String(p.matchTo ?? ''));
  const matchLine = matchLabel(String(snap.state.cameras?.[cam.id]?.stages?.color?.match ?? ''), ref ? `CAM ${ref.n}` : '');
  async function run(cmd: string) {
    try {
      await client.request(cmd, { cameraId: cam.id });
      setError('');
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }
  async function setColour(params: Record<string, unknown>) {
    try {
      await client.request('setStage', { cameraId: cam.id, stage: 'color', params });
      setError('');
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }
  return (
    <>
      <div className="prop">
        <label>Mode</label>
        <span className="seg" role="radiogroup" aria-label="Colour mode">
          {(['calibrate', 'match'] as const).map((m) => (
            <button key={m} role="radio" aria-checked={mode === m} className={mode === m ? 'on' : ''} onClick={() => setColour({ mode: m })}>
              {m === 'calibrate' ? 'Calibrate' : 'Match'}
            </button>
          ))}
        </span>
      </div>
      {error && <ErrorBlock message={error} />}
      {mode === 'match' ? (
        <>
          <Prop label="Match to">
            {(id) => (
              <select id={id} className="ctl" value={String(p.matchTo ?? '')} onChange={(e) => setColour({ matchTo: e.target.value })}>
                <option value="">Auto balance</option>
                {others.map((c) => (
                  <option key={c.id} value={c.id}>{`CAM ${c.n} · ${c.name}`}</option>
                ))}
                {/* A reference removed from the show still holds (spec D12); keep the picker on it, not on Auto balance. */}
                {String(p.matchTo ?? '') && !ref && <option value={String(p.matchTo)}>Camera not in this show</option>}
              </select>
            )}
          </Prop>
          {matchLine && (
            <div className="prop">
              <label>State</label>
              <span className="ro">{matchLine}</span>
            </div>
          )}
        </>
      ) : (
        <>
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
      )}
    </>
  );
}