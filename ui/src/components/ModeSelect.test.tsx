import { fireEvent, render, screen, waitFor } from '@testing-library/react';

import { EngineClient, type EngineSnapshot } from '../api/client';
import type { CameraConfig } from '../api/protocol';
import { AddCameraDialog } from '../dialogs/AddCameraDialog';
import { Inspector } from '../inspector/Inspector';
import { FakeWs } from '../test/fakeWs';

function client() {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  return c;
}
const sent = () => FakeWs.last.sent.map((s) => JSON.parse(s));
const answer = (cmd: string, result: unknown) => FakeWs.last.receive({ id: sent().find((r) => r.cmd === cmd).id, ok: true, result });

test('a device camera lists its native modes OBS-style and sends the chosen one', async () => {
  const c = client();
  render(<AddCameraDialog client={c} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'Cam Link' } });
  fireEvent.change(screen.getByLabelText('Input type'), { target: { value: 'uvc' } });
  answer('listCaptureDevices', [{ name: 'Cam Link 4K', id: 'usb#cl4k' }]);
  await waitFor(() => expect(screen.getByRole('option', { name: 'Cam Link 4K' })).toBeInTheDocument());
  fireEvent.change(screen.getByLabelText('Device'), { target: { value: 'usb#cl4k' } });
  expect(sent().find((r) => r.cmd === 'listCaptureModes').args).toEqual({ device: 'usb#cl4k' });
  answer('listCaptureModes', [
    { width: 1920, height: 1080, fps: 59.94 },
    { width: 1440, height: 1080, fps: 30 },
    { width: 1280, height: 720, fps: 60 },
  ]);
  await waitFor(() => expect(screen.getByRole('option', { name: '720p60' })).toBeInTheDocument());
  const mode = screen.getByLabelText('Mode') as HTMLSelectElement;
  expect([...mode.options].map((o) => o.text)).toEqual(['Auto', '1080p59.94', '1440x1080 30', '720p60']);
  expect(mode.value).toBe('auto');
  fireEvent.change(mode, { target: { value: '1280x720@60' } });
  fireEvent.click(screen.getByRole('button', { name: 'Add camera' }));
  expect(sent().find((r) => r.cmd === 'addCamera').args.input).toEqual({
    kind: 'uvc',
    params: { device: 'usb#cl4k', width: 1280, height: 720, fps: 60 },
  });
});

test('a DeckLink card offers its modes; Auto sends no mode', async () => {
  const c = client();
  render(<AddCameraDialog client={c} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'SDI 1' } });
  fireEvent.change(screen.getByLabelText('Input type'), { target: { value: 'decklink' } });
  answer('listDeckLinkDevices', [
    { index: 0, name: 'DeckLink Duo 2', hasInput: true, hasOutput: true, externalKeying: false, modes: ['1080p50', '1080i50', '720p50'] },
  ]);
  await waitFor(() => expect(screen.getByRole('option', { name: '1. DeckLink Duo 2' })).toBeInTheDocument());
  fireEvent.change(screen.getByLabelText('Card'), { target: { value: '0' } });
  const mode = screen.getByLabelText('Mode') as HTMLSelectElement;
  expect([...mode.options].map((o) => o.text)).toEqual(['Auto', '1080p50', '1080i50', '720p50']);
  fireEvent.click(screen.getByRole('button', { name: 'Add camera' }));
  expect(sent().find((r) => r.cmd === 'addCamera').args.input).toEqual({ kind: 'decklink', params: { device: 0 } });
});

test('the Inspector changes a DeckLink camera mode and keeps a saved mode the card no longer lists', async () => {
  const c = client();
  const cam: CameraConfig = {
    id: 'a',
    name: 'SDI 1',
    input: { kind: 'decklink', params: { device: 0, mode: '1080p25' } },
    outputs: [],
  } as unknown as CameraConfig;
  const snap = { state: { cameras: {} }, stats: undefined } as unknown as EngineSnapshot;
  render(<Inspector client={c} snap={snap} cam={cam} n={1} onAddOutput={() => {}} onRemoved={() => {}} />);
  answer('listDeckLinkDevices', [{ index: 0, name: 'DeckLink', hasInput: true, hasOutput: false, externalKeying: false, modes: ['1080p50'] }]);
  await waitFor(() => expect(screen.getByRole('option', { name: '1080p50' })).toBeInTheDocument());
  const mode = screen.getByLabelText('Mode') as HTMLSelectElement;
  expect(mode.value).toBe('1080p25');
  expect(screen.getByRole('button', { name: 'Apply input changes' })).toBeDisabled();
  fireEvent.change(mode, { target: { value: '1080p50' } });
  fireEvent.click(screen.getByRole('button', { name: 'Apply input changes' }));
  expect(sent().find((r) => r.cmd === 'updateCamera').args).toEqual({ id: 'a', input: { kind: 'decklink', params: { device: 0, mode: '1080p50' } } });
});
