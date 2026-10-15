import { fireEvent, render, screen, waitFor } from '@testing-library/react';

import { EngineClient } from '../api/client';
import { FakeWs } from '../test/fakeWs';
import { AddCameraDialog } from './AddCameraDialog';

function client() {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  return c;
}

test('NDI cameras pick a discovered source', async () => {
  const c = client();
  render(<AddCameraDialog client={c} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'Khách mời' } });
  fireEvent.change(screen.getByLabelText('Input type'), { target: { value: 'ndi' } });
  const list = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'listNdiSources');
  FakeWs.last.receive({ id: list.id, ok: true, result: ['STUDIO (vMix - Output 1)'] });
  await waitFor(() => expect(screen.getByRole('option', { name: 'STUDIO (vMix - Output 1)' })).toBeInTheDocument());
  fireEvent.change(screen.getByLabelText('NDI source'), { target: { value: 'STUDIO (vMix - Output 1)' } });
  fireEvent.click(screen.getByRole('button', { name: 'Add camera' }));
  const add = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'addCamera');
  expect(add.args).toEqual({ name: 'Khách mời', input: { kind: 'ndi', params: { source: 'STUDIO (vMix - Output 1)' } } });
});

test('a BreadCall guest slot is sent as a breadcall input', async () => {
  const c = client();
  render(<AddCameraDialog client={c} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'Guest 2' } });
  fireEvent.change(screen.getByLabelText('Input type'), { target: { value: 'breadcall' } });
  fireEvent.change(screen.getByLabelText('Room URL'), { target: { value: 'wss://livekit.example.com' } });
  fireEvent.change(screen.getByLabelText('Token'), { target: { value: 'abc.def.ghi' } });
  fireEvent.change(screen.getByLabelText('Slot'), { target: { value: '3' } });
  fireEvent.click(screen.getByRole('button', { name: 'Add camera' }));
  const add = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'addCamera');
  expect(add.args).toEqual({
    name: 'Guest 2',
    input: { kind: 'breadcall', params: { url: 'wss://livekit.example.com', token: 'abc.def.ghi', slot: 3 } },
  });
});

test('a built-in or USB camera is picked by device id', async () => {
  const deviceId = '\\\\?\\usb#vid_04f2';
  const c = client();
  render(<AddCameraDialog client={c} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'Laptop' } });
  fireEvent.change(screen.getByLabelText('Input type'), { target: { value: 'uvc' } });
  const list = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'listCaptureDevices');
  FakeWs.last.receive({ id: list.id, ok: true, result: [{ name: 'Integrated Camera', id: deviceId }] });
  await waitFor(() => expect(screen.getByRole('option', { name: 'Integrated Camera' })).toBeInTheDocument());
  fireEvent.change(screen.getByLabelText('Device'), { target: { value: deviceId } });
  fireEvent.click(screen.getByRole('button', { name: 'Add camera' }));
  const add = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'addCamera');
  expect(add.args.input).toEqual({ kind: 'uvc', params: { device: deviceId } });
});

test('a capture device another camera uses is disabled and named', async () => {
  const usedId = '\\\\?\\usb#vid_0fd9';
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({
    type: 'snapshot',
    state: {},
    show: {
      schemaVersion: 1,
      settings: { rate: '60000/1001', resolution: '1920x1080', holdMs: 5000, slatePath: '' },
      cameras: [{ id: 'a', name: 'Camera A', input: { kind: 'uvc', params: { device: usedId } }, outputs: [] }],
    },
  });
  render(<AddCameraDialog client={c} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Input type'), { target: { value: 'uvc' } });
  const list = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'listCaptureDevices');
  FakeWs.last.receive({ id: list.id, ok: true, result: [
    { name: 'Cam Link 4K', id: usedId },
    { name: 'FaceTime HD', id: '\\\\?\\usb#vid_04f2' },
  ] });
  const used = await screen.findByRole('option', { name: 'Cam Link 4K (Already used by Camera A)' });
  expect((used as HTMLOptionElement).disabled).toBe(true);
  expect(screen.getByRole('option', { name: 'FaceTime HD' })).not.toBeDisabled();
});
