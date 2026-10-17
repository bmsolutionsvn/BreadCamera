import { fireEvent, render, screen, within } from '@testing-library/react';

import { EngineClient } from '../api/client';
import { FakeWs } from '../test/fakeWs';
import { SettingsDialog } from './SettingsDialog';

function client() {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({
    type: 'snapshot',
    state: { engine: { gpu: 'AMD Radeon RX 6600', matteBackend: '', previewEncoder: 'h264_amf' } },
    show: { settings: { rate: '60000/1001', resolution: '1920x1080', holdMs: 5000, slatePath: '' }, cameras: [] },
  });
  return c;
}
const sent = (cmd: string) => FakeWs.last.sent.map((s) => JSON.parse(s)).filter((r) => r.cmd === cmd);

test('saves the hold time without asking', () => {
  render(<SettingsDialog client={client()} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Hold the last frame on signal loss (s)'), { target: { value: '8' } });
  fireEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(sent('setSettings')[0].args).toEqual({ holdMs: 8000 });
});

test('the Settings dialog no longer offers a Resolution field', () => {
  render(<SettingsDialog client={client()} onClose={() => {}} />);
  expect(screen.queryByLabelText('Resolution')).toBeNull();
});

test('a frame-rate change asks first, with a verb button', () => {
  render(<SettingsDialog client={client()} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Frame rate'), { target: { value: '50/1' } });
  fireEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(sent('setSettings')).toHaveLength(0);
  fireEvent.click(within(screen.getByRole('dialog', { name: 'Change the show format?' })).getByRole('button', { name: 'Change format' }));
  expect(sent('setSettings')[0].args).toEqual({ rate: '50/1' });
});

test('the Engine page shows the GPU and the missing matte backend', () => {
  render(<SettingsDialog client={client()} onClose={() => {}} />);
  fireEvent.click(screen.getByRole('tab', { name: 'Engine' }));
  expect(screen.getByText('AMD Radeon RX 6600')).toBeInTheDocument();
  expect(screen.getByText('None')).toBeInTheDocument();
});