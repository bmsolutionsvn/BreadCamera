import { act, render, screen, waitFor } from '@testing-library/react';
import { useEffect } from 'react';

import { EngineClient } from '../api/client';
import { Workspace } from '../App';
import { FakeWs } from '../test/fakeWs';
import { NativeVideo } from './NativeVideo';
import { startNativeVideo } from './nativeVideoRegistry';

function makeClient() {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  return c;
}

let shell: ((e: { data: unknown }) => void) | null = null;
function shellStub() {
  const post = vi.fn();
  shell = null;
  (window as unknown as { chrome: unknown }).chrome = {
    webview: {
      postMessage: post,
      addEventListener: (_t: string, fn: (e: { data: unknown }) => void) => (shell = fn),
      removeEventListener: () => (shell = null),
    },
  };
  return post;
}

// The page's boxes measured 200x100 CSS px at (10, 20); `.zero` stands in for display:none.
let left = 10;
let top = 20;
const width = 200;
const height = 100;

beforeEach(() => {
  left = 10;
  top = 20;
  Object.defineProperty(window, 'devicePixelRatio', { configurable: true, value: 1.5 });
  vi.spyOn(Element.prototype, 'getBoundingClientRect').mockImplementation(function (this: Element) {
    const zero = this.parentElement?.classList.contains('zero') ?? false;
    const w = zero ? 0 : width;
    const h = zero ? 0 : height;
    return { x: left, y: top, left, top, width: w, height: h, right: left + w, bottom: top + h, toJSON: () => ({}) } as DOMRect;
  });
});

afterEach(() => {
  delete (window as unknown as { chrome?: unknown }).chrome;
  delete (document as unknown as { visibilityState?: string }).visibilityState;
  location.hash = '';
  Object.defineProperty(window, 'devicePixelRatio', { configurable: true, value: 1 });
  vi.restoreAllMocks();
});

function Harness({ client, native, ids }: { client: EngineClient; native: boolean; ids: string[] }) {
  useEffect(() => startNativeVideo(client), [client]);
  return (
    <div>
      {ids.map((id) => (
        <div key={id} className={id === 'cam:B' ? 'zero' : 'wide'}>
          <NativeVideo id={id} native={native} fallback={<span>fallback {id}</span>} />
        </div>
      ))}
    </div>
  );
}

const sent = () => FakeWs.last.sent.map((s) => JSON.parse(s) as { id: number; cmd: string; args: Record<string, unknown> });
const surfaceReqs = () => sent().filter((m) => m.cmd === 'subscribeSurfaces');
const layers = (post: ReturnType<typeof vi.fn>, at = -1) => JSON.parse(post.mock.calls.at(at)![0] as string);

test('renders the fallback when native is false', () => {
  const post = shellStub();
  render(<Harness client={makeClient()} native={false} ids={['cam:A']} />);
  expect(screen.getByText('fallback cam:A')).toBeInTheDocument();
  expect(surfaceReqs()).toHaveLength(0);
  expect(post).not.toHaveBeenCalled();
});

test('subscribes with device-pixel sizes', async () => {
  shellStub();
  render(<Harness client={makeClient()} native ids={['cam:A']} />);
  await waitFor(() => expect(surfaceReqs()).toHaveLength(1));
  expect(surfaceReqs()[0].args).toEqual({ surfaces: [{ id: 'cam:A', w: 300, h: 150 }] });
});

test('relays the reply to the shell', async () => {
  const post = shellStub();
  render(<Harness client={makeClient()} native ids={['cam:A']} />);
  await waitFor(() => expect(surfaceReqs()).toHaveLength(1));
  act(() => FakeWs.last.receive({ id: surfaceReqs()[0].id, ok: true, result: { pid: 42, surfaces: [{ id: 'cam:A', handle: '99', w: 300, h: 150 }] } }));
  await waitFor(() => expect(post).toHaveBeenCalled());
  expect(layers(post)).toEqual({ type: 'videoLayers', pid: 42, layers: [{ id: 'cam:A', handle: '99', x: 15, y: 30, w: 300, h: 150 }] });
});

test('a position-only change re-posts layers without a new subscription', async () => {
  const post = shellStub();
  render(<Harness client={makeClient()} native ids={['cam:A']} />);
  await waitFor(() => expect(surfaceReqs()).toHaveLength(1));
  act(() => FakeWs.last.receive({ id: surfaceReqs()[0].id, ok: true, result: { pid: 42, surfaces: [{ id: 'cam:A', handle: '99', w: 300, h: 150 }] } }));
  await waitFor(() => expect(post).toHaveBeenCalled());
  left = 30;
  act(() => window.dispatchEvent(new Event('resize')));
  await waitFor(() => expect(layers(post).layers[0].x).toBe(45));
  expect(surfaceReqs()).toHaveLength(1);
});

test('a 0x0 box is left out and visibility hidden sends an empty subscription', async () => {
  shellStub();
  render(<Harness client={makeClient()} native ids={['cam:A', 'cam:B']} />);
  await waitFor(() => expect(surfaceReqs()).toHaveLength(1));
  expect(surfaceReqs()[0].args).toEqual({ surfaces: [{ id: 'cam:A', w: 300, h: 150 }] });
  act(() => FakeWs.last.receive({ id: surfaceReqs()[0].id, ok: true, result: { pid: 42, surfaces: [{ id: 'cam:A', handle: '99', w: 300, h: 150 }] } }));
  Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => 'hidden' });
  act(() => document.dispatchEvent(new Event('visibilitychange')));
  await waitFor(() => expect(surfaceReqs()).toHaveLength(2));
  expect(surfaceReqs()[1].args).toEqual({ surfaces: [] });
});

test('videoLayerFailed shows the no-picture placeholder for that id', async () => {
  shellStub();
  const { container } = render(<Harness client={makeClient()} native ids={['cam:A']} />);
  await waitFor(() => expect(container.querySelector('.nv-box')).not.toBeNull());
  act(() => shell?.({ data: { type: 'videoLayerFailed', id: 'cam:A' } }));
  expect(container.querySelector('.nv-lost')).not.toBeNull();
  expect(screen.queryByText(/fallback/)).toBeNull();
});

test('hiding sends an empty subscription without waiting for a frame', async () => {
  shellStub();
  render(<Harness client={makeClient()} native ids={['cam:A']} />);
  await waitFor(() => expect(surfaceReqs()).toHaveLength(1));
  act(() => FakeWs.last.receive({ id: surfaceReqs()[0].id, ok: true, result: { pid: 42, surfaces: [{ id: 'cam:A', handle: '99', w: 300, h: 150 }] } }));
  // A hidden WebView2 page does not run animation frames.
  vi.spyOn(window, 'requestAnimationFrame').mockReturnValue(1);
  Object.defineProperty(document, 'visibilityState', { configurable: true, get: () => 'hidden' });
  act(() => document.dispatchEvent(new Event('visibilitychange')));
  expect(surfaceReqs()).toHaveLength(2);
  expect(surfaceReqs()[1].args).toEqual({ surfaces: [] });
});

test('the broken-presenter warning shows the no-picture placeholder on every box', async () => {
  shellStub();
  location.hash = '#nativeVideo=1';
  const client = makeClient();
  FakeWs.last.receive({
    type: 'snapshot',
    state: { engine: { nativeVideo: true, warnings: { nativeVideo: 'a preview stopped presenting' } }, cameras: { a: { name: 'A' } } },
    show: { cameras: [{ id: 'a', name: 'A', input: { kind: 'srt', params: {} }, outputs: [] }] },
  });
  const { container } = render(<Workspace client={client} />);
  await waitFor(() => expect(container.querySelector('.nv-box.nv-lost')).not.toBeNull());
});