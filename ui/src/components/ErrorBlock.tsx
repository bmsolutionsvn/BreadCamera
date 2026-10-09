import { Icon } from './Icon';

// Persistent block at the top of the affected panel; never a toast (spec §6.1).
export function ErrorBlock({ title, message, severity = 'danger', label }: { title?: string; message: string; severity?: 'danger' | 'warning' | 'info'; label?: string }) {
  return (
    <div className={`block ${severity}`} role={severity === 'danger' ? 'alert' : 'status'} aria-label={label}>
      <Icon name={severity === 'danger' ? 'err' : severity === 'warning' ? 'warn' : 'chart'} size={14} />
      <div>
        {title && <b>{title} </b>}
        <span className="d">{message}</span>
      </div>
    </div>
  );
}
