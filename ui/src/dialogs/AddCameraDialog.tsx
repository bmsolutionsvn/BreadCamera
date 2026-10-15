import { useEffect, useState } from 'react';

import type { EngineClient } from '../api/client';
import { useEngine } from '../api/useEngine';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon } from '../components/Icon';
import { Modal } from '../components/Modal';
import { Prop } from '../components/Prop';

const chars = (s: string) => [...s].length;

type Kind = 'srt' | 'ndi' | 'uvc' | 'decklink' | 'aja' | 'breadcall';

interface DeckLinkDevice {
  index: number;
  name: string;
  hasInput: boolean;
  hasOutput: boolean;
  externalKeying: boolean;
}

interface AjaDevice {
  index: number;
  name: string;
}

export function AddCameraDialog({ client, onClose }: { client: EngineClient; onClose: () => void }) {
  const [name, setName] = useState('');
  const [kind, setKind] = useState<Kind>('srt');
  const [mode, setMode] = useState<'caller' | 'listener'>('caller');
  const [url, setUrl] = useState('');
  const [host, setHost] = useState('');
  const [port, setPort] = useState('9000');
  const [latency, setLatency] = useState('120');
  const [passphrase, setPassphrase] = useState('');
  const [streamId, setStreamId] = useState('');
  const [ndiSources, setNdiSources] = useState<string[]>([]);
  const [ndiSource, setNdiSource] = useState('');
  const [devices, setDevices] = useState<{ name: string; id: string }[]>([]);
  const [device, setDevice] = useState('');
  const [decklinkDevices, setDecklinkDevices] = useState<DeckLinkDevice[]>([]);
  const [decklinkDevice, setDecklinkDevice] = useState('');
  const [ajaDevices, setAjaDevices] = useState<AjaDevice[]>([]);
  const [ajaDevice, setAjaDevice] = useState('');
  const [ajaChannel, setAjaChannel] = useState(1);
  const [roomUrl, setRoomUrl] = useState('');
  const [token, setToken] = useState('');
  const [slot, setSlot] = useState(1);
  const [serverError, setServerError] = useState('');
  const snap = useEngine(client);
  const usedBy = new Map<string, string>();
  for (const c of snap.show?.cameras ?? [])
    if (c.input.kind === 'uvc' && c.input.params.device) usedBy.set(String(c.input.params.device), c.name);

  const refreshNdi = () => client.request<string[]>('listNdiSources').then(setNdiSources).catch(() => setNdiSources([]));
  const refreshDevices = () =>
    client.request<{ name: string; id: string }[]>('listCaptureDevices').then(setDevices).catch(() => setDevices([]));
  const refreshDeckLink = () =>
    client.request<DeckLinkDevice[]>('listDeckLinkDevices').then(setDecklinkDevices).catch(() => setDecklinkDevices([]));
  const refreshAja = () => client.request<AjaDevice[]>('listAjaDevices').then(setAjaDevices).catch(() => setAjaDevices([]));
  useEffect(() => {
    if (kind === 'ndi') refreshNdi();
    if (kind === 'uvc') refreshDevices();
    if (kind === 'decklink') refreshDeckLink();
    if (kind === 'aja') refreshAja();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [kind]);

  const portNum = Number(port);
  const errors = {
    name: chars(name.trim()) < 1 || chars(name) > 80 ? 'Name must be 1 to 80 characters' : '',
    port: kind === 'srt' && !url && (!Number.isInteger(portNum) || portNum < 1 || portNum > 65535) ? 'Port is a number from 1 to 65535' : '',
    passphrase: kind === 'srt' && passphrase && (passphrase.length < 10 || passphrase.length > 79) ? 'Passphrase is empty or 10 to 79 characters' : '',
    host: kind === 'srt' && mode === 'caller' && !url && !host ? 'Caller mode needs a host or a URL' : '',
    ndi: kind === 'ndi' && !ndiSource ? 'Choose an NDI source' : '',
    device: kind === 'uvc' && !device ? 'Choose a device' : '',
    decklinkDevice: kind === 'decklink' && !decklinkDevice ? 'Choose a card' : '',
    aja: kind === 'aja' && !ajaDevice ? 'Choose a card' : '',
    breadcallUrl: kind === 'breadcall' && (roomUrl.trim().length < 1 || roomUrl.length > 512) ? 'Room URL is 1 to 512 characters' : '',
    breadcallToken: kind === 'breadcall' && (token.trim().length < 1 || token.length > 2048) ? 'Token is 1 to 2048 characters' : '',
  };
  const valid = Object.values(errors).every((e) => !e);

  function srtParams() {
    const params: Record<string, unknown> = { mode, latencyMs: Number(latency) || 120 };
    if (url) params.url = url;
    else {
      params.port = portNum;
      if (host) params.host = host;
    }
    if (passphrase) params.passphrase = passphrase;
    if (streamId) params.streamId = streamId;
    return params;
  }

  async function submit() {
    const input =
      kind === 'ndi'
        ? { kind: 'ndi', params: { source: ndiSource } }
        : kind === 'uvc'
          ? { kind: 'uvc', params: { device } }
          : kind === 'decklink'
            ? { kind: 'decklink', params: { device: Number(decklinkDevice) } }
            : kind === 'aja'
              ? { kind: 'aja', params: { device: Number(ajaDevice), channel: ajaChannel } }
              : kind === 'breadcall'
                ? { kind: 'breadcall', params: { url: roomUrl.trim(), token: token.trim(), slot } }
                : { kind: 'srt', params: srtParams() };
    try {
      await client.request('addCamera', { name: name.trim(), input });
      onClose();
    } catch (e) {
      setServerError(e instanceof Error ? e.message : String(e));
    }
  }

  return (
    <Modal
      title="Add camera"
      onClose={onClose}
      footer={
        <>
          <button className="btn" onClick={onClose}>
            Cancel
          </button>
          <button className="btn primary" disabled={!valid} onClick={submit}>
            <Icon name="plus" size={14} />
            Add camera
          </button>
        </>
      }
    >
      {serverError && <ErrorBlock message={serverError} />}
      <Prop label="Name" hint={name && errors.name ? errors.name : 'Any language works.'}>
        {(id) => <input id={id} className="ctl" autoFocus value={name} onChange={(e) => setName(e.target.value)} aria-invalid={!!name && !!errors.name} />}
      </Prop>
      <Prop label="Input type">
        {(id) => (
          <select id={id} className="ctl" value={kind} onChange={(e) => setKind(e.target.value as Kind)}>
            <option value="srt">SRT (BreadCall feeds, encoders)</option>
            <option value="ndi">NDI</option>
            <option value="uvc">USB or built-in camera</option>
            <option value="decklink">DeckLink (SDI/HDMI)</option>
            <option value="aja">AJA (experimental)</option>
            <option value="breadcall">BreadCall guest</option>
          </select>
        )}
      </Prop>
      {kind === 'ndi' && (
        <Prop label="NDI source" hint={errors.ndi}>
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
        <Prop label="Device" hint={errors.device || 'USB and built-in cameras are video only, no audio.'}>
          {(id) => (
            <div className="row-btns">
              <select id={id} className="ctl" value={device} onChange={(e) => setDevice(e.target.value)}>
                <option value="">Choose a device…</option>
                {devices.map((d) => (
                  <option key={d.id} value={d.id} disabled={usedBy.has(d.id)}>
                    {usedBy.has(d.id) ? d.name + ' (Already used by ' + usedBy.get(d.id) + ')' : d.name}
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
      {kind === 'decklink' && (
        <Prop label="Card" hint={errors.decklinkDevice}>
          {(id) => (
            <div className="row-btns">
              <select id={id} className="ctl" value={decklinkDevice} onChange={(e) => setDecklinkDevice(e.target.value)}>
                <option value="">Choose a card…</option>
                {decklinkDevices
                  .filter((d) => d.hasInput)
                  .map((d) => (
                    <option key={d.index} value={String(d.index)}>
                      {d.index + 1}. {d.name}
                    </option>
                  ))}
              </select>
              <button className="btn" onClick={refreshDeckLink}>
                Refresh
              </button>
            </div>
          )}
        </Prop>
      )}
      {kind === 'aja' && (
        <>
          <Prop label="Card" hint={errors.aja}>
            {(id) => (
              <div className="row-btns">
                <select id={id} className="ctl" value={ajaDevice} onChange={(e) => setAjaDevice(e.target.value)}>
                  <option value="">Choose a card…</option>
                  {ajaDevices.map((d) => (
                    <option key={d.index} value={String(d.index)}>
                      {d.index + 1}. {d.name}
                    </option>
                  ))}
                </select>
                <button className="btn" onClick={refreshAja}>
                  Refresh
                </button>
              </div>
            )}
          </Prop>
          <Prop label="Channel">
            {(id) => (
              <select id={id} className="ctl" value={ajaChannel} onChange={(e) => setAjaChannel(Number(e.target.value))}>
                {[1, 2, 3, 4, 5, 6, 7, 8].map((c) => (
                  <option key={c} value={c}>
                    {c}
                  </option>
                ))}
              </select>
            )}
          </Prop>
          <ErrorBlock severity="warning" message="Experimental — not verified on hardware" />
        </>
      )}
      {kind === 'breadcall' && (
        <>
          <Prop label="Room URL" hint={errors.breadcallUrl || 'The LiveKit room URL from BreadCall.'}>
            {(id) => <input id={id} className="ctl mono" value={roomUrl} placeholder="wss://…" onChange={(e) => setRoomUrl(e.target.value)} />}
          </Prop>
          <Prop label="Token" hint={errors.breadcallToken || 'The receiver token BreadCall minted for this slot.'}>
            {(id) => <input id={id} className="ctl mono" value={token} onChange={(e) => setToken(e.target.value)} />}
          </Prop>
          <Prop label="Slot" hint="The guest slot this camera subscribes to.">
            {(id) => (
              <select id={id} className="ctl" value={slot} onChange={(e) => setSlot(Number(e.target.value))}>
                {[1, 2, 3, 4, 5, 6, 7, 8].map((s) => (
                  <option key={s} value={s}>
                    {s}
                  </option>
                ))}
              </select>
            )}
          </Prop>
        </>
      )}
      {kind === 'srt' && (
        <>
          <Prop label="Mode">
            {(id) => (
              <select id={id} className="ctl" value={mode} onChange={(e) => setMode(e.target.value as 'caller' | 'listener')}>
                <option value="caller">Caller (BreadCamera dials out)</option>
                <option value="listener">Listener (waits for the source)</option>
              </select>
            )}
          </Prop>
          {mode === 'caller' && <Prop label="URL" hint="Paste the feed URL from BreadCall.">{(id) => <input id={id} className="ctl mono" value={url} placeholder="srt://host:port" onChange={(e) => setUrl(e.target.value)} />}</Prop>}
          {!url && (
            <>
              {mode === 'caller' && <Prop label="Host" hint={errors.host}>{(id) => <input id={id} className="ctl mono" value={host} onChange={(e) => setHost(e.target.value)} />}</Prop>}
              <Prop label="Port" hint={errors.port}>{(id) => <input id={id} className="ctl mono" inputMode="numeric" value={port} onChange={(e) => setPort(e.target.value)} />}</Prop>
            </>
          )}
          <Prop label="Latency (ms)">{(id) => <input id={id} className="ctl mono" type="number" value={latency} onChange={(e) => setLatency(e.target.value)} />}</Prop>
          <Prop label="Passphrase" hint={errors.passphrase || 'Optional, stored encrypted.'}>{(id) => <input id={id} className="ctl" type="password" value={passphrase} onChange={(e) => setPassphrase(e.target.value)} />}</Prop>
          <Prop label="Stream ID">{(id) => <input id={id} className="ctl mono" value={streamId} onChange={(e) => setStreamId(e.target.value)} />}</Prop>
        </>
      )}
    </Modal>
  );
}
