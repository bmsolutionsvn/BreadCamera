import type { StageField, StageSchemaJson } from '../api/protocol';
import { Prop } from '../components/Prop';

// An effect's settings from the engine's schema (spec §5.4). fields[0] ("enabled") is the
// group's on/off switch, so it is not a row here.
export function StageForm({ schema, values, onChange }: { schema: StageSchemaJson; values: Record<string, unknown>; onChange: (patch: Record<string, unknown>) => void }) {
  const value = (f: StageField) => values[f.key] ?? f.default;
  return (
    <>
      {schema.fields.slice(1).map((f) => (
        <Prop key={f.key} label={f.label}>
          {(id) => {
            switch (f.type) {
              case 'bool':
                return (
                  <label className="sw-wrap">
                    <input id={id} type="checkbox" role="switch" checked={Boolean(value(f))} onChange={(e) => onChange({ [f.key]: e.target.checked })} />
                    <span className="sw" />
                  </label>
                );
              case 'enum':
                return (
                  <select id={id} className="ctl" value={String(value(f))} onChange={(e) => onChange({ [f.key]: e.target.value })}>
                    {(f.options ?? []).map((o, i) => (
                      <option key={o} value={o}>
                        {f.optionLabels?.[i] ?? o}
                      </option>
                    ))}
                  </select>
                );
              case 'number':
                return (
                  <div className="slider">
                    <input id={id} type="range" min={f.min} max={f.max} step={f.step} value={Number(value(f))} onChange={(e) => onChange({ [f.key]: Number(e.target.value) })} />
                    <span className="sv">{Number(value(f))}</span>
                  </div>
                );
              case 'color':
                return <input id={id} className="ctl" type="color" value={String(value(f))} onChange={(e) => onChange({ [f.key]: e.target.value })} />;
              case 'path':
                return <input id={id} className="ctl mono" value={String(value(f))} placeholder="C:\Shows\background.png" onChange={(e) => onChange({ [f.key]: e.target.value })} />;
              default:
                return null;
            }
          }}
        </Prop>
      ))}
    </>
  );
}
