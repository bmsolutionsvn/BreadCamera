import { clock, holdLeft, nextMarks } from './marks';

test('holding starts a countdown that clears when the picture returns', () => {
  let m = nextMarks(undefined, { name: 'A', phase: 'holding' }, 1000);
  expect(holdLeft(m, 5000, 1000)).toBe('00:05');
  expect(holdLeft(m, 5000, 2600)).toBe('00:04');
  m = nextMarks(m, { name: 'A', phase: 'live', input: { state: 'live', error: null, health: 'receiving' } }, 3000);
  expect(holdLeft(m, 5000, 3000)).toBeNull();
});

test('a fault that recovers on its own leaves a mark with the time', () => {
  let m = nextMarks(undefined, { name: 'A', phase: 'live', outputs: { o: { kind: 'ndi', state: 'offline', error: { code: 'x', message: 'y' } } } }, 10);
  expect(m.recoveredAt).toBeUndefined();
  m = nextMarks(m, { name: 'A', phase: 'live', outputs: { o: { kind: 'ndi', state: 'live', error: null } } }, 20);
  expect(m.recoveredAt).toBe(20);
  expect(clock(new Date(2026, 9, 8, 20, 14).getTime())).toBe('20:14');
});
