import { codecFromSps } from './h264';

test('builds the avc1 codec string from the SPS', () => {
  const au = new Uint8Array([0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1e, 0xaa, 0, 0, 0, 1, 0x68, 0xce]);
  expect(codecFromSps(au)).toBe('avc1.42C01E');
});

test('returns null without an SPS', () => {
  expect(codecFromSps(new Uint8Array([0, 0, 0, 1, 0x65, 0x88]))).toBeNull();
});
