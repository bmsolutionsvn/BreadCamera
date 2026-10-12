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
