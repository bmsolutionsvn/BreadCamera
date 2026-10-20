import { fireEvent, render, screen, within } from '@testing-library/react';

import { EngineClient } from './api/client';
import { Workspace } from './App';
import { FakeWs } from './test/fakeWs';

const show = {
  schemaVersion: 1,
  settings: { rate: '60000/1001', resolution: '1920x1080', holdMs: 5000, slatePath: '' },
  cameras: [
    {
      id: 'a',
      name: 'Sân khấu trái',
      input: { kind: 'srt', params: { mode: 'caller', url: 'srt://1.2.3.4:9000', latencyMs: 120 } },
      outputs: [
        { id: 'o1', kind: 'ndi', keyed: false, enabled: true, params: { name: 'BreadCamera - Sân khấu trái' }, format: { halfRate: false } },
        { id: 'o2', kind: 'fullscreen', keyed: false, enabled: true, params: { monitor: 1 }, format: { halfRate: false } },
      ],
    },
    { id: 'b', name: 'Khách mời', input: { kind: 'srt', params: { mode: 'listener', port: 9001 } }, outputs: [] },
  ],
};
const state = {
  engine: { gpu: 'NVIDIA GeForce RTX 4070', rate: '60000/1001', resolution: '1920x1080', overruns: 0 },
  cameras: {
    a: {
      name: 'Sân khấu trái',
      phase: 'live',
      input: { state: 'live', error: null, health: 'receiving' },
      outputs: {
        o1: { kind: 'ndi', state: 'live', error: null },
        o2: { kind: 'fullscreen', state: 'offline', error: { code: 'output-failed', message: 'monitor 2 is not connected' } },
      },
    },
    b: { name: 'Khách mời', phase: 'slate', input: { state: 'offline', error: { code: 'input-failed', message: 'cannot open srt://:9001' }, health: 'down' } },
  },
};

function client(s: unknown = state, sh: unknown = show) {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({ type: 'snapshot', state: s, show: sh });
  return c;
}

test('strips show input health, names and every output state', () => {
  render(<Workspace client={client()} />);
  expect(screen.getByTitle('SRT input: receiving')).toHaveTextContent('SRT');
  expect(screen.getByTitle('NDI output: Live')).toHaveTextContent('NDI');
  expect(screen.getByTitle('Fullscreen: failed, monitor 2 is not connected')).toBeInTheDocument();
  expect(screen.getByTitle('SRT input: down, showing the slate')).toHaveTextContent('SLATE');
  expect(screen.getAllByText('Khách mời').length).toBeGreaterThan(0);
});

test('the status bar no longer shows a show-wide resolution', () => {
  render(<Workspace client={client()} />);
  const bar = screen.getByLabelText('Status bar');
  expect(within(bar).getByText('59.94')).toBeInTheDocument();  // the rate stays
  expect(within(bar).queryByText(/1920×1080/)).toBeNull();     // the size label is gone
});

test('names the heaviest stage when ticks overrun', () => {
  render(
    <Workspace
      client={client({
        ...state,
        engine: { ...state.engine, overruns: 12 },
        cameras: {
          a: { ...state.cameras.a, stages: { color: { state: 'active', error: null, gpuMs: 0.4 } } },
          b: { ...state.cameras.b, stages: { upscale: { state: 'active', error: null, gpuMs: 6.2 } } },
        },
      })}
    />,
  );
  const bar = screen.getByLabelText('Status bar');
  expect(within(bar).getByText(/heaviest: CAM 2 Upscale 6\.2 ms/)).toBeInTheDocument();
});

test('the first camera is selected; arrow keys and clicks move the Inspector', () => {
  render(<Workspace client={client()} />);
  const insp = screen.getByRole('complementary', { name: 'Inspector' });
  expect(within(insp).getByText('Sân khấu trái')).toBeInTheDocument();
  expect(within(insp).getByRole('alert')).toHaveTextContent('monitor 2 is not connected');
  fireEvent.keyDown(screen.getByRole('listbox', { name: 'Multiview' }), { key: 'ArrowRight' });
  expect(within(insp).getByText('Khách mời')).toBeInTheDocument();
  expect(within(insp).getByRole('alert')).toHaveTextContent('cannot open srt://:9001');
  fireEvent.click(screen.getByRole('option', { name: /CAM 1/ }));
  expect(within(insp).getByText('Sân khấu trái')).toBeInTheDocument();
});

test('an empty show points to Add camera', () => {
  render(<Workspace client={client({}, { ...show, cameras: [] })} />);
  expect(screen.getByText('No cameras yet')).toBeInTheDocument();
});

test('Add output sends the chosen half rate', () => {
  render(<Workspace client={client()} />);
  fireEvent.click(within(screen.getByRole('complementary', { name: 'Inspector' })).getByRole('button', { name: 'Add output' }));
  const dlg = screen.getByRole('dialog', { name: 'Add output to CAM 1' });
  fireEvent.change(within(dlg).getByLabelText('Frame rate'), { target: { value: 'half' } });
  fireEvent.click(within(dlg).getByRole('button', { name: 'Add output' }));
  const req = FakeWs.last.lastRequest();
  expect(req.cmd).toBe('addOutput');
  expect((req.args.output as { format: unknown }).format).toEqual({ halfRate: true });
});

test('Stop all outputs asks first, with a verb button', () => {
  render(<Workspace client={client()} />);
  fireEvent.click(screen.getByRole('button', { name: 'Stop all outputs…' }));
  const dlg = screen.getByRole('dialog', { name: 'Stop all outputs?' });
  fireEvent.click(within(dlg).getByRole('button', { name: 'Stop all outputs' }));
  expect(FakeWs.last.lastRequest().cmd).toBe('stopAllOutputs');
});

test('window buttons post to the shell', () => {
  const postMessage = vi.fn();
  (window as unknown as { chrome: unknown }).chrome = { webview: { postMessage, addEventListener() {}, removeEventListener() {} } };
  render(<Workspace client={client()} />);
  fireEvent.click(screen.getByRole('button', { name: 'Minimise' }));
  expect(postMessage).toHaveBeenCalledWith('{"type":"window","action":"minimize"}');
  fireEvent.click(screen.getByRole('button', { name: 'Quit BreadCamera' }));
  expect(postMessage).toHaveBeenCalledWith('{"type":"window","action":"close"}');
  delete (window as unknown as { chrome?: unknown }).chrome;
});

function subscribed(): string[] {
  const msgs = FakeWs.last.sent
    .map((s) => JSON.parse(s) as { cmd: string; args: { cameraIds: string[] } })
    .filter((m) => m.cmd === 'subscribePreview');
  return msgs.length ? msgs[msgs.length - 1].args.cameraIds : [];
}

test('clicking a tile focuses one camera; Esc returns to the grid', () => {
  render(<Workspace client={client()} />);
  expect(subscribed()).toEqual(['a', 'b']);
  fireEvent.click(screen.getByRole('option', { name: /CAM 2/ }));
  expect(subscribed()).toEqual(['b']);
  expect(screen.getAllByRole('option', { name: /^CAM / })).toHaveLength(1);
  expect(screen.getByRole('button', { name: 'Grid' })).toBeInTheDocument();
  fireEvent.keyDown(window, { key: 'Escape' });
  expect(subscribed()).toEqual(['a', 'b']);
  expect(screen.getAllByRole('option', { name: /^CAM / })).toHaveLength(2);
});

test('Escape closes a dialog opened while focused, keeping the focus view', () => {
  render(<Workspace client={client()} />);
  fireEvent.click(screen.getByRole('option', { name: /CAM 2/ }));
  expect(subscribed()).toEqual(['b']);
  // Open a dialog from the focused view, like a user would (Ctrl+, opens Settings).
  fireEvent.keyDown(window, { key: ',', ctrlKey: true });
  expect(screen.getByRole('dialog', { name: 'Settings' })).toBeInTheDocument();
  // One Escape closes the dialog and nothing else: the focused view must stay focused.
  fireEvent.keyDown(window, { key: 'Escape' });
  expect(screen.queryByRole('dialog', { name: 'Settings' })).not.toBeInTheDocument();
  expect(subscribed()).toEqual(['b']);
  expect(screen.getAllByRole('option', { name: /^CAM / })).toHaveLength(1);
});

test('the Grid button and clicking the focused picture both return to the grid', () => {
  render(<Workspace client={client()} />);
  fireEvent.click(screen.getByRole('option', { name: /CAM 2/ }));
  expect(subscribed()).toEqual(['b']);
  fireEvent.click(screen.getByRole('button', { name: 'Grid' }));
  expect(subscribed()).toEqual(['a', 'b']);

  fireEvent.click(screen.getByRole('option', { name: /CAM 1/ }));
  expect(subscribed()).toEqual(['a']);
  fireEvent.click(screen.getByRole('option', { name: /CAM 1/ }));
  expect(subscribed()).toEqual(['a', 'b']);
  expect(screen.getAllByRole('option', { name: /^CAM / })).toHaveLength(2);
});

test('losing the engine shows the restart block and every pill goes Offline', () => {
  const c = client();
  render(<Workspace client={c} />);
  FakeWs.last.close();
  expect(screen.getByRole('status', { name: 'Engine restarting' })).toBeInTheDocument();
  expect(screen.getByTitle('NDI output: Offline')).toBeInTheDocument();
  c.close();
});
