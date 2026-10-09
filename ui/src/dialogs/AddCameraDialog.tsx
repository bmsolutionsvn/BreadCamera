import { useState } from 'react';

import type { EngineClient } from '../api/client';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon } from '../components/Icon';
import { Modal } from '../components/Modal';
import { Prop } from '../components/Prop';

const chars = (s: string) => [...s].length;

export function AddCameraDialog({ client, onClose }: { client: EngineClient; onClose: () => void }) {
  const [name, setName] = useState('');
  const [mode, setMode] = useState<'caller' | 'listener'>('caller');
  const [url, setUrl] = useState('');
  const [host, setHost] = useState('');
  const [port, setPort] = useState('9000');
  const [latency, setLatency] = useState('120');
  const [passphrase, setPassphrase] = useState('');
  const [streamId, setStreamId] = useState('');
  const [serverError, setServerError] = useState('');

  const portNum = Number(port);
  const errors = {
    name: chars(name.trim()) < 1 || chars(name) > 80 ? 'Name must be 1 to 80 characters' : '',
    port: !url && (!Number.isInteger(portNum) || portNum < 1 || portNum > 65535) ? 'Port is a number from 1 to 65535' : '',
    passphrase: passphrase && (passphrase.length < 10 || passphrase.length > 79) ? 'Passphrase is empty or 10 to 79 characters' : '',
    host: mode === 'caller' && !url && !host ? 'Caller mode needs a host or a URL' : '',
  };
  const valid = Object.values(errors).every((e) => !e);

  async function submit() {
    const params: Record<string, unknown> = { mode, latencyMs: Number(latency) || 120 };
    if (url) params.url = url;
    else {
      params.port = portNum;
      if (host) params.host = host;
    }
    if (passphrase) params.passphrase = passphrase;
    if (streamId) params.streamId = streamId;
    try {
      await client.request('addCamera', { name: name.trim(), input: { kind: 'srt', params } });
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
          <select id={id} className="ctl" value="srt" disabled>
            <option value="srt">SRT (BreadCall feeds, encoders)</option>
          </select>
        )}
      </Prop>
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
    </Modal>
  );
}
