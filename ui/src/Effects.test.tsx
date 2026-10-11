import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';

import { EngineClient } from './api/client';
import { Workspace } from './App';
import { FakeWs } from './test/fakeWs';

const schemas = [
  {
    kind: 'matte',
    label: 'Background removal',
    description: 'AI person cut-out without a green screen.',
    icon: 'matte',
    requires: [],
    onAdd: { enabled: true },
    fields: [
      { key: 'enabled', type: 'bool', default: false, label: 'On' },
      { key: 'quality', type: 'enum', default: 'quality', label: 'Quality', options: ['quality', 'performance'], optionLabels: ['High quality', 'Performance'] },
    ],
  },
  {
    kind: 'background',
    label: 'Background',
    description: 'Blur, replace or key the background.',
    icon: 'bg',
    requires: ['matte'],
    onAdd: { enabled: true, mode: 'blur' },
    fields: [
      { key: 'enabled', type: 'bool', default: true, label: 'On' },
      { key: 'mode', type: 'enum', default: 'none', label: 'Background mode', options: ['none', 'keyed', 'blur', 'replace'], optionLabels: ['None', 'Keyed (fill + key)', 'Blur', 'Replace'] },
    ],
  },
];
const show = {
  schemaVersion: 1,
  settings: { rate: '60/1', resolution: '1920x1080', holdMs: 5000, slatePath: '' },
  cameras: [
    {
      id: 'a',
      name: 'MC chính – Cam 2',
      input: { kind: 'srt', params: { mode: 'listener', port: 9000 } },
      outputs: [],
      stages: { matte: { enabled: true, quality: 'quality', choke: 0, feather: 1, threshold: 0.5 } },
    },
  ],
};
const state = {
  cameras: {
    a: {
      name: 'MC chính – Cam 2',
      phase: 'live',
      input: { state: 'live', error: null, health: 'receiving' },
      stages: { matte: { state: 'bypassed', error: { code: 'matte-unavailable', message: 'needs an NVIDIA RTX GPU' } } },
    },
  },
};

async function setup(opts: { wait?: string; show?: object; state?: object } = {}) {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({ type: 'snapshot', state: opts.state ?? state, show: opts.show ?? show });
  render(<Workspace client={c} />);
  const req = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'getStageSchemas');
  FakeWs.last.receive({ id: req.id, ok: true, result: schemas });
  const insp = screen.getByRole('complementary', { name: 'Inspector' });
  await waitFor(() => expect(within(insp).getByText(opts.wait ?? 'BYPASSED')).toBeInTheDocument());
  return insp;
}

test('an added effect shows its state, its error and its settings; the strip shows its icon', async () => {
  const insp = await setup();
  const group = within(insp).getByRole('group', { name: 'Background removal' });
  expect(within(group).getByRole('alert')).toHaveTextContent('needs an NVIDIA RTX GPU');
  expect(within(group).getByLabelText('Quality')).toHaveValue('quality');
  expect(screen.getByTitle('Background removal: bypassed')).toBeInTheDocument();
});

test('Add effect lists every effect and adds one with its onAdd params', async () => {
  const insp = await setup();
  fireEvent.click(within(insp).getByRole('button', { name: 'Add effect' }));
  const dlg = screen.getByRole('dialog', { name: 'Add effect to CAM 1' });
  expect(within(dlg).getByRole('radio', { name: 'Background removal' })).toBeDisabled();
  expect(within(dlg).getByText('ADDED')).toBeInTheDocument();
  fireEvent.click(within(dlg).getByRole('radio', { name: 'Background' }));
  fireEvent.click(within(dlg).getByRole('button', { name: 'Add effect' }));
  const r = FakeWs.last.lastRequest();
  expect(r.cmd).toBe('setStage');
  expect(r.args).toEqual({ cameraId: 'a', stage: 'background', params: { enabled: true, mode: 'blur' } });
});

test('removing an effect asks first', async () => {
  const insp = await setup();
  fireEvent.click(within(insp).getByRole('button', { name: 'Remove Background removal' }));
  const dlg = screen.getByRole('dialog', { name: 'Remove Background removal?' });
  fireEvent.click(within(dlg).getByRole('button', { name: 'Remove effect' }));
  expect(FakeWs.last.lastRequest()).toMatchObject({ cmd: 'removeStage', args: { cameraId: 'a', stage: 'matte' } });
});

test('rapid setting changes coalesce into one setStage with the latest params', async () => {
  const insp = await setup();
  vi.useFakeTimers();
  try {
    const group = within(insp).getByRole('group', { name: 'Background removal' });
    const quality = within(group).getByLabelText('Quality');
    fireEvent.change(quality, { target: { value: 'performance' } });
    fireEvent.change(quality, { target: { value: 'quality' } });
    const before = FakeWs.last.sent.length;
    vi.advanceTimersByTime(150);
    const setStages = FakeWs.last.sent
      .slice(before)
      .map((s) => JSON.parse(s))
      .filter((r) => r.cmd === 'setStage');
    expect(setStages).toHaveLength(1);
    expect(setStages[0].args).toEqual({ cameraId: 'a', stage: 'matte', params: { quality: 'quality' } });
  } finally {
    vi.useRealTimers();
  }
});

test('toggling the effect switch sends setStage immediately', async () => {
  const insp = await setup();
  const group = within(insp).getByRole('group', { name: 'Background removal' });
  const before = FakeWs.last.sent.length;
  fireEvent.click(within(group).getByRole('switch', { name: 'Background removal on' }));
  const setStages = FakeWs.last.sent
    .slice(before)
    .map((s) => JSON.parse(s))
    .filter((r) => r.cmd === 'setStage');
  expect(setStages).toHaveLength(1);
  expect(setStages[0].args).toEqual({ cameraId: 'a', stage: 'matte', params: { enabled: false } });
});

test('an effect whose requirement is missing is explained, not hidden', async () => {
  const insp = await setup({
    wait: 'No effects yet. Add one to process this camera.',
    show: { ...show, cameras: [{ ...show.cameras[0], stages: {} }] },
    state: { cameras: { a: { ...state.cameras.a, stages: {} } } },
  });
  fireEvent.click(within(insp).getByRole('button', { name: 'Add effect' }));
  const dlg = screen.getByRole('dialog', { name: 'Add effect to CAM 1' });
  await waitFor(() => expect(within(dlg).getByRole('radio', { name: 'Background' })).toBeDisabled());
  expect(within(dlg).getByText('Add Background removal first.')).toBeInTheDocument();
});
