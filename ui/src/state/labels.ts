// Short codes on the strip; full names in tooltips (docs/design/multiview.md).
const IN: Record<string, [string, string]> = {
  srt: ['SRT', 'SRT input'],
  ndi: ['NDI', 'NDI input'],
  decklink: ['SDI', 'DeckLink input'],
  aja: ['AJA', 'AJA input'],
  uvc: ['USB', 'USB or built-in camera'],
};
const OUT: Record<string, [string, string]> = {
  ndi: ['NDI', 'NDI output'],
  fullscreen: ['FS', 'Fullscreen'],
  srt: ['SRT', 'SRT output'],
  omt: ['OMT', 'OMT output'],
  vcam: ['VCAM', 'Virtual camera'],
  decklink: ['SDI', 'SDI output'],
  aja: ['SDI', 'AJA SDI output'],
};
export const inputCode = (k: string) => IN[k]?.[0] ?? k.toUpperCase();
export const inputName = (k: string) => IN[k]?.[1] ?? `${k} input`;
export const outputCode = (k: string) => OUT[k]?.[0] ?? k.toUpperCase();
export const outputName = (k: string) => OUT[k]?.[1] ?? `${k} output`;

const RATES: Record<string, string> = { '50/1': '50', '60000/1001': '59.94', '60/1': '60' };
export const rateLabel = (r: string | undefined) => (r ? RATES[r] ?? r : '—');
export const sizeLabel = (r: string | undefined) => (r ? r.replace('x', '×') : '—');
export const signalLabel = { ready: 'Ready', connecting: 'Connecting', live: 'Live', offline: 'Offline' } as const;
