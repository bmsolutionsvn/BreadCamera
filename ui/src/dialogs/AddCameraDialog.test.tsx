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
