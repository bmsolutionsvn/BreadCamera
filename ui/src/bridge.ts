interface WebViewBridge {
  postMessage(m: unknown): void;
  addEventListener(type: 'message', fn: (e: { data: unknown }) => void): void;
  removeEventListener(type: 'message', fn: (e: { data: unknown }) => void): void;
}
const bridge = (): WebViewBridge | undefined => (window as unknown as { chrome?: { webview?: WebViewBridge } }).chrome?.webview;

// The page sends strings (the shell reads them with TryGetWebMessageAsString).
export function postToShell(msg: Record<string, unknown>) {
  bridge()?.postMessage(JSON.stringify(msg));
}

// The shell sends JSON (PostWebMessageAsJson), so e.data is already an object.
export function onShellMessage(fn: (m: { type?: string; [k: string]: unknown }) => void): () => void {
  const b = bridge();
  if (!b) return () => {};
  const h = (e: { data: unknown }) => fn((e.data ?? {}) as { type?: string });
  b.addEventListener('message', h);
  return () => b.removeEventListener('message', h);
}
