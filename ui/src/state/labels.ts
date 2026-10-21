// Short codes on the strip; full names in tooltips (docs/design/multiview.md).
const IN: Record<string, [string, string]> = {
  srt: ['SRT', 'SRT input'],
  ndi: ['NDI', 'NDI input'],
  decklink: ['SDI', 'DeckLink input'],
  aja: ['AJA', 'AJA input'],
  uvc: ['CAM', 'Device camera'],
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

// The Project frame rate values (spec §1.3): core rates plus HFR 119.88 and 120 only.
export const RATE_OPTIONS = [
  ['15/1', '15'],
  ['24000/1001', '23.976'],
  ['24/1', '24'],
  ['25/1', '25'],
  ['30000/1001', '29.97'],
  ['30/1', '30'],
  ['50/1', '50'],
  ['60000/1001', '59.94'],
  ['60/1', '60'],
  ['120000/1001', '119.88'],
  ['120/1', '120'],
] as const;
const RATES: Record<string, string> = Object.fromEntries(RATE_OPTIONS);
export const rateLabel = (r: string | undefined) => (r ? RATES[r] ?? r : '—');
export const sizeLabel = (r: string | undefined) => (r ? r.replace('x', '×') : '—');
export const signalLabel = { ready: 'Ready', connecting: 'Connecting', live: 'Live', offline: 'Offline' } as const;
