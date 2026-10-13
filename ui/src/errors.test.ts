import { errorText } from './errors';

test('known codes get a headline and keep the engine detail', () => {
  const r = errorText({ code: 'input-failed', message: 'cannot open srt://1.2.3.4:9000: Connection refused' });
  expect(r.headline).toBe('The source could not be opened.');
  expect(r.detail).toContain('srt://1.2.3.4:9000');
  expect(errorText({ code: 'ndi-failed', message: 'x' }).headline).toBe('The output failed and is retrying.');
});

test('unknown codes fall back without leaking undefined', () => {
  const r = errorText({ code: 'brand-new-code', message: 'detail' });
  expect(r.headline).toBe('Something went wrong.');
  expect(r.detail).toBe('detail');
  expect(JSON.stringify(r)).not.toContain('undefined');
});