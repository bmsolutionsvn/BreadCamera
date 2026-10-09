import { FakeWs } from '../test/fakeWs';
import { EngineClient, EngineError } from './client';

function make() {
  FakeWs.all = [];
  const c = new EngineClient('ws://x/?token=t', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  return c;
}

afterEach(() => vi.useRealTimers());

test('snapshot then patch build the state', () => {
  const c = make();
  FakeWs.last.receive({ type: 'snapshot', state: { cameras: { a: { name: 'A' } } }, show: { cameras: [] } });
  FakeWs.last.receive({ type: 'patch', ops: [{ op: 'replace', path: '/cameras/a/phase', value: 'live' }] });
  expect(c.getSnapshot().state.cameras?.a.phase).toBe('live');
});

test('requests resolve by id and server errors reject with the code', async () => {
  const c = make();
  const p = c.request('addCamera', { name: 'x' });
  const req = FakeWs.last.lastRequest();
  expect(req.cmd).toBe('addCamera');
  FakeWs.last.receive({ id: req.id, ok: true, result: { id: 'abc' } });
  await expect(p).resolves.toEqual({ id: 'abc' });
  const bad = c.request('removeCamera', { id: 'nope' });
  FakeWs.last.receive({ id: FakeWs.last.lastRequest().id, ok: false, error: { code: 'not-found', message: 'no camera nope' } });
  await expect(bad).rejects.toBeInstanceOf(EngineError);
  await expect(bad).rejects.toMatchObject({ code: 'not-found' });
});

test('after an engine restart the new snapshot replaces the old state, and previews resubscribe', async () => {
  vi.useFakeTimers();
  const c = make();
  c.setPreviewSubscription(['a'], 'small');
  FakeWs.last.receive({ type: 'snapshot', state: { cameras: { a: { name: 'A' }, b: { name: 'B' } } }, show: { cameras: [] } });
  const pending = c.request('ping');
  FakeWs.last.close();
  await expect(pending).rejects.toThrow();
  expect(c.getSnapshot().status).toBe('closed');
  vi.advanceTimersByTime(1000);
  const second = FakeWs.last;
  expect(FakeWs.all.length).toBe(2);
  second.open();
  expect(second.sent.some((s) => JSON.parse(s).cmd === 'subscribePreview')).toBe(true);
  second.receive({ type: 'snapshot', state: { cameras: { a: { name: 'A' } } }, show: { cameras: [] } });
  expect(Object.keys(c.getSnapshot().state.cameras ?? {})).toEqual(['a']);
  c.close();
});

test('binary preview messages reach the camera listener', () => {
  const c = make();
  const got: string[] = [];
  c.onPreview('cam1', (m) => got.push(m.cameraId));
  FakeWs.last.receiveBinary([1, 0, 4, 0, 99, 97, 109, 49, 0, 0, 0, 0, 0, 0, 0, 0, 1]);
  expect(got).toEqual(['cam1']);
});
