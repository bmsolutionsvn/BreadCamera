import { useSyncExternalStore } from 'react';
import { flushSync } from 'react-dom';

import type { EngineClient, EngineSnapshot } from './client';

// Engine messages must land in the DOM in the same turn they arrive, so the operator sees
// the state change now instead of at the next scheduler tick.
export function useEngine(client: EngineClient): EngineSnapshot {
  return useSyncExternalStore(
    (onChange) => client.subscribe(() => flushSync(onChange)),
    client.getSnapshot,
  );
}
