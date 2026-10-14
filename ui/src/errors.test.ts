import { errorText } from './errors';

// Every error code the engine emits as an ErrorInfo (src/, verified by grep for ErrorInfo{"code").
// Keep this list in step with the engine: an unmapped code must fail the test below.
const ENGINE_CODES = [
  // src/io/*_input.cpp
  'input-failed',
  'input-ended',
  'input-config',
  'ndi-failed',
  'ndi-format',
  'ndi-no-video',
  'ndi-audio-rate',
  'decklink-failed',
  'decklink-no-signal',
  'aja-failed',
  'capture-failed',
  'breadcall-no-guest',
  // src/io/*_output.h (errorCode()) and src/engine/camera.cpp
  'output-failed',
  'srt-out-failed',
  'omt-failed',
  'vcam-failed',
  'fullscreen-failed',
  'output-config',
  // stages and engine advisories
  'stage-failed',
  'stage-overrun',
  'matte-unavailable',
  'background-image',
  'color-lut',
];

test('known codes get a headline and keep the engine detail', () => {
  const r = errorText({ code: 'input-failed', message: 'cannot open srt://1.2.3.4:9000: Connection refused' });
  expect(r.headline).toBe('The source could not be opened.');
  expect(r.detail).toContain('srt://1.2.3.4:9000');
  expect(errorText({ code: 'ndi-failed', message: 'x' }).headline).toBe('The output failed and is retrying.');
  expect(errorText({ code: 'breadcall-no-guest', message: 'no guest in slot 2' }).headline).toBe('No guest is in this BreadCall slot yet.');
});

test('every engine error code gets a plain headline', () => {
  const unmapped = ENGINE_CODES.filter((code) => errorText({ code, message: 'x' }).headline === 'Something went wrong.');
  expect(unmapped).toEqual([]);
});

test('the input context reads a shared code as a source error', () => {
  expect(errorText({ code: 'ndi-failed', message: 'x' }, 'input').headline).toBe('The source failed and is retrying.');
  expect(errorText({ code: 'decklink-failed', message: 'x' }, 'input').headline).toBe('The source failed and is retrying.');
  expect(errorText({ code: 'aja-failed', message: 'x' }, 'input').headline).toBe('The source failed and is retrying.');
  // ndi-format is an input-only code and keeps its own headline either way.
  expect(errorText({ code: 'ndi-format', message: 'x' }, 'input').headline).toBe('The NDI source sends a video format we cannot use.');
  expect(errorText({ code: 'ndi-format', message: 'x' }).headline).toBe('The NDI source sends a video format we cannot use.');
  // The default context keeps the output-family reading.
  expect(errorText({ code: 'ndi-failed', message: 'x' }, 'output').headline).toBe('The output failed and is retrying.');
});

test('unknown codes fall back without leaking undefined', () => {
  const r = errorText({ code: 'brand-new-code', message: 'detail' });
  expect(r.headline).toBe('Something went wrong.');
  expect(r.detail).toBe('detail');
  expect(JSON.stringify(r)).not.toContain('undefined');
});