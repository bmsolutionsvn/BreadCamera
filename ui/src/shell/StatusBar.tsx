import type { EngineSnapshot } from '../api/client';
import { Icon } from '../components/Icon';
import { rateLabel } from '../state/labels';

// The engine's stage labels, mirrored for the overrun line; an unknown kind falls back to its key.
const STAGE_LABEL: Record<string, string> = {
  cleanup: 'Clean up',
  upscale: 'Upscale',
  color: 'Colour',
  matte: 'Background removal',
  background: 'Background',
};

export function StatusBar({ snap }: { snap: EngineSnapshot }) {
  const e = snap.state.engine ?? {};
  const open = snap.status === 'open';
  const outs = Object.values(snap.state.cameras ?? {}).flatMap((c) => Object.values(c.outputs ?? {}));
  const live = open ? outs.filter((o) => o.state === 'live').length : 0;
  const failing = open ? outs.filter((o) => o.error).length : 0;
  // D15: when budget overruns, name the most expensive stage across every camera (by GPU time).
  const ids = snap.show?.cameras?.map((c) => c.id) ?? Object.keys(snap.state.cameras ?? {});
  let heaviest: { cam: number; label: string; ms: number } | null = null;
  for (let i = 0; i < ids.length; i++) {
    for (const [kind, st] of Object.entries(snap.state.cameras?.[ids[i]]?.stages ?? {})) {
      const ms = st.gpuMs ?? 0;
      if (ms > 0 && ms > (heaviest?.ms ?? 0)) heaviest = { cam: i + 1, label: STAGE_LABEL[kind] ?? kind, ms };
    }
  }
  return (
    <footer className="status" aria-label="Status bar">
      <span>
        <i className={`dot${open ? '' : ' warn'}`} />
        {open ? 'Engine running' : <span className="w">Engine restarting…</span>}
      </span>
      <span title="Sets effect time budgets only; inputs and outputs keep their own rates">
        Project rate <b>{rateLabel(e.rate)}</b> fps
      </span>
      <span>GPU {e.gpu ?? '—'}</span>
      <span className={e.overruns ? 'w' : ''}>
        {e.overruns ? (
          <>
            <Icon name="warn" size={12} />
            {e.overruns} budget overruns
            {heaviest && ` · heaviest: CAM ${heaviest.cam} ${heaviest.label} ${heaviest.ms.toFixed(1)} ms`}
          </>
        ) : (
          'Budget on time'
        )}
      </span>
      <span className="r">
        Outputs <b>{live} live</b>
        {failing > 0 && <span className="e"> · {failing} retrying</span>}
      </span>
    </footer>
  );
}
