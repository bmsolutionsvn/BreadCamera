import { useEffect, useState } from 'react';

import type { EngineClient } from '../api/client';
import type { StageSchemaJson } from '../api/protocol';
import { useEngine } from '../api/useEngine';

// The engine's stage schemas in pipeline order (spec §5.4); fetched again after every reconnect.
export function useStageSchemas(client: EngineClient): StageSchemaJson[] {
  const { status } = useEngine(client);
  const [schemas, setSchemas] = useState<StageSchemaJson[]>([]);
  useEffect(() => {
    if (status !== 'open') return;
    let live = true;
    client
      .request<StageSchemaJson[]>('getStageSchemas')
      .then((s) => live && setSchemas(s))
      .catch(() => {});
    return () => {
      live = false;
    };
  }, [client, status]);
  return schemas;
}
