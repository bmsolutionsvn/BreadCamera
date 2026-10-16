import type { ErrorInfo } from './api/protocol';

// Codes emitted only by output adapters.
const OUTPUT_ONLY = ['output-failed', 'srt-out-failed', 'omt-failed', 'vcam-failed', 'fullscreen-failed'];
// Codes emitted by both an input adapter and the matching output adapter (`src/io/*_input.cpp`
// and `*_output.cpp`); the call site's context decides which plain headline the operator sees.
const SHARED_IO = ['ndi-failed', 'decklink-failed', 'aja-failed'];
const OUTPUT_FAMILY = [...OUTPUT_ONLY, ...SHARED_IO];

const HEADLINES: Record<string, string> = {
  'input-failed': 'The source could not be opened.',
  'input-ended': 'The source stopped sending.',
  'input-config': 'The source settings are not valid.',
  'input-retry': 'The source failed and is retrying.',
  'output-failed': 'The output failed and is retrying.',
  'output-config': 'The output settings are not valid.',
  'stage-failed': 'A processing step failed and is bypassed.',
  'stage-overrun': 'A processing step is too slow and is bypassed.',
  'matte-unavailable': 'Background removal cannot run on this PC.',
  'background-image': 'The background image cannot be read; using the colour.',
  'color-lut': 'The colour correction file cannot be read; the camera runs uncorrected.',
  'ndi-no-video': 'The NDI source sends no video.',
  'ndi-audio-rate': 'NDI audio is not 48 kHz and is ignored.',
  'ndi-format': 'The NDI source sends a video format we cannot use.',
  'decklink-no-signal': 'No signal on the DeckLink input.',
  'capture-failed': 'The camera could not be opened.',
  'breadcall-no-guest': 'No guest is in this BreadCall slot yet.',
};

// A plain headline for the operator; the engine's own message stays underneath as the detail.
// `context` lets an input call site read a shared code (ndi-failed, decklink-failed, aja-failed)
// as a source error; the default keeps the output-family reading.
export function errorText(e: ErrorInfo, context: 'input' | 'output' = 'output'): { headline: string; detail: string } {
  let code = e.code;
  if (context === 'input' && SHARED_IO.includes(code)) code = 'input-retry';
  else if (OUTPUT_FAMILY.includes(code)) code = 'output-failed';
  return { headline: HEADLINES[code] ?? 'Something went wrong.', detail: e.message ?? '' };
}