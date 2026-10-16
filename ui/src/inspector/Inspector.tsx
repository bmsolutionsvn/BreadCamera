import { useEffect, useState, type ReactNode } from 'react';

import type { EngineClient, EngineSnapshot } from '../api/client';
import type { CameraConfig } from '../api/protocol';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon } from '../components/Icon';
import { Prop } from '../components/Prop';
import { ConfirmDialog } from '../dialogs/ConfirmDialog';
import { inputCode, outputCode, outputName, signalLabel } from '../state/labels';

export function Section({ title, right, children }: { title: string; right?: ReactNode; children: ReactNode }) {
  return (
    <details className="sec" open>
      <summary>
        <span className="chev">
          <Icon name="chev" size={12} />
        </span>
        {title}
        <span className="right" onClick={(e) => e.preventDefault()}>
          {right}
        </span>
      </summary>
      <div className="sec-b">{children}</div>
    </details>
  );
}

function formatLabel(o: CameraConfig['outputs'][number]) {
  const f = o.format;
  const size = f && f.width ? `${f.width}×${f.height}` : 'same as show';
  return `${o.keyed ? 'Keyed' : 'Composited'} · ${size}${f?.halfRate ? ' · half rate' : ''}`;
}

// The selected camera: Input, (M2: Effects via children), Outputs (spec §6.4).
export function Inspector({ client, snap, cam, n, onAddOutput, onRemoved, children }: {
  client: EngineClient;
  snap: EngineSnapshot;
  cam: CameraConfig | undefined;
  n: number;
  onAddOutput: () => void;
  onRemoved: () => void;
  children?: ReactNode;
}) {
  const [confirm, setConfirm] = useState<null | { kind: 'camera' } | { kind: 'output'; id: string }>(null);
  const p = cam?.input.params ?? {};
  const kind = cam?.input.kind ?? 'srt';
  const [draft, setDraft] = useState({ mode: '', url: '', host: '', port: '', latency: '', passphrase: '' });
  const [ndiSources, setNdiSources] = useState<string[]>([]);
  const [ndiSource, setNdiSource] = useState('');
  const [devices, setDevices] = useState<{ name: string; id: string }[]>([]);
  const [device, setDevice] = useState('');
  const [error, setError] = useState('');
  const refreshNdi = () => client.request<string[]>('listNdiSources').then(setNdiSources).catch(() => setNdiSources([]));
  const refreshDevices = () =>
    client.request<{ name: string; id: string }[]>('listCaptureDevices').then(setDevices).catch(() => setDevices([]));
  useEffect(() => {
    setDraft({
      mode: String(p.mode ?? 'caller'),
      url: String(p.url ?? ''),
      host: String(p.host ?? ''),
      port: p.port === undefined ? '' : String(p.port),
      latency: String(p.latencyMs ?? 120),
      passphrase: '',
    });
    setNdiSource(String(p.source ?? ''));
    setDevice(String(p.device ?? ''));
    setError('');
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [cam?.id, JSON.stringify(p)]);
  useEffect(() => {
    if (kind === 'ndi') refreshNdi();
    else if (kind === 'uvc') refreshDevices();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [cam?.id, kind]);

  if (!cam) {
    return (
      <aside className="insp" aria-label="Inspector">
        <div className="insp-h">
          <span className="k">INSPECTOR</span>
          <div className="t dim">No camera selected</div>
        </div>
        <p className="note pad">Select a camera in Multiview to edit its input and outputs.</p>
      </aside>
    );
  }
  const cs = snap.state.cameras?.[cam.id];
  const stats = snap.stats?.cameras?.[cam.id];
  const health = cs?.input?.health ?? 'offline';
  const dirty =
    kind === 'ndi'
      ? ndiSource !== String(p.source ?? '')
      : kind === 'uvc'
        ? device !== String(p.device ?? '')
        : kind === 'srt' &&
          (draft.mode !== String(p.mode ?? 'caller') ||
            draft.url !== String(p.url ?? '') ||
            draft.host !== String(p.host ?? '') ||
            draft.port !== (p.port === undefined ? '' : String(p.port)) ||
            draft.latency !== String(p.latencyMs ?? 120) ||
            draft.passphrase !== '');

  async function apply() {
    let params: Record<string, unknown>;
    if (kind === 'ndi') params = { source: ndiSource };
    else if (kind === 'uvc') params = { device };
    else {
      params = { mode: draft.mode, latencyMs: Number(draft.latency) || 120 };
      if (draft.url) params.url = draft.url;
      else {
        params.port = Number(draft.port);
        if (draft.host) params.host = draft.host;
      }
      params.passphrase = draft.passphrase ? draft.passphrase : { set: (p.passphrase as { set?: boolean })?.set ?? false };
      if (p.streamId) params.streamId = p.streamId;
    }
    try {
      await client.request('updateCamera', { id: cam!.id, input: { kind, params } });
      setError('');
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  return (
    <aside className="insp" aria-label="Inspector">
      <div className="insp-h">
        <span className="k">INSPECTOR · CAM {n}</span>
        <div className="t">
          <span className={`in-chip ${health === 'receiving' ? 'ready' : health === 'degraded' ? 'conn' : health === 'down' ? 'err' : 'off'}`}>{inputCode(cam.input.kind)}</span>
          <span className="n">{cam.name}</span>
        </div>
      </div>
      <div className="insp-b">
        <Section title="Input" right={<span className="ro">{health}</span>}>
          {cs?.input?.error && <ErrorBlock error={cs.input.error} source="input" />}
          {cs?.tickError && <ErrorBlock title="Processing failed." message={cs.tickError} />}
          {error && <ErrorBlock message={error} />}
          {kind === 'srt' && (
            <>
              <Prop label="Mode">
                {(id) => (
                  <select id={id} className="ctl" value={draft.mode} onChange={(e) => setDraft({ ...draft, mode: e.target.value })}>
                    <option value="caller">Caller (BreadCamera dials out)</option>
                    <option value="listener">Listener (waits for the source)</option>
                  </select>
                )}
              </Prop>
              {draft.mode === 'caller' && (
                <Prop label="URL">{(id) => <input id={id} className="ctl mono" value={draft.url} placeholder="srt://host:port" onChange={(e) => setDraft({ ...draft, url: e.target.value })} />}</Prop>
              )}
              {!draft.url && (
                <>
                  {draft.mode === 'caller' && <Prop label="Host">{(id) => <input id={id} className="ctl mono" value={draft.host} onChange={(e) => setDraft({ ...draft, host: e.target.value })} />}</Prop>}
                  <Prop label="Port">{(id) => <input id={id} className="ctl mono" inputMode="numeric" value={draft.port} onChange={(e) => setDraft({ ...draft, port: e.target.value })} />}</Prop>
                </>
              )}
              <Prop label="Latency (ms)">{(id) => <input id={id} className="ctl mono" type="number" min={20} max={8000} value={draft.latency} onChange={(e) => setDraft({ ...draft, latency: e.target.value })} />}</Prop>
              <Prop label="Passphrase" hint={(p.passphrase as { set?: boolean })?.set ? 'Set. Type a new one to replace it.' : 'Optional, stored encrypted.'}>
                {(id) => <input id={id} className="ctl" type="password" value={draft.passphrase} onChange={(e) => setDraft({ ...draft, passphrase: e.target.value })} />}
              </Prop>
            </>
          )}
          {kind === 'ndi' && (
            <Prop label="NDI source">
              {(id) => (
                <div className="row-btns">
                  <select id={id} className="ctl" value={ndiSource} onChange={(e) => setNdiSource(e.target.value)}>
                    <option value="">Choose a source…</option>
                    {ndiSources.map((s) => (
                      <option key={s} value={s}>
                        {s}
                      </option>
                    ))}
                  </select>
                  <button className="btn" onClick={refreshNdi}>
                    Refresh
                  </button>
                </div>
              )}
            </Prop>
          )}
          {kind === 'uvc' && (
            <Prop label="Device" hint="Device cameras are video only, no audio.">
              {(id) => (
                <div className="row-btns">
                  <select id={id} className="ctl" value={device} onChange={(e) => setDevice(e.target.value)}>
                    <option value="">Choose a device…</option>
                    {devices.map((d) => (
                      <option key={d.id} value={d.id}>
                        {d.name}
                      </option>
                    ))}
                  </select>
                  <button className="btn" onClick={refreshDevices}>
                    Refresh
                  </button>
                </div>
              )}
            </Prop>
          )}
          {kind !== 'srt' && kind !== 'ndi' && kind !== 'uvc' && <p className="note">This input kind is not editable here.</p>}
          {stats && (
            <Prop label="Receiving">
              {() => (
                <span className="ro">
                  {stats.fps.toFixed(0)} fps · {stats.drops} dropped · {stats.latencyMs.toFixed(0)} ms
                </span>
              )}
            </Prop>
          )}
          <div className="row-end">
            <button className="btn" disabled={!dirty} onClick={apply}>
              Apply input changes
            </button>
          </div>
        </Section>
        {children}
        <Section
          title="Outputs"
          right={
            <button className="tb sm" onClick={onAddOutput}>
              <Icon name="plus" size={12} />
              Add output
            </button>
          }
        >
          {cam.outputs.map((o) => cs?.outputs?.[o.id]?.error && <ErrorBlock key={`e-${o.id}`} error={cs!.outputs![o.id].error!} />)}
          {cam.outputs.length === 0 && <p className="note">No outputs yet. Add one to send this camera on.</p>}
          {cam.outputs.map((o) => {
            const os = cs?.outputs?.[o.id];
            const st = os?.state ?? 'ready';
            const dest = o.kind === 'ndi' ? String(o.params.name ?? `BreadCamera - ${cam.name}`) : o.kind === 'fullscreen' ? `Monitor ${Number(o.params.monitor ?? 0) + 1}` : outputName(o.kind);
            return (
              <div className="orow" key={o.id}>
                {os?.error ? (
                  <span className="err-chip">
                    <Icon name="retry" size={11} />
                    {outputCode(o.kind)}
                  </span>
                ) : (
                  <span className={`pill ${st}`} aria-label={`${outputName(o.kind)}: ${signalLabel[st]}`}>
                    <i />
                    {outputCode(o.kind)}
                  </span>
                )}
                <div className="o-main">
                  <div className="n">{dest}</div>
                  <div className="m">{formatLabel(o)}</div>
                </div>
                <label className="sw-wrap" title={o.enabled ? 'Sending. Switch off to stop this output.' : 'Stopped. Switch on to send.'}>
                  <input type="checkbox" role="switch" aria-label={`${outputName(o.kind)} on`} checked={o.enabled} onChange={(e) => client.request('updateOutput', { cameraId: cam.id, id: o.id, enabled: e.target.checked }).catch(() => {})} />
                  <span className="sw" />
                </label>
                <button className="ibtn" aria-label={`Remove ${outputName(o.kind)}`} onClick={() => setConfirm({ kind: 'output', id: o.id })}>
                  <Icon name="trash" size={14} />
                </button>
              </div>
            );
          })}
        </Section>
        <div className="pad">
          <button className="btn ghost-danger" onClick={() => setConfirm({ kind: 'camera' })}>
            <Icon name="trash" size={14} />
            Remove camera…
          </button>
        </div>
      </div>
      {confirm?.kind === 'camera' && (
        <ConfirmDialog
          title="Remove camera?"
          body={`CAM ${n} "${cam.name}" and all its outputs stop now. Their receivers lose picture.`}
          confirm="Remove camera"
          onCancel={() => setConfirm(null)}
          onConfirm={async () => {
            await client.request('removeCamera', { id: cam.id }).catch(() => {});
            setConfirm(null);
            onRemoved();
          }}
        />
      )}
      {confirm?.kind === 'output' && (
        <ConfirmDialog
          title="Remove output?"
          body="Its receiver loses picture now."
          confirm="Remove output"
          onCancel={() => setConfirm(null)}
          onConfirm={async () => {
            await client.request('removeOutput', { cameraId: cam.id, id: confirm.id }).catch(() => {});
            setConfirm(null);
          }}
        />
      )}
    </aside>
  );
}
