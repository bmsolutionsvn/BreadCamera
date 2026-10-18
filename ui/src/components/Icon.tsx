const PATHS = {
  matte: '<path d="M8 8.2a2.4 2.4 0 1 0 0-4.8 2.4 2.4 0 0 0 0 4.8Z"/><path d="M3.5 13.5c.6-2.3 2.3-3.6 4.5-3.6s3.9 1.3 4.5 3.6"/><path d="M1.5 4V2.5a1 1 0 0 1 1-1H4M12 1.5h1.5a1 1 0 0 1 1 1V4M14.5 12v1.5a1 1 0 0 1-1 1H12M4 14.5H2.5a1 1 0 0 1-1-1V12"/>',
  cleanup: '<path d="M8 2.5 9.4 6.6 13.5 8 9.4 9.4 8 13.5 6.6 9.4 2.5 8 6.6 6.6Z"/>',
  upscale: '<path d="M9.5 2H14v4.5M6.5 14H2V9.5"/><path d="M14 2 9 7M2 14l5-5"/>',
  bg: '<rect x="1.5" y="3" width="13" height="10" rx="1.5"/><circle cx="5.5" cy="6.5" r="1.2"/><path d="m14.5 10.5-3.5-3.5-6 6"/>',
  color: '<circle cx="8" cy="8" r="6.5"/><path d="M8 1.5v13"/><path d="M8 4.5h4.6M8 8h6.5M8 11.5h4.6"/>',
  arrow: '<path d="M2.5 8h11M9.5 4l4 4-4 4"/>',
  seen: '<path d="M2.5 8a5.5 5.5 0 1 0 1.6-3.9"/><path d="M2.5 2.5v2.8h2.8"/><path d="M8 5v3.2l2 1.3"/>',
  chart: '<rect x="1.5" y="3" width="13" height="10" rx="1"/><path d="M5.8 3v10M10.2 3v10M1.5 8h13"/>',
  settings: '<path d="M2 4.5h7M12 4.5h2M2 11.5h2M7 11.5h7"/><circle cx="10.5" cy="4.5" r="1.5"/><circle cx="5.5" cy="11.5" r="1.5"/>',
  plus: '<path d="M8 3v10M3 8h10"/>',
  chev: '<path d="m6 4 4 4-4 4"/>',
  warn: '<path d="M8 2 1.5 13.5h13L8 2Z"/><path d="M8 6.5v3M8 11.6v.1"/>',
  err: '<circle cx="8" cy="8" r="6.5"/><path d="M8 4.8v3.6M8 10.9v.1"/>',
  retry: '<path d="M13.5 8a5.5 5.5 0 1 1-1.6-3.9"/><path d="M13.5 2.5v2.8h-2.8"/>',
  check: '<path d="m3.5 8.5 3 3 6-7"/>',
  cam: '<rect x="1.5" y="4" width="9.5" height="8" rx="1.5"/><path d="m11 7 3.5-2v6L11 9"/>',
  power: '<path d="M8 1.5v6"/><path d="M4.4 4a5.5 5.5 0 1 0 7.2 0"/>',
  trash: '<path d="M2.5 4h11M6 4V2.5h4V4M4 4l.7 9.5h6.6L12 4"/>',
  min: '<path d="M3 8h10"/>',
  max: '<rect x="3.5" y="3.5" width="9" height="9"/>',
  restore: '<rect x="3.5" y="5.5" width="7" height="7"/><path d="M5.5 5.5v-2h7v7h-2"/>',
  close: '<path d="m3.5 3.5 9 9M12.5 3.5l-9 9"/>',
} as const;
export type IconName = keyof typeof PATHS;

// One stroke weight for every icon (craft floor: no glyphs standing in for icons).
export function Icon({ name, size = 16 }: { name: IconName; size?: number }) {
  return <svg className="i" width={size} height={size} viewBox="0 0 16 16" aria-hidden="true" dangerouslySetInnerHTML={{ __html: PATHS[name] }} />;
}
