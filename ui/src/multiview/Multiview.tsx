import { useEffect, useLayoutEffect, useMemo, useRef, useState, type KeyboardEvent } from 'react';

import type { EngineClient } from '../api/client';
import type { CameraConfig, StateTree } from '../api/protocol';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon } from '../components/Icon';
import { PreviewCanvas } from '../preview/PreviewCanvas';
import type { Marks } from '../state/marks';
import { Strip } from './Strip';

export type Layout = 'auto' | '2x2' | '3x2' | '4x2';
const AUTO: Record<number, [number, number]> = { 1: [1, 1], 2: [2, 1], 3: [2, 2], 4: [2, 2], 5: [3, 2], 6: [3, 2], 7: [4, 2], 8: [4, 2] };
export function gridOf(layout: Layout, n: number): [number, number] {
  if (layout === 'auto') return AUTO[Math.min(Math.max(n, 1), 8)];
  const [c, r] = layout.split('x').map(Number);
  return [c, r];
}

const STRIP = 29;
const GAP = 3;

// Tiles keep 16:9 and the whole wall fits the area (docs/design/multiview.md §6).
export function Multiview({ client, cameras, state, marks, holdMs, now, selected, onSelect, layout, onLayout, offline }: {
  client: EngineClient;
  cameras: CameraConfig[];
  state: StateTree;
  marks: Record<string, Marks>;
  holdMs: number;
  now: number;
  selected: string | null;
  onSelect: (id: string) => void;
  layout: Layout;
  onLayout: (l: Layout) => void;
  offline: boolean;
}) {
  const area = useRef<HTMLDivElement>(null);
  const [tileW, setTileW] = useState(320);
  const [cols, rows] = gridOf(layout, cameras.length);
  const ids = useMemo(() => cameras.map((c) => c.id), [cameras]);

  useEffect(() => {
    // Only stream previews while the window is visible (spec §6.3).
    const sync = () => client.setPreviewSubscription(document.visibilityState === 'hidden' ? [] : ids, 'small');
    sync();
    document.addEventListener('visibilitychange', sync);
    return () => document.removeEventListener('visibilitychange', sync);
  }, [client, ids]);

  useLayoutEffect(() => {
    const el = area.current;
    if (!el || typeof ResizeObserver === 'undefined') return;
    const fit = () => {
      const W = el.clientWidth - 8;
      const H = el.clientHeight - 8;
      setTileW(Math.max(160, Math.floor(Math.min((W - GAP * (cols - 1)) / cols, ((H - GAP * (rows - 1)) / rows - STRIP) * (16 / 9)))));
    };
    fit();
    const ro = new ResizeObserver(fit);
    ro.observe(el);
    return () => ro.disconnect();
  }, [cols, rows]);

  function onKey(e: KeyboardEvent) {
    const i = Math.max(0, ids.indexOf(selected ?? ''));
    const step = { ArrowRight: 1, ArrowLeft: -1, ArrowDown: cols, ArrowUp: -cols }[e.key];
    if (step === undefined || ids.length === 0) return;
    e.preventDefault();
    onSelect(ids[Math.min(ids.length - 1, Math.max(0, i + step))]);
  }

  const warnings = Object.values(state.engine?.warnings ?? {}).filter((w) => w);
  return (
    <section className="center" aria-label="Multiview area">
      <div className="subbar">
        <b>Multiview</b>
        <span>{cameras.length} cameras</span>
        <span className="right">
          Layout
          <span className="seg" role="radiogroup" aria-label="Layout">
            {(['auto', '2x2', '3x2', '4x2'] as Layout[]).map((l) => (
              <button key={l} role="radio" aria-checked={layout === l} className={layout === l ? 'on' : ''} onClick={() => onLayout(l)}>
                {l === 'auto' ? 'Auto' : l.replace('x', '×')}
              </button>
            ))}
          </span>
        </span>
      </div>
      <div className="wall-wrap">
        {offline && (
          <ErrorBlock
            severity="warning"
            label="Engine restarting"
            title="Engine restarting."
            message="The connection to the engine dropped; the shell restarts it with this show. Outputs come back in about 3 seconds. The pictures are the last frames received."
          />
        )}
        {warnings.map((w) => (
          <ErrorBlock key={w} severity="warning" title="Warning." message={w} />
        ))}
        {cameras.length === 0 ? (
          <div className="empty">
            <div>
              <div className="frame">
                <Icon name="cam" />
              </div>
              <span className="h">No cameras yet</span>
              <span>Use Add camera in the toolbar (Ctrl+Shift+N). Each camera can send to several outputs.</span>
            </div>
          </div>
        ) : (
          <div className="wall-area" ref={area}>
            <div className="wall" role="listbox" aria-label="Multiview" tabIndex={0} onKeyDown={onKey} style={{ gridTemplateColumns: `repeat(${cols}, ${tileW}px)` }}>
              {cameras.map((cfg, i) => (
                <div
                  key={cfg.id}
                  role="option"
                  aria-selected={selected === cfg.id}
                  aria-label={`CAM ${i + 1} ${cfg.name}`}
                  className={`tile${selected === cfg.id ? ' sel' : ''}`}
                  onClick={() => onSelect(cfg.id)}
                >
                  <div className="pic">
                    <PreviewCanvas client={client} cameraId={cfg.id} />
                  </div>
                  <Strip n={i + 1} cfg={cfg} cam={state.cameras?.[cfg.id]} marks={marks[cfg.id]} holdMs={holdMs} now={now} dense={cameras.length > 4} offline={offline} />
                </div>
              ))}
            </div>
          </div>
        )}
      </div>
    </section>
  );
}
