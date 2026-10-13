import type { ErrorInfo } from './api/protocol';

const OUTPUT = ['output-failed', 'srt-out-failed', 'ndi-failed', 'omt-failed', 'decklink-failed', 'aja-failed', 'vcam-failed', 'fullscreen-failed'];
const HEADLINES: Record<string, string> = {
  'input-failed': 'The source could not be opened.',
  'input-ended': 'The source stopped sending.',
  'input-config': 'The source settings are not valid.',
  'output-failed': 'The output failed and is retrying.',
  'output-config': 'The output settings are not valid.',
  'stage-failed': 'A processing step failed and is bypassed.',
  'stage-overrun': 'A processing step is too slow and is bypassed.',
  'matte-unavailable': 'Background removal cannot run on this PC.',
  'background-image': 'The background image cannot be read; using the colour.',
  'color-lut': 'The colour correction file cannot be read; the camera runs uncorrected.',
  'ndi-no-video': 'The NDI source sends no video.',
  'ndi-audio-rate': 'NDI audio is not 48 kHz and is ignored.',
  'decklink-no-signal': 'No signal on the DeckLink input.',
  'capture-failed': 'The camera could not be opened (USB or built-in).',
};

// A plain headline for the operator; the engine's own message stays underneath as the detail.
export function errorText(e: ErrorInfo): { headline: string; detail: string } {
  const code = OUTPUT.includes(e.code) ? 'output-failed' : e.code;
  return { headline: HEADLINES[code] ?? 'Something went wrong.', detail: e.message ?? '' };
}