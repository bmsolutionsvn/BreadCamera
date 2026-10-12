import { useEffect, useMemo, useRef, useState } from 'react';

import { EngineClient } from './api/client';
import { useEngine } from './api/useEngine';
import { CalibrationView } from './calibration/CalibrationView';
import { useCalibration } from './calibration/useCalibration';
import { ErrorBlock } from './components/ErrorBlock';
import { AddCameraDialog } from './dialogs/AddCameraDialog';
import { AddEffectDialog } from './dialogs/AddEffectDialog';
import { AddOutputDialog } from './dialogs/AddOutputDialog';
import { ConfirmDialog } from './dialogs/ConfirmDialog';
import { QuitDialog } from './dialogs/QuitDialog';
import { Effects } from './inspector/Effects';
import { Inspector } from './inspector/Inspector';
import { Multiview, type Layout } from './multiview/Multiview';
import { StatusBar } from './shell/StatusBar';
import { TitleBar } from './shell/TitleBar';
import { Toolbar } from './shell/Toolbar';
import { nextMarks, type Marks } from './state/marks';
import { useStageSchemas } from './state/schemas';

function engineUrl(): string | null {
  const p = new URLSearchParams(location.hash.slice(1));
  const port = p.get('port');
  const token = p.get('token');
  return port && token ? `ws://127.0.0.1:${port}/?token=${encodeURIComponent(token)}` : null;
}

// Everything below the engine connection; tests render this with a fake client.
export function Workspace({ client }: { client: EngineClient }) {
  const snap = useEngine(client);
  const schemas = useStageSchemas(client);
  const cameras = snap.show?.cameras ?? [];
  const offline = snap.status !== 'open';
  const [selected, setSelected] = useState<string | null>(null);
  const [layout, setLayout] = useState<Layout>('auto');
  const [dialog, setDialog] = useState<null | 'addCamera' | 'addOutput' | 'addEffect' | 'stopAll' | 'quit'>(null);
  const [now, setNow] = useState(() => Date.now());
  const marks = useRef<Record<string, Marks>>({});
  const calib = useCalibration(client);

  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(t);
  }, []);
  useEffect(() => {
    if (selected && cameras.some((c) => c.id === selected)) return;
    setSelected(cameras[0]?.id ?? null);
  }, [cameras, selected]);
  useEffect(() => {
    const t = Date.now();
    for (const c of cameras) marks.current[c.id] = nextMarks(marks.current[c.id], snap.state.cameras?.[c.id], t);
  }, [snap.state, cameras]);
  useEffect(() => {
    if (calib.state) setSelected(calib.state.cameraId);
  }, [calib.state?.cameraId]);
  useEffect(() => {
    if (calib.state && !cameras.some((c) => c.id === calib.state!.cameraId)) calib.cancel();
  }, [cameras, calib.state?.cameraId]);

  function select(id: string) {
    setSelected(id);
    if (marks.current[id]) marks.current[id] = { ...marks.current[id], recoveredAt: undefined };
  }

  const index = cameras.findIndex((c) => c.id === selected);
  const cam = index >= 0 ? cameras[index] : undefined;
  const holdMs = snap.show?.settings?.holdMs ?? 5000;
  const menus = useMemo(
    () => ({
      file: [{ label: 'Quit…', onSelect: () => setDialog('quit') }],
      camera: [
        { label: 'Add camera…', shortcut: 'Ctrl+Shift+N', onSelect: () => setDialog('addCamera') },
        { label: 'Add output…', disabled: !cam, onSelect: () => setDialog('addOutput') },
        { label: 'Add effect…', disabled: !cam, onSelect: () => setDialog('addEffect') },
        { label: 'Calibrate colour…', shortcut: 'Ctrl+K', disabled: !cam || !cam.stages || !('color' in cam.stages), onSelect: () => cam && calib.start(cam.id) },
      ],
      view: (['auto', '2x2', '3x2', '4x2'] as Layout[]).map((l) => ({ label: `Layout ${l === 'auto' ? 'Auto' : l.replace('x', '×')}`, onSelect: () => setLayout(l) })),
      help: [{ label: 'BreadCamera 0.1', disabled: true, onSelect: () => {} }],
    }),
    [cam, calib.start],
  );
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape' && calib.state) {
        calib.cancel();
        return;
      }
      if (e.ctrlKey && e.shiftKey && e.key.toLowerCase() === 'n') {
        e.preventDefault();
        setDialog('addCamera');
        return;
      }
      if (e.ctrlKey && e.key.toLowerCase() === 'k') {
        e.preventDefault();
        if (cam?.stages && 'color' in cam.stages) calib.start(cam.id);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [calib.state, calib.cancel, calib.start, cam]);

  const calibIndex = cameras.findIndex((c) => c.id === calib.state?.cameraId);
  const calibCam = calibIndex >= 0 ? cameras[calibIndex] : undefined;

  return (
    <div className="win">
      <TitleBar menus={menus} />
      <Toolbar onAddCamera={() => setDialog('addCamera')} onStopAll={() => setDialog('stopAll')} />
      <div className="body">
        {calib.state && calibCam ? (
          <CalibrationView client={client} cam={calibCam} n={calibIndex + 1} state={calib.state} onCorner={calib.moveCorner} />
        ) : (
          <Multiview
            client={client}
            cameras={cameras}
            state={snap.state}
            schemas={schemas}
            marks={marks.current}
            holdMs={holdMs}
            now={now}
            selected={selected}
            onSelect={select}
            layout={layout}
            onLayout={setLayout}
            offline={offline}
          />
        )}
        <div className="splitter" />
        <Inspector client={client} snap={snap} cam={cam} n={index + 1} onAddOutput={() => setDialog('addOutput')} onRemoved={() => setSelected(null)}>
          {cam && <Effects key={cam.id} client={client} snap={snap} cam={cam} n={index + 1} schemas={schemas} onAdd={() => setDialog('addEffect')} calib={calib} />}
        </Inspector>
      </div>
      <StatusBar snap={snap} />
      {dialog === 'addCamera' && <AddCameraDialog client={client} onClose={() => setDialog(null)} />}
      {dialog === 'addOutput' && cam && <AddOutputDialog client={client} camera={cam} n={index + 1} onClose={() => setDialog(null)} />}
      {dialog === 'addEffect' && cam && <AddEffectDialog client={client} camera={cam} n={index + 1} schemas={schemas} onClose={() => setDialog(null)} />}
      {dialog === 'stopAll' && (
        <ConfirmDialog
          title="Stop all outputs?"
          body="Every receiver loses picture now. The engine and the show stay open; switch outputs back on in each camera's Inspector."
          confirm="Stop all outputs"
          onCancel={() => setDialog(null)}
          onConfirm={() => {
            client.request('stopAllOutputs').catch(() => {});
            setDialog(null);
          }}
        />
      )}
      <QuitDialog open={dialog === 'quit'} onClose={() => setDialog(null)} />
    </div>
  );
}

function Connected({ url }: { url: string }) {
  const client = useMemo(() => new EngineClient(url), [url]);
  useEffect(() => {
    client.connect();
    return () => client.close();
  }, [client]);
  return <Workspace client={client} />;
}

export function App() {
  const url = engineUrl();
  if (!url) return <ErrorBlock message="Engine connection details are missing. Open BreadCamera from its app icon." />;
  return <Connected url={url} />;
}
