import { useState } from 'react';

import type { EngineClient } from '../api/client';
import type { CameraConfig, StageSchemaJson } from '../api/protocol';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon, type IconName } from '../components/Icon';
import { Modal } from '../components/Modal';
import { isAdded } from '../inspector/Effects';

// Every registered effect appears here automatically (spec §5.4); a missing dependency is explained, not hidden.
export function AddEffectDialog({ client, camera, n, schemas, onClose }: { client: EngineClient; camera: CameraConfig; n: number; schemas: StageSchemaJson[]; onClose: () => void }) {
  const usable = (s: StageSchemaJson) => !isAdded(camera, s.kind) && s.requires.every((k) => isAdded(camera, k)) && !s.unavailable;
  const [pick, setPick] = useState(() => schemas.find(usable)?.kind ?? '');
  const [error, setError] = useState('');
  const labelOf = (kind: string) => schemas.find((s) => s.kind === kind)?.label ?? kind;

  async function submit() {
    const s = schemas.find((x) => x.kind === pick);
    if (!s) return;
    try {
      await client.request('setStage', { cameraId: camera.id, stage: s.kind, params: s.onAdd });
      onClose();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  return (
    <Modal
      title={`Add effect to CAM ${n}`}
      onClose={onClose}
      footer={
        <>
          <button className="btn" onClick={onClose}>
            Cancel
          </button>
          <button className="btn primary" disabled={!pick} onClick={submit}>
            <Icon name="plus" size={14} />
            Add effect
          </button>
        </>
      }
    >
      {error && <ErrorBlock message={error} />}
      <div className="pick" role="radiogroup" aria-label="Effects">
        {schemas.map((s) => {
          const done = isAdded(camera, s.kind);
          const missing = s.requires.filter((k) => !isAdded(camera, k));
          const off = done || missing.length > 0 || Boolean(s.unavailable);
          return (
            <label key={s.kind} className={`pick-row${pick === s.kind ? ' sel' : ''}${off ? ' off' : ''}`}>
              <input type="radio" name="effect" aria-label={s.label} checked={pick === s.kind} disabled={!usable(s)} onChange={() => setPick(s.kind)} />
              <span className="ic">
                <Icon name={s.icon as IconName} size={18} />
              </span>
              <div>
                <b>{s.label}</b>
                <p>{s.description}</p>
                {(s.requires.length > 0 || s.unavailable) && (
                  <span className="req">
                    {s.unavailable ??
                      (missing.length ? `Add ${missing.map(labelOf).join(', ')} first.` : `Needs ${s.requires.map(labelOf).join(', ')}, already on this camera.`)}
                  </span>
                )}
              </div>
              {done && <span className="tag2">ADDED</span>}
            </label>
          );
        })}
      </div>
      <p className="note">New effects appear here when BreadCamera ships them. Each runs in its fixed place in the chain.</p>
    </Modal>
  );
}
