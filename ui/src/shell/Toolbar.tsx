import type { ReactNode } from 'react';

import { Icon } from '../components/Icon';

// Add camera is the only "add" here: outputs and effects belong to a camera (spec §6.4).
export function Toolbar({ onAddCamera, onStopAll, extra }: { onAddCamera: () => void; onStopAll: () => void; extra?: ReactNode }) {
  return (
    <div className="toolbar" role="toolbar" aria-label="Toolbar">
      <button className="tb accent" title="Add camera (Ctrl+Shift+N)" onClick={onAddCamera}>
        <Icon name="plus" />
        Add camera
      </button>
      <span className="tspace" />
      {extra}
      <span className="tsep" />
      <button className="tb danger" onClick={onStopAll}>
        <Icon name="power" />
        Stop all outputs…
      </button>
    </div>
  );
}
