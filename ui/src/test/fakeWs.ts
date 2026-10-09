// Minimal stand-in for the browser WebSocket used by EngineClient tests.
export class FakeWs {
  static all: FakeWs[] = [];
  static get last(): FakeWs {
    return FakeWs.all[FakeWs.all.length - 1];
  }
  url: string;
  binaryType = 'blob';
  readyState = 0;
  sent: string[] = [];
  onopen: ((e: unknown) => void) | null = null;
  onmessage: ((e: { data: unknown }) => void) | null = null;
  onclose: ((e: unknown) => void) | null = null;
  constructor(url: string) {
    this.url = url;
    FakeWs.all.push(this);
  }
  send(d: string) {
    this.sent.push(d);
  }
  close() {
    this.readyState = 3;
    this.onclose?.({});
  }
  open() {
    this.readyState = 1;
    this.onopen?.({});
  }
  receive(obj: unknown) {
    this.onmessage?.({ data: JSON.stringify(obj) });
  }
  receiveBinary(bytes: number[]) {
    this.onmessage?.({ data: new Uint8Array(bytes).buffer });
  }
  lastRequest(): { id: number; cmd: string; args: Record<string, unknown> } {
    return JSON.parse(this.sent[this.sent.length - 1]);
  }
}
