import { useState } from 'react';

import type { EngineClient } from '../api/client';
import { useEngine } from '../api/useEngine';
import { ErrorBlock } from '../components/ErrorBlock';
import { Modal } from '../components/Modal';
import { Prop } from '../components/Prop';
import { ConfirmDialog } from './ConfirmDialog';

const RATES = [
  ['50/1', '50'],
  ['60000/1001', '59.94'],
  ['60/1', '60'],
] as const;
type Page = 'show' | 'engine' | 'about';

// Settings is a dialog, not a page (spec §6.4).
export function SettingsDialog({ client, onClose }: { client: EngineClient; onClose: () => void }) {
  const { show, state } = useEngine(client);
  const s = { rate: '60/1', holdMs: 5000, slatePath: '', ...show?.settings };
  const [page, setPage] = useState<Page>('show');
  const [rate, setRate] = useState(s.rate);
  const [hold, setHold] = useState(String(s.holdMs / 1000));
  const [slate, setSlate] = useState(s.slatePath);
  const [confirm, setConfirm] = useState(false);
  const [error, setError] = useState('');
  const engine = state.engine ?? {};

  const holdMs = Math.round(Number(hold) * 1000);
  const holdError = !Number.isFinite(holdMs) || holdMs < 0 || holdMs > 60000 ? 'Hold is 0 to 60 seconds' : '';
  const formatChanged = rate !== s.rate;

  async function save() {
    const args: Record<string, unknown> = {};
    if (rate !== s.rate) args.rate = rate;
    if (holdMs !== s.holdMs) args.holdMs = holdMs;
    if (slate !== s.slatePath) args.slatePath = slate;
    if (Object.keys(args).length === 0) return onClose();
    try {
      await client.request('setSettings', args);
      onClose();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  return (
    <Modal
      title="Settings"
      wide
      onClose={onClose}
      footer={
        <>
          <button className="btn" onClick={onClose}>
            Cancel
          </button>
          <button className="btn primary" disabled={!!holdError} onClick={() => (formatChanged ? setConfirm(true) : save())}>
            Save
          </button>
        </>
      }
    >
      <div className="settings-wrap">
        <div className="cats" role="tablist" aria-label="Settings pages">
          {(['show', 'engine', 'about'] as Page[]).map((p) => (
            <button key={p} role="tab" aria-selected={page === p} className={page === p ? 'on' : ''} onClick={() => setPage(p)}>
              {p === 'show' ? 'Show' : p === 'engine' ? 'Engine' : 'About'}
            </button>
          ))}
        </div>
        <div className="pane" role="tabpanel">
          {error && <ErrorBlock message={error} />}
          {page === 'show' && (
            <>
              <h3>Show format</h3>
              <Prop label="Frame rate">
                {(id) => (
                  <select id={id} className="ctl mono" value={rate} onChange={(e) => setRate(e.target.value)}>
                    {RATES.map(([v, l]) => (
                      <option key={v} value={v}>
                        {l}
                      </option>
                    ))}
                  </select>
                )}
              </Prop>
              <h3>Signal loss</h3>
              <Prop label="Hold the last frame on signal loss (s)" hint={holdError}>
                {(id) => <input id={id} className="ctl mono" type="number" min={0} max={60} step={0.5} value={hold} onChange={(e) => setHold(e.target.value)} />}
              </Prop>
              <Prop label="Slate image" hint="Leave empty for the built-in slate.">
                {(id) => <input id={id} className="ctl mono" value={slate} placeholder="C:\Shows\slate.png" onChange={(e) => setSlate(e.target.value)} />}
              </Prop>
            </>
          )}
          {page === 'engine' && (
            <>
              <h3>Engine</h3>
              {Object.entries(engine.warnings ?? {})
                .filter(([, w]) => w)
                .map(([name, w]) => (
                  <ErrorBlock key={name} severity="warning" message={w} />
                ))}
              <Prop label="GPU">{() => <span className="ro">{engine.gpu ?? '—'}</span>}</Prop>
              <Prop label="Background removal">{() => <span className="ro">{engine.matteBackend || 'None'}</span>}</Prop>
              <Prop label="Preview encoder">{() => <span className="ro">{engine.previewEncoder || 'None'}</span>}</Prop>
            </>
          )}
          {page === 'about' && (
            <>
              <h3>BreadCamera 0.1 — BM Solutions</h3>
              <p className="note">Third-party licences: THIRD_PARTY_NOTICES.md in the install folder.</p>
            </>
          )}
        </div>
      </div>
      {confirm && (
        <ConfirmDialog
          title="Change the show format?"
          body="Every output restarts and loses picture for about 1 second."
          confirm="Change format"
          danger={false}
          onCancel={() => setConfirm(false)}
          onConfirm={() => {
            setConfirm(false);
            save();
          }}
        />
      )}
    </Modal>
  );
}