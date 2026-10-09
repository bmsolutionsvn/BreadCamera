import { parsePreviewMessage } from './protocol';

test('parses the preview header written by the engine', () => {
  const bytes = [1, 1, 4, 0, 99, 97, 109, 49, 0xd2, 0x04, 0, 0, 0, 0, 0, 0, 9, 9];
  const m = parsePreviewMessage(new Uint8Array(bytes).buffer);
  expect(m.cameraId).toBe('cam1');
  expect(m.keyframe).toBe(true);
  expect(m.ptsUs).toBe(1234);
  expect(Array.from(m.data)).toEqual([9, 9]);
});

test('rejects unknown versions and short buffers', () => {
  expect(() => parsePreviewMessage(new Uint8Array([2, 0, 0, 0]).buffer)).toThrow();
  expect(() => parsePreviewMessage(new Uint8Array([1, 0, 9, 0, 1]).buffer)).toThrow();
});
