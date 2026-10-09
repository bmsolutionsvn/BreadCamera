import { useEffect, useState } from 'react';

import { onShellMessage, postToShell } from '../bridge';
import { ConfirmDialog } from './ConfirmDialog';

// The tray's Quit… asks the page; the operator confirms with a verb (spec §2).
// `open`/`onClose` let the File menu's Quit… use the same confirmation, so no
// menu item ever claims to quit while the window merely hides.
export function QuitDialog({ open = false, onClose }: { open?: boolean; onClose?: () => void }) {
  const [asked, setAsked] = useState(false);
  useEffect(() => onShellMessage((m) => m.type === 'confirmQuit' && setAsked(true)), []);
  const show = open || asked;
  if (!show) return null;
  const close = () => {
    setAsked(false);
    onClose?.();
  };
  return (
    <ConfirmDialog
      title="Quit BreadCamera?"
      body="Every output stops now and cameras on air lose picture. The engine shuts down."
      confirm="Quit and stop all outputs"
      onCancel={close}
      onConfirm={() => {
        postToShell({ type: 'quitConfirmed' });
        close();
      }}
    />
  );
}
