import { Fragment, useRef, useState } from 'react';

import type { EngineClient, EngineSnapshot } from '../api/client';
import type { CameraConfig, StageSchemaJson } from '../api/protocol';
import type { Calibration } from '../calibration/useCalibration';
import { ColourPanel } from '../calibration/ColourPanel';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon, type IconName } from '../components/Icon';
import { ConfirmDialog } from '../dialogs/ConfirmDialog';
import { inputCode } from '../state/labels';
import { Section } from './Inspector';
import { StageForm } from './StageForm';

const STATE = { active: ['RUNNING', 'run'], bypassed: ['BYPASSED', 'bad'], off: ['OFF', 'dim'] } as const;

export const isAdded = (cam: CameraConfig, kind: string) => !!cam.stages && kind in cam.stages;

// Effects are per-camera add-ins (spec §5.4), shown in pipeline order.
export function Effects({ client, snap, cam, n, schemas, onAdd, calib }: {
  client: EngineClient;
  snap: EngineSnapshot;
  cam: CameraConfig;
  n: number;
  schemas: StageSchemaJson[];
  onAdd: () => void;
  calib: Calibration;
}) {
  const [errors, setErrors] = useState<Record<string, string>>({});
  const [removing, setRemoving] = useState<StageSchemaJson | null>(null);
  const timers = useRef<Record<string, ReturnType<typeof setTimeout>>>({});
  const pending = useRef<Record<string, Record<string, unknown>>>({});
  const cs = snap.state.cameras?.[cam.id];
  const added = schemas.filter((s) => isAdded(cam, s.kind));
  const labelOf = (kind: string) => schemas.find((s) => s.kind === kind)?.label ?? kind;

  function flush(kind: string) {
    const params = pending.current[kind];
    pending.current[kind] = {};
    client
      .request('setStage', { cameraId: cam.id, stage: kind, params })
      .then(() => setErrors((e) => ({ ...e, [kind]: '' })))
      .catch((err: Error) => setErrors((e) => ({ ...e, [kind]: err.message })));
  }

  // Sliders fire many changes: one setStage per effect every 150 ms.
  // The on/off switch is binary and visible — flush it now so it never snaps back.
  function change(kind: string, patch: Record<string, unknown>) {
    pending.current[kind] = { ...pending.current[kind], ...patch };
    clearTimeout(timers.current[kind]);
    if ('enabled' in patch) {
      flush(kind);
      return;
    }
    timers.current[kind] = setTimeout(() => flush(kind), 150);
  }

  return (
    <Section
      title="Effects"
      right={
        <button className="tb sm" onClick={onAdd}>
          <Icon name="plus" size={12} />
          Add effect
        </button>
      }
    >
      <div className="chain" aria-label="Processing order">
        <span>{inputCode(cam.input.kind)} in</span>
        {added.map((s) => (
          <Fragment key={s.kind}>
            <Icon name="arrow" size={10} />
            <span className="fxn">{s.label}</span>
          </Fragment>
        ))}
        <Icon name="arrow" size={10} />
        <span>{cam.outputs.length} outputs</span>
      </div>
      {added.length === 0 && <p className="note">No effects yet. Add one to process this camera.</p>}
      {added.map((s) => {
        const st = cs?.stages?.[s.kind];
        const values = cam.stages?.[s.kind] ?? {};
        const on = Boolean(values.enabled ?? s.fields[0]?.default);
        const [stateLabel, cls] = (STATE as Record<string, readonly [string, string]>)[st?.state ?? 'off'] ?? STATE.off;
        const missing = s.requires.filter((k) => !isAdded(cam, k));
        return (
          <div className="group" role="group" aria-label={s.label} key={s.kind}>
            <div className="group-h">
              <span className="gi">
                <Icon name={s.icon as IconName} size={14} />
              </span>
              <b>{s.label}</b>
              <span className="right">
                <span className={`st ${cls}`}>{stateLabel}</span>
                <label className="sw-wrap">
                  <input type="checkbox" role="switch" aria-label={`${s.label} on`} checked={on} onChange={(e) => change(s.kind, { enabled: e.target.checked })} />
                  <span className="sw" />
                </label>
                <button className="ibtn" aria-label={`Remove ${s.label}`} onClick={() => setRemoving(s)}>
                  <Icon name="trash" size={14} />
                </button>
              </span>
            </div>
            <div className="group-b">
              {st?.error && <ErrorBlock title={`${s.label} bypassed.`} message={st.error.message} />}
              {errors[s.kind] && <ErrorBlock message={errors[s.kind]} />}
              {missing.length > 0 && <p className="note">Needs {missing.map(labelOf).join(', ')}, which is not on this camera.</p>}
              {s.kind === 'color' && <ColourPanel client={client} cam={cam} n={n} calib={calib} />}
              <StageForm schema={s} values={values} onChange={(p) => change(s.kind, p)} />
            </div>
          </div>
        );
      })}
      {added.length > 0 && <p className="note">Effects run in this fixed order.</p>}
      {removing && (
        <ConfirmDialog
          title={`Remove ${removing.label}?`}
          body={`${removing.label} stops on CAM ${n} now and its settings are dropped.`}
          confirm="Remove effect"
          onCancel={() => setRemoving(null)}
          onConfirm={async () => {
            await client.request('removeStage', { cameraId: cam.id, stage: removing.kind }).catch(() => {});
            setRemoving(null);
          }}
        />
      )}
    </Section>
  );
}
