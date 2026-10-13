import type { ErrorInfo } from '../api/protocol';
import { errorText } from '../errors';
import { Icon } from './Icon';

// Persistent block at the top of the affected panel; never a toast (spec §6.1).
export function ErrorBlock({ title, message, error, severity = 'danger', label }: { title?: string; message?: string; error?: ErrorInfo; severity?: 'danger' | 'warning' | 'info'; label?: string }) {
  const text = error ? errorText(error) : null;
  const headline = text ? text.headline : title;
  const detail = text ? text.detail : (message ?? '');
  return (
    <div className={`block ${severity}`} role={severity === 'danger' ? 'alert' : 'status'} aria-label={label}>
      <Icon name={severity === 'danger' ? 'err' : severity === 'warning' ? 'warn' : 'chart'} size={14} />
      <div>
        {headline && <b>{headline} </b>}
        <span className="d">{detail}</span>
      </div>
    </div>
  );
}
