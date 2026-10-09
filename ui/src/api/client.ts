import { applyPatch } from './patch';
import {
  parsePreviewMessage,
  type ErrorInfo,
  type PreviewMessage,
  type PreviewSize,
  type ShowJson,
  type StateTree,
  type Stats,
} from './protocol';

export class EngineError extends Error {
  constructor(public code: string, message: string) {
    super(message);
  }
}

export interface EngineSnapshot {
  status: 'connecting' | 'open' | 'closed';
  state: StateTree;
  show: ShowJson | null;
  stats: Stats | null;
}

type Pending = { resolve: (v: unknown) => void; reject: (e: Error) => void };

// One WebSocket to the engine; reconnects every second while the engine restarts.
export class EngineClient {
  private ws: WebSocket | null = null;
  private nextId = 1;
  private pending = new Map<number, Pending>();
  private listeners = new Set<() => void>();
  private previewListeners = new Map<string, Set<(m: PreviewMessage) => void>>();
  private previewSub: { ids: string[]; size: PreviewSize } | null = null;
  private closed = false;
  private retry: ReturnType<typeof setTimeout> | undefined;
  private snap: EngineSnapshot = { status: 'connecting', state: {}, show: null, stats: null };

  constructor(
    private url: string,
    private wsFactory: (url: string) => WebSocket = (u) => new WebSocket(u),
  ) {}

  connect() {
    const ws = this.wsFactory(this.url);
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => {
      this.update({ status: 'open' });
      if (this.previewSub) this.send('subscribePreview', { cameraIds: this.previewSub.ids, size: this.previewSub.size });
    };
    ws.onmessage = (e) => this.onMessage(e.data);
    ws.onclose = () => {
      for (const p of this.pending.values()) p.reject(new Error('disconnected from the engine'));
      this.pending.clear();
      this.update({ status: 'closed' });
      if (!this.closed) this.retry = setTimeout(() => this.connect(), 1000);
    };
    this.ws = ws;
  }

  close() {
    this.closed = true;
    clearTimeout(this.retry);
    this.ws?.close();
  }

  getSnapshot = (): EngineSnapshot => this.snap;

  subscribe = (fn: () => void) => {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  };

  onPreview(cameraId: string, fn: (m: PreviewMessage) => void) {
    let set = this.previewListeners.get(cameraId);
    if (!set) this.previewListeners.set(cameraId, (set = new Set()));
    set.add(fn);
    return () => set!.delete(fn);
  }

  setPreviewSubscription(ids: string[], size: PreviewSize) {
    this.previewSub = { ids, size };
    if (this.ws?.readyState === 1) this.send('subscribePreview', { cameraIds: ids, size });
  }

  request<T = unknown>(cmd: string, args: Record<string, unknown> = {}): Promise<T> {
    if (!this.ws || this.ws.readyState !== 1) return Promise.reject(new Error('not connected to the engine'));
    const id = this.send(cmd, args);
    return new Promise<T>((resolve, reject) => this.pending.set(id, { resolve: resolve as (v: unknown) => void, reject }));
  }

  private send(cmd: string, args: Record<string, unknown>) {
    const id = this.nextId++;
    this.ws!.send(JSON.stringify({ id, cmd, args }));
    return id;
  }

  private update(part: Partial<EngineSnapshot>) {
    this.snap = { ...this.snap, ...part };
    for (const fn of this.listeners) fn();
  }

  private onMessage(data: unknown) {
    if (data instanceof ArrayBuffer) {
      let m: PreviewMessage;
      try {
        m = parsePreviewMessage(data);
      } catch {
        return;
      }
      for (const fn of this.previewListeners.get(m.cameraId) ?? []) fn(m);
      return;
    }
    let msg: Record<string, unknown>;
    try {
      msg = JSON.parse(String(data));
    } catch {
      return;
    }
    switch (msg.type) {
      case 'snapshot':
        this.update({ state: msg.state as StateTree, show: msg.show as ShowJson });
        return;
      case 'patch':
        this.update({ state: applyPatch(this.snap.state, msg.ops as never) });
        return;
      case 'show':
        this.update({ show: msg.show as ShowJson });
        return;
      case 'stats':
        this.update({ stats: msg as unknown as Stats });
        return;
    }
    const p = typeof msg.id === 'number' ? this.pending.get(msg.id) : undefined;
    if (!p) return;
    this.pending.delete(msg.id as number);
    if (msg.ok) p.resolve(msg.result);
    else {
      const e = msg.error as ErrorInfo;
      p.reject(new EngineError(e?.code ?? 'unknown', e?.message ?? 'unknown error'));
    }
  }
}
