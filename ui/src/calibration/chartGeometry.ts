export type Pt = [number, number];

// Chart space (u across 6 columns, v down 4 rows) → picture, through the four grid corners
// (top-left, top-right, bottom-right, bottom-left). Same maths as src/core/chart.cpp.
export function chartPoint(q: Pt[], u: number, v: number): Pt {
  const [p0, p1, p2, p3] = q;
  const dx1 = p1[0] - p2[0], dx2 = p3[0] - p2[0], dx3 = p0[0] - p1[0] + p2[0] - p3[0];
  const dy1 = p1[1] - p2[1], dy2 = p3[1] - p2[1], dy3 = p0[1] - p1[1] + p2[1] - p3[1];
  const den = dx1 * dy2 - dx2 * dy1 || 1e-12;
  const g = (dx3 * dy2 - dx2 * dy3) / den;
  const h = (dx1 * dy3 - dx3 * dy1) / den;
  const a = p1[0] - p0[0] + g * p1[0], b = p3[0] - p0[0] + h * p3[0];
  const d = p1[1] - p0[1] + g * p1[1], e = p3[1] - p0[1] + h * p3[1];
  const w = g * u + h * v + 1;
  return [(a * u + b * v + p0[0]) / w, (d * u + e * v + p0[1]) / w];
}

export const DEFAULT_CORNERS: Pt[] = [
  [0.25, 0.22],
  [0.75, 0.22],
  [0.75, 0.78],
  [0.25, 0.78],
];