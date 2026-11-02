import { useEffect, useRef, useState, type ReactNode } from 'react';

import { onLayerFailed, register } from './nativeVideoRegistry';

// Native mode: the shell paints the engine's surface under this empty box. Fallback: the H.264
// canvas. A failed layer gets the same black no-picture cell the tile shows today (spec §6.1).
export function NativeVideo({ id, native, presenterLost = false, fallback }: { id: string; native: boolean; presenterLost?: boolean; fallback: ReactNode }) {
  const box = useRef<HTMLDivElement>(null);
  const [failed, setFailed] = useState(false);
  useEffect(() => setFailed(false), [id]);
  useEffect(() => {
    if (!native || !box.current) return;
    return register(id, box.current);
  }, [id, native]);
  useEffect(() => {
    if (!native) return;
    return onLayerFailed((bad) => {
      if (bad === id) setFailed(true);
    });
  }, [id, native]);
  if (!native) return <>{fallback}</>;
  return <div ref={box} className={`nv-box${failed || presenterLost ? ' nv-lost' : ''}`} aria-hidden="true" />;
}