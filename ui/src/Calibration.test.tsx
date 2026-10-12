import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';

import { EngineClient } from './api/client';
import { Workspace } from './App';
import { FakeWs } from './test/fakeWs';

const colorSchema = {
  kind: 'color',
  label: 'Colour',
  description: 'Calibrate once against a colour chart.',
  icon: 'color',
  requires: [],
  onAdd: { enabled: true },
  fields: [
    { key: 'enabled', type: 'bool', default: false, label: 'On' },
    { key: 'lut', type: 'path', default: '', label: 'LUT', hidden: true },
  ],
};
const at = new Date(2026, 9, 8, 19, 40).getTime() / 1000;
function show(color: Record<string, unknown>) {
  return {
    schemaVersion: 1,
    settings: { rate: '60/1', resolution: '1920x1080', holdMs: 5000, slatePath: '' },
    cameras: [{ id: 'a', name: 'MC chính – Cam 2', input: { kind: 'srt', params: {} }, outputs: [], stages: { color } }],
  };
}
const state = { cameras: { a: { name: 'MC chính – Cam 2', phase: 'live', input: { state: 'live', error: null, health: 'receiving' }, stages: { color: { state: 'active', error: null } } } } };

async function setup(color: Record<string, unknown>) {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({ type: 'snapshot', state, show: show(color) });
  render(<Workspace client={c} />);
  const req = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'getStageSchemas');
  FakeWs.last.receive({ id: req.id, ok: true, result: [colorSchema] });
  const insp = screen.getByRole('complementary', { name: 'Inspector' });
  await waitFor(() => within(insp).getByRole('group', { name: 'Colour' }));
  return within(insp).getByRole('group', { name: 'Colour' });
}

test('a calibrated camera shows the result in words, with the numbers in the tooltip', async () => {
  const group = await setup({ enabled: true, lut: 'C:/l.cube', previousLut: '', chart: 'card', deltaE: 1.1, deltaEBefore: 7.4, calibratedAt: at, reference: false });
  const line = within(group).getByText(/Good match · 19:40/);
  expect(line.closest('[title]')).toHaveAttribute('title', expect.stringContaining('7.4'));
  expect(within(group).getByRole('button', { name: 'Calibrate again' })).toBeInTheDocument();
  expect(within(group).queryByRole('button', { name: 'Undo' })).toBeNull();
});

test('Calibrate shows the camera alone, looks for the chart and captures with the corners', async () => {
  const group = await setup({ enabled: true, lut: '', previousLut: '', chart: 'card', deltaE: 0, deltaEBefore: 0, calibratedAt: 0, reference: false });
  expect(within(group).getByText('Not calibrated')).toBeInTheDocument();
  fireEvent.click(within(group).getByRole('button', { name: 'Calibrate' }));
  expect(screen.getByText('Calibrating colour')).toBeInTheDocument();
  const find = FakeWs.last.lastRequest();
  expect(find.cmd).toBe('findChart');
  FakeWs.last.receive({ id: find.id, ok: true, result: { available: true, found: true, corners: [[0.3, 0.25], [0.72, 0.22], [0.74, 0.7], [0.28, 0.72]] } });
  await waitFor(() => expect(within(group).getByText(/Chart found/)).toBeInTheDocument());
  fireEvent.click(within(group).getByRole('radio', { name: 'Tablet' }));
  fireEvent.click(within(group).getByRole('button', { name: 'Capture' }));
  const cal = FakeWs.last.lastRequest();
  expect(cal.cmd).toBe('calibrateColor');
  expect(cal.args).toEqual({ cameraId: 'a', chart: 'tablet', corners: [[0.3, 0.25], [0.72, 0.22], [0.74, 0.7], [0.28, 0.72]] });
  FakeWs.last.receive({ id: cal.id, ok: true, result: { deltaEBefore: 6, deltaE: 1.2, rating: 'good', reference: false } });
  await waitFor(() => expect(screen.queryByText('Calibrating colour')).toBeNull());
});

test('a failed capture keeps calibrating and shows why', async () => {
  const group = await setup({ enabled: true, lut: '', previousLut: '', chart: 'card', deltaE: 0, deltaEBefore: 0, calibratedAt: 0, reference: false });
  fireEvent.click(within(group).getByRole('button', { name: 'Calibrate' }));
  fireEvent.click(within(group).getByRole('button', { name: 'Capture' }));
  const cal = FakeWs.last.sent.map((s) => JSON.parse(s)).filter((r) => r.cmd === 'calibrateColor').pop();
  FakeWs.last.receive({ id: cal.id, ok: false, error: { code: 'invalid', message: 'the corners are not on the chart' } });
  await waitFor(() => expect(within(group).getByRole('alert')).toHaveTextContent('the corners are not on the chart'));
  expect(screen.getByText('Calibrating colour')).toBeInTheDocument();
});