import { useId, type ReactNode } from 'react';

// Label-left property row (spec §6.4). The render prop receives the id to label the control.
export function Prop({ label, children, hint }: { label: string; children: (id: string) => ReactNode; hint?: string }) {
  const id = useId();
  return (
    <div className="prop">
      <label htmlFor={id}>{label}</label>
      <div className="prop-c">
        {children(id)}
        {hint && <span className="hint">{hint}</span>}
      </div>
    </div>
  );
}
