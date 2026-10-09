import { useEffect, type ReactNode } from 'react';

import { Icon } from './Icon';

// Native-style dialog: title bar with close, body, footer band. Escape closes.
export function Modal({ title, onClose, footer, children, wide }: { title: string; onClose: () => void; footer: ReactNode; children: ReactNode; wide?: boolean }) {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && onClose();
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [onClose]);
  return (
    <div className="scrim">
      <div className={`dlg${wide ? ' wide' : ''}`} role="dialog" aria-modal="true" aria-label={title}>
        <div className="dlg-h">
          {title}
          <button className="x" aria-label="Close" onClick={onClose}>
            <Icon name="close" size={12} />
          </button>
        </div>
        <div className="dlg-b">{children}</div>
        <div className="dlg-f">{footer}</div>
      </div>
    </div>
  );
}
