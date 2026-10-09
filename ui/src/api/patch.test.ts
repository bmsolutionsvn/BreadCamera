import { applyPatch } from './patch';

test('replace creates missing parents', () => {
  const out = applyPatch({}, [{ op: 'replace', path: '/cameras/abc/input/state', value: 'live' }]);
  expect(out).toEqual({ cameras: { abc: { input: { state: 'live' } } } });
});

test('remove deletes, and removing a missing path is a no-op', () => {
  const s = { cameras: { a: { name: 'A' }, b: { name: 'B' } } };
  const out = applyPatch(s, [
    { op: 'remove', path: '/cameras/a' },
    { op: 'remove', path: '/cameras/zzz' },
  ]);
  expect(out).toEqual({ cameras: { b: { name: 'B' } } });
  expect(s.cameras.a).toBeDefined(); // input not mutated
});

test('escaped pointer tokens', () => {
  const out = applyPatch({}, [{ op: 'replace', path: '/a~1b/c~0d', value: 1 }]);
  expect(out).toEqual({ 'a/b': { 'c~d': 1 } });
});
