import { fireEvent, render, screen } from '@testing-library/react';

import { EngineClient } from '../api/client';
import { FakeWs } from '../test/fakeWs';
import { AddOutputDialog } from './AddOutputDialog';

test('an SRT output sends its connection and bitrate, and the format', () => {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  render(<AddOutputDialog client={c} camera={{ id: 'a', name: 'A' }} n={1} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Output type'), { target: { value: 'srt' } });
  fireEvent.change(screen.getByLabelText('Mode'), { target: { value: 'listener' } });
  fireEvent.change(screen.getByLabelText('Port'), { target: { value: '9300' } });
  fireEvent.change(screen.getByLabelText('Resolution'), { target: { value: '1280x720' } });
  fireEvent.click(screen.getByRole('button', { name: 'Add output' }));
  const add = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'addOutput');
  expect(add.args.output.kind).toBe('srt');
  expect(add.args.output.params).toMatchObject({ mode: 'listener', port: 9300, bitrateKbps: 8000 });
  expect(add.args.output.format).toEqual({ width: 1280, height: 720, halfRate: false });
});

test('an OMT output offers a mode and sends its mode and name', () => {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  render(<AddOutputDialog client={c} camera={{ id: 'a', name: 'A' }} n={1} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Output type'), { target: { value: 'omt' } });
  expect(screen.getByLabelText('Mode')).toBeTruthy();
  fireEvent.click(screen.getByRole('button', { name: 'Add output' }));
  const add = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'addOutput');
  expect(add.args.output).toMatchObject({
    kind: 'omt',
    params: { mode: 'hx', name: 'BreadCamera - A' },
    format: { width: 0, height: 0, halfRate: false },
  });
  expect(add.args.output.params.codec).toBe('hevc');  // the default codec is sent for OMT-HX
});

test('an OMT-HX output offers a codec and sends the one chosen', () => {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  render(<AddOutputDialog client={c} camera={{ id: 'a', name: 'A' }} n={1} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Output type'), { target: { value: 'omt' } });
  expect(screen.getByLabelText('Codec')).toBeTruthy();
  fireEvent.change(screen.getByLabelText('Codec'), { target: { value: 'h264' } });
  fireEvent.click(screen.getByRole('button', { name: 'Add output' }));
  const add = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'addOutput');
  expect(add.args.output.params).toMatchObject({ mode: 'hx', codec: 'h264', name: 'BreadCamera - A' });
});

test('a plain OMT output hides the codec and sends no codec', () => {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  render(<AddOutputDialog client={c} camera={{ id: 'a', name: 'A' }} n={1} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Output type'), { target: { value: 'omt' } });
  fireEvent.change(screen.getByLabelText('Mode'), { target: { value: 'full' } });
  expect(screen.queryByLabelText('Codec')).toBeNull();
  fireEvent.click(screen.getByRole('button', { name: 'Add output' }));
  const add = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'addOutput');
  expect(add.args.output.params.codec).toBeUndefined();
});

test('a virtual camera output picks the first free number and has no format choice', () => {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({
    type: 'snapshot',
    state: {},
    show: { cameras: [{ id: 'z', name: 'Z', input: { kind: 'srt', params: {} }, outputs: [{ id: 'v', kind: 'vcam', keyed: false, enabled: true, params: { index: 1 } }] }] },
  });
  render(<AddOutputDialog client={c} camera={{ id: 'a', name: 'A' }} n={1} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Output type'), { target: { value: 'vcam' } });
  expect(screen.queryByLabelText('Resolution')).toBeNull();
  fireEvent.click(screen.getByRole('button', { name: 'Add output' }));
  const add = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'addOutput');
  expect(add.args.output).toMatchObject({ kind: 'vcam', params: { index: 2 }, format: { width: 0, height: 0, halfRate: false } });
});
