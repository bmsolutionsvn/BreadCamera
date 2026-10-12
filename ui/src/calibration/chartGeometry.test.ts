import { chartPoint } from './chartGeometry';

const q: [number, number][] = [
  [0.3, 0.25],
  [0.72, 0.22],
  [0.74, 0.7],
  [0.28, 0.72],
];

test('chart space maps to the four corners', () => {
  expect(chartPoint(q, 0, 0)).toEqual([0.3, 0.25]);
  const [x, y] = chartPoint(q, 1, 1);
  expect(x).toBeCloseTo(0.74, 6);
  expect(y).toBeCloseTo(0.7, 6);
});