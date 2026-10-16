import type { EngineSnapshot } from '../api/client';
import { Icon } from '../components/Icon';
import { rateLabel } from '../state/labels';

export function StatusBar({ snap }: { snap: EngineSnapshot }) {
  const e = snap.state.engine ?? {};
  const open = snap.status === 'open';
  const outs = Object.values(snap.state.cameras ?? {}).flatMap((c) => Object.values(c.outputs ?? {}));
  const live = open ? outs.filter((o) => o.state === 'live').length : 0;
  const failing = open ? outs.filter((o) => o.error).length : 0;
  return (
    <footer className="status" aria-label="Status bar">
      <span>
        <i className={`dot${open ? '' : ' warn'}`} />
        {open ? 'Engine running' : <span className="w">Engine restarting…</span>}
      </span>
      <span>
        <b>{rateLabel(e.rate)}</b> fps
      </span>
      <span>GPU {e.gpu ?? '—'}</span>
      <span className={e.overruns ? 'w' : ''}>
        {e.overruns ? (
          <>
            <Icon name="warn" size={12} />
            {e.overruns} tick overruns
          </>
        ) : (
          'Ticks on time'
        )}
      </span>
      <span className="r">
        Outputs <b>{live} live</b>
        {failing > 0 && <span className="e"> · {failing} retrying</span>}
      </span>
    </footer>
  );
}
