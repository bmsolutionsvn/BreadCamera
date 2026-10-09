import { useEffect, useState } from 'react';

import { onShellMessage, postToShell } from '../bridge';
import { Icon } from '../components/Icon';
import { Menu, type MenuItem } from '../components/Menu';

export interface TitleMenus {
  file: (MenuItem | 'sep')[];
  camera: (MenuItem | 'sep')[];
  view: (MenuItem | 'sep')[];
  help: (MenuItem | 'sep')[];
}

// Custom title bar (spec §6.4): the shell removed the system caption; CSS app-region drags.
export function TitleBar({ menus }: { menus: TitleMenus }) {
  const [maximized, setMaximized] = useState(false);
  useEffect(() => onShellMessage((m) => m.type === 'windowState' && setMaximized(!!m.maximized)), []);
  const win = (action: string) => postToShell({ type: 'window', action });
  return (
    <header className="titlebar">
      <span className="app-ic">
        <Icon name="cam" />
      </span>
      <nav className="menus" aria-label="Menu bar">
        <Menu label="File" items={menus.file} />
        <Menu label="Camera" items={menus.camera} />
        <Menu label="View" items={menus.view} />
        <Menu label="Help" items={menus.help} />
      </nav>
      <div className="wintitle">BreadCamera</div>
      <div className="caption">
        <button aria-label="Minimise" onClick={() => win('minimize')}>
          <Icon name="min" size={12} />
        </button>
        <button aria-label={maximized ? 'Restore' : 'Maximise'} onClick={() => win('maximize')}>
          <Icon name={maximized ? 'restore' : 'max'} size={12} />
        </button>
        <button className="close" aria-label="Close to tray (outputs keep running)" onClick={() => win('close')}>
          <Icon name="close" size={12} />
        </button>
      </div>
    </header>
  );
}
