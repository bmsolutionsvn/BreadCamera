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
        { id: 'o1', kind: 'ndi', keyed: false, enabled: true, params: { name: 'BreadCamera - Sân khấu trái' }, format: { width: 0, height: 0, halfRate: false } },
        { id: 'o2', kind: 'fullscreen', keyed: false, enabled: true, params: { monitor: 1 }, format: { width: 0, height: 0, halfRate: false } },
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

test('Add output sends the chosen size and half rate', () => {
  render(<Workspace client={client()} />);
  fireEvent.click(within(screen.getByRole('complementary', { name: 'Inspector' })).getByRole('button', { name: 'Add output' }));
  const dlg = screen.getByRole('dialog', { name: 'Add output to CAM 1' });
  fireEvent.change(within(dlg).getByLabelText('Resolution'), { target: { value: '1280x720' } });
  fireEvent.change(within(dlg).getByLabelText('Frame rate'), { target: { value: 'half' } });
  fireEvent.click(within(dlg).getByRole('button', { name: 'Add output' }));
  const req = FakeWs.last.lastRequest();
  expect(req.cmd).toBe('addOutput');
  expect((req.args.output as { format: unknown }).format).toEqual({ width: 1280, height: 720, halfRate: true });
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

test('losing the engine shows the restart block and every pill goes Offline', () => {
  const c = client();
  render(<Workspace client={c} />);
  FakeWs.last.close();
  expect(screen.getByRole('status', { name: 'Engine restarting' })).toBeInTheDocument();
  expect(screen.getByTitle('NDI output: Offline')).toBeInTheDocument();
  c.close();
});
