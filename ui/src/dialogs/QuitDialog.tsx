import { useEffect, useState } from 'react';

import { onShellMessage, postToShell } from '../bridge';
import { ConfirmDialog } from './ConfirmDialog';

// The tray's Quit… asks the page; the operator confirms with a verb (spec §2).
export function QuitDialog() {
  const [open, setOpen] = useState(false);
  useEffect(() => onShellMessage((m) => m.type === 'confirmQuit' && setOpen(true)), []);
  if (!open) return null;
  return (
    <ConfirmDialog
      title="Quit BreadCamera?"
      body="Every output stops now and cameras on air lose picture. The engine shuts down."
      confirm="Quit and stop all outputs"
      onCancel={() => setOpen(false)}
      onConfirm={() => postToShell({ type: 'quitConfirmed' })}
    />
  );
}
