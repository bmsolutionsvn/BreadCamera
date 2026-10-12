import { useEffect, useState } from 'react';

import type { EngineClient } from '../api/client';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon } from '../components/Icon';
import { Modal } from '../components/Modal';
import { Prop } from '../components/Prop';

interface Monitor {
  index: number;
  name: string;
  width: number;
  height: number;
  primary: boolean;
}

const SIZES: Record<string, [number, number]> = { same: [0, 0], '1280x720': [1280, 720], '960x540': [960, 540] };

// Output format per spec §4.3: same as show or smaller; same rate or half. M4 adds kinds.
export function AddOutputDialog({ client, camera, n, onClose }: { client: EngineClient; camera: { id: string; name: string }; n: number; onClose: () => void }) {
  const [kind, setKind] = useState<'ndi' | 'srt' | 'fullscreen'>('ndi');
  const [ndiName, setNdiName] = useState(`BreadCamera - ${camera.name}`);
  const [srtMode, setSrtMode] = useState<'caller' | 'listener'>('caller');
  const [srtHost, setSrtHost] = useState('');
  const [srtPort, setSrtPort] = useState('9000');
  const [srtLatency, setSrtLatency] = useState('120');
  const [srtPassphrase, setSrtPassphrase] = useState('');
  const [bitrate, setBitrate] = useState('8000');
  const [monitors, setMonitors] = useState<Monitor[]>([]);
  const [monitor, setMonitor] = useState(0);
  const [size, setSize] = useState('same');
  const [rate, setRate] = useState<'same' | 'half'>('same');
  const [serverError, setServerError] = useState('');

  useEffect(() => {
    client.request<Monitor[]>('listMonitors').then(setMonitors).catch(() => setMonitors([]));
  }, [client]);

  const srtPortNum = Number(srtPort);
  const srtErrors = {
    port: kind === 'srt' && (!Number.isInteger(srtPortNum) || srtPortNum < 1 || srtPortNum > 65535) ? 'Port is a number from 1 to 65535' : '',
    host: kind === 'srt' && srtMode === 'caller' && !srtHost ? 'Caller mode needs a host' : '',
    passphrase: kind === 'srt' && srtPassphrase && (srtPassphrase.length < 10 || srtPassphrase.length > 79) ? 'Passphrase is empty or 10 to 79 characters' : '',
  };
  const valid = (kind !== 'ndi' || !!ndiName.trim()) && Object.values(srtErrors).every((e) => !e);

  async function submit() {
    let params: Record<string, unknown>;
    if (kind === 'ndi') params = { name: ndiName };
    else if (kind === 'srt') {
      params = { mode: srtMode, port: srtPortNum, latencyMs: Number(srtLatency) || 120, bitrateKbps: Number(bitrate) || 8000 };
      if (srtMode === 'caller' && srtHost) params.host = srtHost;
      if (srtPassphrase) params.passphrase = srtPassphrase;
    } else params = { monitor };
    const [width, height] = kind === 'fullscreen' ? [0, 0] : SIZES[size];
    try {
      await client.request('addOutput', { cameraId: camera.id, output: { kind, params, format: { width, height, halfRate: rate === 'half' } } });
      onClose();
    } catch (e) {
      setServerError(e instanceof Error ? e.message : String(e));
    }
  }

  return (
    <Modal
      title={`Add output to CAM ${n}`}
      onClose={onClose}
      footer={
        <>
          <button className="btn" onClick={onClose}>
            Cancel
          </button>
          <button className="btn primary" disabled={!valid} onClick={submit}>
            <Icon name="plus" size={14} />
            Add output
          </button>
        </>
      }
    >
      {serverError && <ErrorBlock message={serverError} />}
      <Prop label="Output type">
        {(id) => (
          <select id={id} className="ctl" value={kind} onChange={(e) => setKind(e.target.value as 'ndi' | 'srt' | 'fullscreen')}>
            <option value="ndi">NDI</option>
            <option value="srt">SRT (H.264 over SRT)</option>
            <option value="fullscreen">Fullscreen on a monitor</option>
          </select>
        )}
      </Prop>
      {kind === 'ndi' && <Prop label="NDI source name">{(id) => <input id={id} className="ctl" value={ndiName} onChange={(e) => setNdiName(e.target.value)} />}</Prop>}
      {kind === 'srt' && (
        <>
          <Prop label="Mode">
            {(id) => (
              <select id={id} className="ctl" value={srtMode} onChange={(e) => setSrtMode(e.target.value as 'caller' | 'listener')}>
                <option value="caller">Caller (BreadCamera dials in)</option>
                <option value="listener">Listener (waits for the receiver)</option>
              </select>
            )}
          </Prop>
          {srtMode === 'caller' && (
            <Prop label="Host" hint={srtErrors.host}>
              {(id) => <input id={id} className="ctl mono" value={srtHost} onChange={(e) => setSrtHost(e.target.value)} />}
            </Prop>
          )}
          <Prop label="Port" hint={srtErrors.port}>
            {(id) => <input id={id} className="ctl mono" inputMode="numeric" value={srtPort} onChange={(e) => setSrtPort(e.target.value)} />}
          </Prop>
          <Prop label="Latency (ms)">
            {(id) => <input id={id} className="ctl mono" type="number" value={srtLatency} onChange={(e) => setSrtLatency(e.target.value)} />}
          </Prop>
          <Prop label="Passphrase" hint={srtErrors.passphrase || 'Optional, stored encrypted.'}>
            {(id) => <input id={id} className="ctl" type="password" value={srtPassphrase} onChange={(e) => setSrtPassphrase(e.target.value)} />}
          </Prop>
          <Prop label="Bitrate (kb/s)">
            {(id) => <input id={id} className="ctl mono" inputMode="numeric" value={bitrate} onChange={(e) => setBitrate(e.target.value)} />}
          </Prop>
        </>
      )}
      {kind === 'fullscreen' && (
        <Prop label="Monitor">
          {(id) => (
            <select id={id} className="ctl" value={monitor} onChange={(e) => setMonitor(Number(e.target.value))}>
              {monitors.map((m) => (
                <option key={m.index} value={m.index}>
                  {m.index + 1}. {m.name} ({m.width}×{m.height})
                </option>
              ))}
            </select>
          )}
        </Prop>
      )}
      {kind !== 'fullscreen' && (
        <Prop label="Resolution">
          {(id) => (
            <select id={id} className="ctl" value={size} onChange={(e) => setSize(e.target.value)}>
              <option value="same">Same as show</option>
              <option value="1280x720">1280×720</option>
              <option value="960x540">960×540</option>
            </select>
          )}
        </Prop>
      )}
      <Prop label="Frame rate" hint="Half sends every second frame. Other rates are not offered because converting them judders.">
        {(id) => (
          <select id={id} className="ctl" value={rate} onChange={(e) => setRate(e.target.value as 'same' | 'half')}>
            <option value="same">Same as show</option>
            <option value="half">Half rate</option>
          </select>
        )}
      </Prop>
    </Modal>
  );
}
