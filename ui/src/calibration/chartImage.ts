// The ColorChecker 24 layout in sRGB, for showing on a tablet (spec §5.3). Same values as src/core/chart.cpp.
const SRGB = [
  '#735244', '#c29682', '#627a9d', '#576c43', '#8580b1', '#67bdaa',
  '#d67e2c', '#505ba6', '#c15a63', '#5e3c6c', '#9dbc40', '#e0a32e',
  '#383d96', '#469449', '#af363c', '#e7c71f', '#bb5695', '#0885a1',
  '#f3f3f2', '#c8c8c8', '#a0a0a0', '#7a7a79', '#555555', '#343434',
];

// Saves breadcamera-chart.png through the WebView2 download prompt.
export function downloadChartImage() {
  const c = document.createElement('canvas');
  c.width = 2048;
  c.height = 1536;
  const ctx = c.getContext('2d');
  if (!ctx) return;
  ctx.fillStyle = '#141414';
  ctx.fillRect(0, 0, c.width, c.height);
  const cell = 300, gap = 30, x0 = (c.width - 6 * cell) / 2, y0 = (c.height - 4 * cell) / 2;
  SRGB.forEach((hex, i) => {
    ctx.fillStyle = hex;
    ctx.fillRect(x0 + (i % 6) * cell + gap / 2, y0 + Math.floor(i / 6) * cell + gap / 2, cell - gap, cell - gap);
  });
  c.toBlob((blob) => {
    if (!blob) return;
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'breadcamera-chart.png';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  });
}