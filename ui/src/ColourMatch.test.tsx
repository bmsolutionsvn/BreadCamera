import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';

import { EngineClient } from './api/client';
import { Workspace } from './App';
import { FakeWs } from './test/fakeWs';

const colorSchema = {
  kind: 'color',
  label: 'Colour',
  description: 'Calibrate this camera once against a colour chart (printed card or tablet); the correction is then locked.',
  icon: 'color',
  requires: [],
  onAdd: { enabled: true },
  fields: [
    { key: 'enabled', type: 'bool', default: false, label: 'On' },
    { key: 'mode', type: 'enum', default: 'calibrate', label: 'Mode', hidden: true },
    { key: 'matchTo', type: 'text', default: '', label: 'Match to', hidden: true },
  ],
};

const show = (color: Record<string, unknown>) => ({
  schemaVersion: 1,
  settings: { rate: '60/1', resolution: '1920x1080', holdMs: 5000, slatePath: '' },
  cameras: [
    { id: 'a', name: 'Stage left', input: { kind: 'srt', params: {} }, outputs: [], stages: { color } },
    { id: 'b', name: 'Guest', input: { kind: 'srt', params: {} }, outputs: [] },
  ],
});

const state = (match: string | null) => ({
  cameras: {
    a: { name: 'Stage left', phase: 'live', input: { state: 'live', error: null, health: 'receiving' }, stages: { color: { state: 'active', error: null, match } } },
    b: { name: 'Guest', phase: 'live', input: { state: 'live', error: null, health: 'receiving' } },
  },
});

async function setup(color: Record<string, unknown>, match: string | null = null) {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({ type: 'snapshot', state: state(match), show: show(color) });
  render(<Workspace client={c} />);
  const req = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'getStageSchemas');
  FakeWs.last.receive({ id: req.id, ok: true, result: [colorSchema] });
  const insp = screen.getByRole('complementary', { name: 'Inspector' });
  await waitFor(() => within(insp).getByRole('group', { name: 'Colour' }));
  return within(insp).getByRole('group', { name: 'Colour' });
}

const lastSetStage = () => FakeWs.last.sent.map((s) => JSON.parse(s)).filter((r) => r.cmd === 'setStage').pop();

test('switches to Match and picks a camera', async () => {
  const group = await setup({ enabled: true, mode: 'calibrate', matchTo: '' });
  expect(within(group).getByRole('button', { name: 'Calibrate' })).toBeInTheDocument();

  fireEvent.click(within(group).getByRole('radio', { name: 'Match' }));
  expect(lastSetStage().args).toEqual({ cameraId: 'a', stage: 'color', params: { mode: 'match' } });

  // The engine echoes the new show; the panel switches to Match and drops the calibrate controls.
  FakeWs.last.receive({ type: 'show', show: show({ enabled: true, mode: 'match', matchTo: '' }) });
  await waitFor(() => expect(within(group).queryByRole('button', { name: 'Calibrate' })).toBeNull());

  const picker = within(group).getByLabelText('Match to');
  expect(within(picker).getByRole('option', { name: 'CAM 2 · Guest' })).toBeInTheDocument();
  fireEvent.change(picker, { target: { value: 'b' } });
  expect(lastSetStage().args).toEqual({ cameraId: 'a', stage: 'color', params: { matchTo: 'b' } });
});

test('says when it is holding', async () => {
  const group = await setup({ enabled: true, mode: 'match', matchTo: 'b' }, 'holding-reference-down');
  expect(within(group).getByText('Holding — CAM 2 has no picture')).toBeInTheDocument();
});

test('shows the router refusal', async () => {
  const group = await setup({ enabled: true, mode: 'match', matchTo: '' });
  fireEvent.change(within(group).getByLabelText('Match to'), { target: { value: 'b' } });
  const pick = lastSetStage();
  FakeWs.last.receive({ id: pick.id, ok: false, error: { code: 'invalid', message: 'Guest already matches this camera, directly or through another camera.' } });
  await waitFor(() => expect(within(group).getByRole('alert')).toHaveTextContent('Guest already matches this camera, directly or through another camera.'));
});