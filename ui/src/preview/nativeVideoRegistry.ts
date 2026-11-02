/// <reference types="vite/client" />
import type { EngineClient } from '../api/client';
import { onShellMessage, postToShell } from '../bridge';

// One shared registry for every mounted NativeVideo box (spec §6.1). The boxes are collected
// here, measured once per animation frame, and relayed to the shell as composition layers.

type Measured = { id: string; x: number; y: number; w: number; h: number };

const boxes = new Map<string, HTMLElement>();
const failedListeners = new Set<(id: string) => void>();
const warned = new WeakSet<HTMLElement>();

let client: EngineClient | null = null;
let reply: { pid: number; handles: Map<string, string> } | null = null;
let lastSub = '';
let frame: number | null = null;
let dirty = false;

export function nativeVideoRequested(): boolean {
  return new URLSearchParams(location.hash.slice(1)).get('nativeVideo') === '1';
}

export function onLayerFailed(fn: (id: string) => void): () => void {
  failedListeners.add(fn);
  return () => failedListeners.delete(fn);
}

export function startNativeVideo(c: EngineClient): () => void {
  client = c;
  lastSub = '';
  reply = null;
  markDirty();
  const offShell = onShellMessage((m) => {
    if (m.type === 'videoLayerFailed' && typeof m.id === 'string') for (const fn of failedListeners) fn(m.id);
  });
  // Fires for the first reply and for every reconnect resend: the new pid must reach the shell.
  const offReply = c.onSurfaces((r) => {
    reply = { pid: r.pid, handles: new Map(r.surfaces.map((s) => [s.id, s.handle])) };
    postLayers(measure());
  });
  const onVisibility = () => {
    // A hidden WebView2 page does not run animation frames, so the unsubscribe cannot wait for one.
    if (document.visibilityState !== 'hidden') {
      markDirty();
      return;
    }
    if (frame !== null) cancelAnimationFrame(frame);
    frame = null;
    dirty = true;
    flush();
  };
  document.addEventListener('visibilitychange', onVisibility);
  return () => {
    offShell();
    offReply();
    document.removeEventListener('visibilitychange', onVisibility);
    client = null;
    reply = null;
    lastSub = '';
    if (frame !== null) cancelAnimationFrame(frame);
    frame = null;
    dirty = false;
  };
}

export function register(id: string, el: HTMLElement): () => void {
  boxes.set(id, el);
  if (import.meta.env.DEV) warnOpaqueAncestors(el);
  const ro = typeof ResizeObserver === 'undefined' ? null : new ResizeObserver(markDirty);
  ro?.observe(el);
  function onMedia() {
    arm();
    markDirty();
  }
  let mq: MediaQueryList | null = null;
  function arm() {
    mq?.removeEventListener('change', onMedia);
    mq = typeof window.matchMedia === 'function' ? window.matchMedia(`(resolution: ${window.devicePixelRatio || 1}dppx)`) : null;
    mq?.addEventListener('change', onMedia);
  }
  arm();
  window.addEventListener('resize', markDirty);
  markDirty();
  return () => {
    ro?.disconnect();
    mq?.removeEventListener('change', onMedia);
    window.removeEventListener('resize', markDirty);
    if (boxes.get(id) === el) boxes.delete(id);
    markDirty();
  };
}

function markDirty() {
  dirty = true;
  if (frame === null) frame = requestAnimationFrame(flush);
}

function flush() {
  frame = null;
  if (!dirty) return;
  dirty = false;
  if (!client) return;
  const list = measure();
  const sig = list.map((m) => `${m.id}:${m.w}x${m.h}`).join('|');
  if (sig === lastSub) {
    postLayers(list);
    return;
  }
  lastSub = sig;
  client.subscribeSurfaces(list.map(({ id, w, h }) => ({ id, w, h }))).catch(() => {});
}

function measure(): Measured[] {
  if (document.visibilityState === 'hidden') return [];
  const dpr = window.devicePixelRatio || 1;
  const out: Measured[] = [];
  for (const [id, el] of boxes) {
    const r = el.getBoundingClientRect();
    const w = Math.round(r.width * dpr);
    const h = Math.round(r.height * dpr);
    if (w <= 0 || h <= 0) continue; // display:none or not laid out yet
    out.push({ id, x: Math.round(r.left * dpr), y: Math.round(r.top * dpr), w, h });
  }
  return out;
}

function postLayers(list: Measured[]) {
  if (!reply) return;
  const layers = [];
  for (const m of list) {
    const handle = reply.handles.get(m.id);
    if (handle) layers.push({ id: m.id, handle, x: m.x, y: m.y, w: m.w, h: m.h });
  }
  postToShell({ type: 'videoLayers', pid: reply.pid, layers });
}

// The video sits under the page, so an ancestor painting a background hides it (spec §6.2).
function warnOpaqueAncestors(el: HTMLElement) {
  if (warned.has(el)) return;
  warned.add(el);
  for (let p = el.parentElement; p; p = p.parentElement) {
    const cs = getComputedStyle(p);
    const bg = cs.backgroundColor;
    const clear = !bg || bg === 'transparent' || /^rgba\([^)]*,\s*0(?:\.0+)?\)$/.test(bg);
    if (!clear || (cs.backgroundImage && cs.backgroundImage !== 'none')) {
      console.warn(`NativeVideo: <${p.tagName.toLowerCase()} class="${p.className}"> paints a background behind the video; make it transparent under html.native-video (spec §6.2).`);
    }
  }
}