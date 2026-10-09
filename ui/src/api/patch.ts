import type { PatchOp } from './protocol';

// Our protocol: "replace" creates missing parents; removing a missing path is a no-op.
export function applyPatch<T extends object>(state: T, ops: PatchOp[]): T {
  const out = structuredClone(state) as Record<string, unknown>;
  for (const op of ops) {
    const keys = op.path.split('/').slice(1).map((k) => k.replace(/~1/g, '/').replace(/~0/g, '~'));
    if (keys.length === 0) continue;
    let node = out;
    let ok = true;
    for (const k of keys.slice(0, -1)) {
      const next = node[k];
      if (typeof next !== 'object' || next === null) {
        if (op.op === 'remove') {
          ok = false;
          break;
        }
        node[k] = {};
      }
      node = node[k] as Record<string, unknown>;
    }
    if (!ok) continue;
    const last = keys[keys.length - 1];
    if (op.op === 'replace') node[last] = op.value;
    else delete node[last];
  }
  return out as T;
}
