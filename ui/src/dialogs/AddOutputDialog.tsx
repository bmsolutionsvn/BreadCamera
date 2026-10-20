import { useEffect, useState } from 'react';

import type { EngineClient } from '../api/client';
import { useEngine } from '../api/useEngine';
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

type Kind = 'ndi' | 'omt' | 'srt' | 'fullscreen' | 'decklink' | 'aja' | 'vcam';

const FIXED_FORMAT_NOTE = 'SDI uses 1080p at the camera rate where the card supports it; the virtual camera is 1080p30.';

// Output format per spec §4.3: one frame size per camera (the show size); only the rate is a
// per-output choice (same or half). M4b adds the SDI and virtual-camera kinds, whose format and
// rate are fixed by the hardware and not offered here.
export function AddOutputDialog({ client, camera, n, onClose }: { client: EngineClient; camera: { id: string; name: string }; n: number; onClose: () => void }) {
  const [kind, setKind] = useState<Kind>('ndi');
  const [ndiName, setNdiName] = useState(`BreadCamera - ${camera.name}`);
  const [omtMode, setOmtMode] = useState<'hx' | 'full'>('hx');
  const [omtCodec, setOmtCodec] = useState<'hevc' | 'h264'>('hevc');
  const [omtName, setOmtName] = useState(`BreadCamera - ${camera.name}`);
  const [srtMode, setSrtMode] = useState<'caller' | 'listener'>('caller');
  const [srtHost, setSrtHost] = useState('');
  const [srtPort, setSrtPort] = useState('9000');
  const [srtLatency, setSrtLatency] = useState('120');
  const [srtPassphrase, setSrtPassphrase] = useState('');
  const [bitrate, setBitrate] = useState('8000');
  const [monitors, setMonitors] = useState<Monitor[]>([]);
  const [monitor, setMonitor] = useState(0);
  const [decklinkDevices, setDecklinkDevices] = useState<DeckLinkDevice[]>([]);
  const [decklinkDevice, setDecklinkDevice] = useState('');
  const [decklinkKeyed, setDecklinkKeyed] = useState(false);
  const [ajaDevices, setAjaDevices] = useState<AjaDevice[]>([]);
  const [ajaDevice, setAjaDevice] = useState('');
  const [ajaChannel, setAjaChannel] = useState(1);
  const [vcamIndex, setVcamIndex] = useState(1);
  const [rate, setRate] = useState<'same' | 'half'>('same');
  const [serverError, setServerError] = useState('');

  const snap = useEngine(client);
  const usedVcam = new Set(
    (snap.show?.cameras ?? []).flatMap((c) => c.outputs.filter((o) => o.kind === 'vcam').map((o) => Number(o.params.index))),
  );
  const firstFreeVcam = [1, 2, 3, 4].find((x) => !usedVcam.has(x)) ?? 1;

  useEffect(() => {
    client.request<Monitor[]>('listMonitors').then(setMonitors).catch(() => setMonitors([]));
  }, [client]);
  useEffect(() => {
    if (kind === 'decklink') client.request<DeckLinkDevice[]>('listDeckLinkDevices').then(setDecklinkDevices).catch(() => setDecklinkDevices([]));
    if (kind === 'aja') client.request<AjaDevice[]>('listAjaDevices').then(setAjaDevices).catch(() => setAjaDevices([]));
  }, [client, kind]);

  const srtPortNum = Number(srtPort);
  const srtErrors = {
    port: kind === 'srt' && (!Number.isInteger(srtPortNum) || srtPortNum < 1 || srtPortNum > 65535) ? 'Port is a number from 1 to 65535' : '',
    host: kind === 'srt' && srtMode === 'caller' && !srtHost ? 'Caller mode needs a host' : '',
    passphrase: kind === 'srt' && srtPassphrase && (srtPassphrase.length < 10 || srtPassphrase.length > 79) ? 'Passphrase is empty or 10 to 79 characters' : '',
  };
  const errors = {
    ...srtErrors,
    decklink: kind === 'decklink' && decklinkDevice === '' ? 'Choose a card' : '',
    aja: kind === 'aja' && ajaDevice === '' ? 'Choose a card and a channel' : '',
  };
  const valid = (kind !== 'ndi' || !!ndiName.trim()) && Object.values(errors).every((e) => !e);

  const decklinkCard = decklinkDevices.find((d) => String(d.index) === decklinkDevice);
  const externalKeying = !!decklinkCard?.externalKeying;
  const fixed = kind === 'fullscreen' || kind === 'decklink' || kind === 'aja' || kind === 'vcam';

  function chooseKind(k: Kind) {
    setKind(k);
    // The virtual camera defaults to the lowest number no output in the show uses yet.
    if (k === 'vcam') setVcamIndex(firstFreeVcam);
  }

  async function submit() {
    let params: Record<string, unknown>;
    if (kind === 'ndi') params = { name: ndiName };
    else if (kind === 'omt') params = omtMode === 'hx' ? { mode: omtMode, codec: omtCodec, name: omtName } : { mode: omtMode, name: omtName };
    else if (kind === 'srt') {
      params = { mode: srtMode, port: srtPortNum, latencyMs: Number(srtLatency) || 120, bitrateKbps: Number(bitrate) || 8000 };
      if (srtMode === 'caller' && srtHost) params.host = srtHost;
      if (srtPassphrase) params.passphrase = srtPassphrase;
    } else if (kind === 'decklink') params = { device: Number(decklinkDevice) };
    else if (kind === 'aja') params = { device: Number(ajaDevice), channel: ajaChannel };
    else if (kind === 'vcam') params = { index: vcamIndex };
    else params = { monitor };
    const keyed = kind === 'decklink' ? decklinkKeyed : false;
    try {
      await client.request('addOutput', { cameraId: camera.id, output: { kind, keyed, params, format: { halfRate: rate === 'half' } } });
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
          <select id={id} className="ctl" value={kind} onChange={(e) => chooseKind(e.target.value as Kind)}>
            <option value="ndi">NDI</option>
            <option value="omt">OMT (Open Media Transport)</option>
            <option value="srt">SRT (H.264 over SRT)</option>
            <option value="fullscreen">Fullscreen on a monitor</option>
            <option value="decklink">DeckLink SDI</option>
            <option value="aja">AJA SDI (experimental)</option>
            <option value="vcam">Windows virtual camera</option>
          </select>
        )}
      </Prop>
      {kind === 'ndi' && <Prop label="NDI source name">{(id) => <input id={id} className="ctl" value={ndiName} onChange={(e) => setNdiName(e.target.value)} />}</Prop>}
      {kind === 'omt' && (
        <>
          <Prop label="Mode">
            {(id) => (
              <select id={id} className="ctl" value={omtMode} onChange={(e) => setOmtMode(e.target.value as 'hx' | 'full')}>
                <option value="hx">OMT-HX (HEVC or H.264)</option>
                <option value="full">OMT (plain, VMX)</option>
              </select>
            )}
          </Prop>
          {omtMode === 'hx' && (
            <Prop label="Codec">
              {(id) => (
                <select id={id} className="ctl" value={omtCodec} onChange={(e) => setOmtCodec(e.target.value as 'hevc' | 'h264')}>
                  <option value="hevc">HEVC (H.265)</option>
                  <option value="h264">H.264</option>
                </select>
              )}
            </Prop>
          )}
          <Prop label="Name">
            {(id) => <input id={id} className="ctl" value={omtName} onChange={(e) => setOmtName(e.target.value)} />}
          </Prop>
        </>
      )}
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
      {kind === 'decklink' && (
        <>
          <Prop label="Card" hint={errors.decklink}>
            {(id) => (
              <div className="row-btns">
                <select id={id} className="ctl" value={decklinkDevice} onChange={(e) => setDecklinkDevice(e.target.value)}>
                  <option value="">Choose a card…</option>
                  {decklinkDevices
                    .filter((d) => d.hasOutput)
                    .map((d) => (
                      <option key={d.index} value={String(d.index)}>
                        {d.index + 1}. {d.name}
                      </option>
                    ))}
                </select>
                <button className="btn" onClick={() => client.request<DeckLinkDevice[]>('listDeckLinkDevices').then(setDecklinkDevices).catch(() => setDecklinkDevices([]))}>
                  Refresh
                </button>
              </div>
            )}
          </Prop>
          <Prop label="Key + fill out (2 SDI ports)" hint={externalKeying ? undefined : 'This card has no external keyer'}>
            {(id) => <input id={id} type="checkbox" checked={decklinkKeyed} disabled={!externalKeying} onChange={(e) => setDecklinkKeyed(e.target.checked)} />}
          </Prop>
        </>
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
                <button className="btn" onClick={() => client.request<AjaDevice[]>('listAjaDevices').then(setAjaDevices).catch(() => setAjaDevices([]))}>
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
      {kind === 'vcam' && (
        <Prop label="Virtual camera number">
          {(id) => (
            <select id={id} className="ctl" value={vcamIndex} onChange={(e) => setVcamIndex(Number(e.target.value))}>
              {[1, 2, 3, 4].map((x) => (
                <option key={x} value={x}>
                  {x}
                </option>
              ))}
            </select>
          )}
        </Prop>
      )}
      {fixed ? (
        <div className="hint">{FIXED_FORMAT_NOTE}</div>
      ) : (
        <Prop label="Frame rate" hint="Half sends every second frame. Other rates are not offered because converting them judders.">
          {(id) => (
            <select id={id} className="ctl" value={rate} onChange={(e) => setRate(e.target.value as 'same' | 'half')}>
              <option value="same">Same as show</option>
              <option value="half">Half rate</option>
            </select>
          )}
        </Prop>
      )}
    </Modal>
  );
}
