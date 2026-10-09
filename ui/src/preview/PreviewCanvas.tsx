import { useEffect, useRef } from 'react';

import type { EngineClient } from '../api/client';
import type { PreviewSize } from '../api/protocol';
import { codecFromSps } from './h264';

// Decodes the engine's H.264 preview with WebCodecs (hardware decode in WebView2).
export function PreviewCanvas({ client, cameraId, size = 'small' }: { client: EngineClient; cameraId: string; size?: PreviewSize }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    if (typeof VideoDecoder === 'undefined') return; // jsdom / very old runtime
    let configured = false;
    const decoder = new VideoDecoder({
      output: (frame) => {
        const c = canvas.current;
        if (c) {
          c.width = frame.displayWidth;
          c.height = frame.displayHeight;
          c.getContext('2d')?.drawImage(frame, 0, 0);
        }
        frame.close();
      },
      error: () => {
        configured = false;
        client.request('requestKeyframe', { cameraId, size }).catch(() => {});
      },
    });
    const off = client.onPreview(cameraId, (m) => {
      if (!configured) {
        if (!m.keyframe) return;
        const codec = codecFromSps(m.data);
        if (!codec) return;
        decoder.configure({ codec, optimizeForLatency: true });
        configured = true;
      }
      if (decoder.state !== 'configured') return;
      decoder.decode(new EncodedVideoChunk({ type: m.keyframe ? 'key' : 'delta', timestamp: m.ptsUs, data: m.data }));
    });
    return () => {
      off();
      if (decoder.state !== 'closed') decoder.close();
    };
  }, [client, cameraId, size]);
  return <canvas ref={canvas} aria-hidden="true" />;
}
