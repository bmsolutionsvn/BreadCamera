# BreadCamera M5 — Hardening: settings, Vietnamese errors, soak and fault runs, installer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make BreadCamera shippable: a Settings screen (show rate, hold time, slate, language, GPU/backend info), Vietnamese-first error text for every engine error code, a soak + fault-injection tool that proves spec §8.3/§8.4 against the real executables, and an Inno Setup installer that also registers the virtual camera and ships the licence notices.

**Architecture:** UI work stays in `ui/`; error text is a pure mapping from the engine's stable `{code, message}` to localised copy (the English engine message is kept as detail). The soak tool `bc_soak.exe` drives real `breadcamera-engine.exe` processes over the control API with SRT test streams generated in-process, samples stats and process memory, injects faults on a schedule, and writes a CSV + verdict. The installer is an Inno Setup script built by `ISCC.exe`.

**Tech Stack:** as before; Inno Setup 6 (installed with `winget install JRSoftware.InnoSetup` if missing).

**Spec:** `docs/superpowers/specs/2026-10-08-breadcamera-design.md` (§6.4, §8.3, §8.4, §7 installer, §7.1 licences).

**Branch:** `bcam-m5`, cut from `bcam-m4b`.

## Global Constraints

- All earlier Global Constraints apply.
- Glossary is binding (UX-CONTRACT): Sẵn sàng / Đang kết nối / Đang phát / Tắt; Vietnamese is the source, English the translation. No hard-coded user-facing string outside `ui/src/i18n.ts`.
- Errors stay persistent blocks; the block shows the localised sentence first and the engine's detail second.
- Soak pass criteria (spec §8.3): 4× 1080p60 SRT, all effects on (matte via the active backend or bypassed with an error on PCs without one — say which), every buildable output type active, **zero tick overruns**, **flat memory** (working set at the end ≤ 105 % of the 10-minute mark), **latency ≤ 50 ms** on every camera.
- Fault criteria (spec §8.4): input killed → hold then slate then live again on return; one output receiver killed → others unaffected; stage forced to fail → bypassed for that camera only; engine killed → back with its show in < 3 s.
- The installer installs per machine (admin), puts the app in `%ProgramFiles%\BreadMedia\BreadCamera`, registers `breadcamera-vcam.dll` only on Windows 11 (build ≥ 22000), and removes the registration on uninstall. It never deletes `%APPDATA%\BreadCamera` (shows are user data).

## Review Focus

1. **A setting that would disrupt a live show** (rate change while cameras are live) — the UI asks for confirmation with a verb button ("Đổi tốc độ khung hình") and explains that every output restarts — owned by Task 1.
2. **An engine error code the UI has never seen** — falls back to a generic Vietnamese sentence plus the English detail, never `undefined` or a raw code alone — owned by Task 2.
3. **Hard-coded strings sneaking back into components** — a test fails the build — owned by Task 2.
4. **The soak tool itself failing silently** (engine never started, pushers never connected) — the tool exits non-zero with a reason; a run with zero samples is a failure, not a pass — owned by Task 3.
5. **Installing over a running BreadCamera** — the installer closes the shell/engine first (asks the user), or refuses with a clear message — owned by Task 4.

## File Structure

```
ui/src/screens/Settings.tsx, ui/src/errors.ts, ui/src/i18n.ts (language at runtime)
ui/src/test/strings.test.ts
tools/soak/soak_main.cpp            bc_soak.exe
installer/BreadCamera.iss, installer/THIRD_PARTY_NOTICES.md, installer/LGPL-source-offer.txt
scripts/package.ps1                 build release + UI + installer
docs/superpowers/manual-checks/m5-fresh-install.md
```

## Execution notes for the controller

- 5 tasks → batched reviews (1–3, 4–5).
- The full 8-hour soak does **not** run inside this plan: Task 3 runs a 30-minute soak as evidence and leaves `scripts/soak.ps1 -Hours 8` for a separate night (the planner queues it).

---

### Task 1: Settings screen and runtime language

**Files:**
- Create: `ui/src/screens/Settings.tsx`
- Modify: `ui/src/i18n.ts` (language from `localStorage`, `setLang`), `ui/src/App.tsx` (nav), `ui/src/api/protocol.ts` (`ShowJson.settings` already typed)
- Test: `ui/src/screens/Settings.test.tsx`, `ui/src/i18n.test.ts`

**Interfaces:**
- Consumes: `setSettings {rate?, holdMs?, slatePath?}` (M1), state `/engine/gpu`, `/engine/matteBackend`, `/engine/previewEncoder`, `/engine/warnings/*`.
- Produces: `export function setLang(l: 'vi' | 'en'): void` (stores `bc.lang` in `localStorage`, reloads), `lang` read from `localStorage` (try/catch → `'vi'`), `?lang=en` still honoured for one session; `Settings({client})`.

Copy (vi / en): nav "Cài đặt" / "Settings"; "Tốc độ khung hình" / "Frame rate" with options "50", "59,94", "60" (vi uses a decimal comma) / "50", "59.94", "60"; "Giữ khung cuối khi mất tín hiệu (giây)" / "Hold the last frame on signal loss (seconds)"; "Ảnh màn chờ" / "Slate image" with hint "Để trống để dùng màn chờ mặc định." / "Leave empty for the built-in slate."; "Ngôn ngữ" / "Language" ("Tiếng Việt", "English"); "GPU", "Tách nền" / "Background removal" (backend name or "Không có" / "None"), "Bộ mã hoá xem trước" / "Preview encoder"; save button "Lưu cài đặt" / "Save settings"; confirm for a rate change: title "Đổi tốc độ khung hình?" / "Change the frame rate?", body "Mọi đầu ra sẽ khởi động lại và mất hình trong khoảng 1 giây." / "Every output restarts and loses picture for about 1 second.", primary "Đổi tốc độ khung hình" / "Change frame rate".

- [ ] **Step 1: Tests**

`ui/src/i18n.test.ts`:
```ts
test('language comes from localStorage, defaults to Vietnamese', async () => {
  localStorage.removeItem('bc.lang');
  vi.resetModules();
  expect((await import('./i18n')).lang).toBe('vi');
  localStorage.setItem('bc.lang', 'en');
  vi.resetModules();
  const m = await import('./i18n');
  expect(m.lang).toBe('en');
  expect(m.t('signal.live')).toBe('Live');
  localStorage.removeItem('bc.lang');
});
```

`ui/src/screens/Settings.test.tsx`:
```tsx
import { fireEvent, render, screen, within } from '@testing-library/react';

import { EngineClient } from '../api/client';
import { FakeWs } from '../test/fakeWs';
import { Settings } from './Settings';

function client() {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({
    type: 'snapshot',
    state: { engine: { gpu: 'AMD Radeon RX 6600', matteBackend: '', previewEncoder: 'h264_amf' } },
    show: { settings: { rate: '60/1', holdMs: 5000, slatePath: '' }, cameras: [] },
  });
  return c;
}

test('shows engine info and saves hold time without a confirm', () => {
  const c = client();
  render(<Settings client={c} />);
  expect(screen.getByText('AMD Radeon RX 6600')).toBeInTheDocument();
  expect(screen.getByText('Không có')).toBeInTheDocument();
  fireEvent.change(screen.getByLabelText('Giữ khung cuối khi mất tín hiệu (giây)'), { target: { value: '8' } });
  fireEvent.click(screen.getByRole('button', { name: 'Lưu cài đặt' }));
  const req = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'setSettings');
  expect(req.args).toEqual({ holdMs: 8000, slatePath: '' });
});

test('a rate change asks first, with a verb button', () => {
  const c = client();
  render(<Settings client={c} />);
  fireEvent.mouseDown(screen.getByLabelText('Tốc độ khung hình'));
  fireEvent.click(within(screen.getByRole('listbox')).getByText('50'));
  fireEvent.click(screen.getByRole('button', { name: 'Lưu cài đặt' }));
  expect(FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'setSettings')).toBeUndefined();
  fireEvent.click(screen.getByRole('button', { name: 'Đổi tốc độ khung hình' }));
  const req = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'setSettings');
  expect(req.args.rate).toBe('50/1');
});
```

- [ ] **Step 2: Implement**

`ui/src/i18n.ts` — replace the `lang` line with:
```ts
function initialLang(): 'vi' | 'en' {
  try {
    if (typeof location !== 'undefined' && new URLSearchParams(location.search).get('lang') === 'en') return 'en';
    return localStorage.getItem('bc.lang') === 'en' ? 'en' : 'vi';
  } catch {
    return 'vi';
  }
}
export const lang = initialLang();

export function setLang(l: 'vi' | 'en') {
  try {
    localStorage.setItem('bc.lang', l);
  } catch {
    /* storage unavailable: the choice lasts for this page only */
  }
  location.reload();
}
```
and add the keys listed in the copy table (`nav.settings`, `settings.rate`, `settings.hold`, `settings.slate`, `settings.slateHint`, `settings.language`, `settings.gpu`, `settings.matte`, `settings.none`, `settings.previewEncoder`, `settings.save`, `settings.rateTitle`, `settings.rateBody`, `settings.rateConfirm`).

`ui/src/screens/Settings.tsx`:
```tsx
import { Box, Button, MenuItem, Stack, TextField, Typography } from '@mui/material';
import { useEffect, useState } from 'react';

import type { EngineClient } from '../api/client';
import { useEngine } from '../api/useEngine';
import { ConfirmDialog } from '../components/ConfirmDialog';
import { ErrorBlock } from '../components/ErrorBlock';
import { lang, setLang, t } from '../i18n';

const rates = [
  { value: '50/1', vi: '50', en: '50' },
  { value: '60000/1001', vi: '59,94', en: '59.94' },
  { value: '60/1', vi: '60', en: '60' },
];

export function Settings({ client }: { client: EngineClient }) {
  const { show, state } = useEngine(client);
  const s = show?.settings ?? { rate: '60/1', holdMs: 5000, slatePath: '' };
  const [rate, setRate] = useState(s.rate);
  const [hold, setHold] = useState(String(s.holdMs / 1000));
  const [slate, setSlate] = useState(s.slatePath);
  const [confirm, setConfirm] = useState(false);
  const [error, setError] = useState('');
  useEffect(() => {
    setRate(s.rate);
    setHold(String(s.holdMs / 1000));
    setSlate(s.slatePath);
  }, [s.rate, s.holdMs, s.slatePath]);

  async function save(withRate: boolean) {
    const args: Record<string, unknown> = { holdMs: Math.round(Number(hold) * 1000), slatePath: slate };
    if (withRate) args.rate = rate;
    try {
      await client.request('setSettings', args);
      setError('');
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  const engine = state.engine ?? {};
  return (
    <Box sx={{ p: 2, maxWidth: 640 }}>
      <Stack direction="row" justifyContent="space-between" alignItems="center" sx={{ mb: 2 }}>
        <Typography variant="h4">{t('nav.settings')}</Typography>
        <Button variant="contained" onClick={() => (rate !== s.rate ? setConfirm(true) : save(false))}>
          {t('settings.save')}
        </Button>
      </Stack>
      {error && <ErrorBlock message={error} />}
      <Stack spacing={2}>
        <TextField select label={t('settings.rate')} value={rate} onChange={(e) => setRate(e.target.value)}>
          {rates.map((r) => (
            <MenuItem key={r.value} value={r.value}>
              {lang === 'en' ? r.en : r.vi}
            </MenuItem>
          ))}
        </TextField>
        <TextField label={t('settings.hold')} type="number" inputProps={{ min: 0, max: 60, step: 0.5 }} value={hold} onChange={(e) => setHold(e.target.value)} />
        <TextField label={t('settings.slate')} helperText={t('settings.slateHint')} value={slate} onChange={(e) => setSlate(e.target.value)} />
        <TextField select label={t('settings.language')} value={lang} onChange={(e) => setLang(e.target.value as 'vi' | 'en')}>
          <MenuItem value="vi">Tiếng Việt</MenuItem>
          <MenuItem value="en">English</MenuItem>
        </TextField>
        <Typography variant="body2">
          {t('settings.gpu')}: {engine.gpu}
        </Typography>
        <Typography variant="body2">
          {t('settings.matte')}: {engine.matteBackend || t('settings.none')}
        </Typography>
        <Typography variant="body2">
          {t('settings.previewEncoder')}: {engine.previewEncoder || t('settings.none')}
        </Typography>
      </Stack>
      <ConfirmDialog
        open={confirm}
        title={t('settings.rateTitle')}
        body={t('settings.rateBody')}
        confirmLabel={t('settings.rateConfirm')}
        onClose={() => setConfirm(false)}
        onConfirm={() => {
          setConfirm(false);
          save(true);
        }}
      />
    </Box>
  );
}
```
(`"Tiếng Việt"` and `"English"` are language self-names and are allowed literals — the Task 2 string guard exempts them by name.)

Add `matteBackend?: string` to `StateTree.engine` in `protocol.ts`. `App.tsx`: a third nav button "Cài đặt" and `view === 'settings'`.

Run `npm test` and `npm run build` in `ui/`.

- [ ] **Step 3: Commit**

```bash
git add ui/src
git commit -m "feat(ui): settings screen (rate with confirm, hold, slate, language, engine info)"
```

---

### Task 2: Vietnamese-first error text and the string guard

**Files:**
- Create: `ui/src/errors.ts`, `ui/src/errors.test.ts`, `ui/src/test/strings.test.ts`
- Modify: `ui/src/components/ErrorBlock.tsx` (accept `{code, message}`), every `ErrorBlock` call site that has an `ErrorInfo` (Multiview, CameraDetail, ColorMatch), `ui/src/i18n.ts` (error sentences)

**Interfaces:**
- Produces: `errorText(e: ErrorInfo): { headline: string; detail: string }`; `ErrorBlock` props become `{ title?, message?, error?: ErrorInfo, severity? }` (when `error` is given, the headline is shown in bold and the engine message as the detail line).

Error codes from the engine (all milestones) and their Vietnamese headline (English in `en`): `input-failed` "Không mở được nguồn tín hiệu." / "The source could not be opened."; `input-ended` "Nguồn đã ngừng gửi tín hiệu." / "The source stopped sending."; `input-config` "Cấu hình nguồn không hợp lệ." / "The source settings are not valid."; `output-failed`, `srt-out-failed`, `ndi-failed`, `omt-failed`, `decklink-failed`, `aja-failed`, `vcam-failed`, `fullscreen-failed` "Đầu ra gặp lỗi và đang thử lại." / "The output failed and is retrying."; `output-config` "Cấu hình đầu ra không hợp lệ." / "The output settings are not valid."; `stage-failed` "Bước xử lý gặp lỗi nên đang được bỏ qua." / "A processing step failed and is bypassed."; `stage-overrun` "Bước xử lý quá chậm nên đang được bỏ qua." / "A processing step is too slow and is bypassed."; `matte-unavailable` "Máy này không chạy được tách nền." / "Background removal cannot run on this PC."; `background-image` "Không đọc được ảnh nền; đang dùng màu nền." / "The background image cannot be read; using the colour."; `color-lut` "Không đọc được LUT màu; chỉ dùng tinh chỉnh." / "The colour LUT cannot be read; using trims only."; `ndi-no-video` "Nguồn NDI không gửi hình." / "The NDI source sends no video."; `ndi-audio-rate` "Âm thanh NDI không phải 48 kHz nên bị bỏ qua." / "NDI audio is not 48 kHz and is ignored."; `decklink-no-signal` "Không có tín hiệu ở cổng DeckLink." / "No signal on the DeckLink input."; `capture-failed` "Không mở được thiết bị USB." / "The USB device could not be opened."; any other code "Có lỗi xảy ra." / "Something went wrong.".

- [ ] **Step 1: Tests**

`ui/src/errors.test.ts`:
```ts
import { errorText } from './errors';

test('known codes get a Vietnamese headline and keep the engine detail', () => {
  const r = errorText({ code: 'input-failed', message: 'cannot open srt://1.2.3.4:9000: Connection refused' });
  expect(r.headline).toBe('Không mở được nguồn tín hiệu.');
  expect(r.detail).toContain('srt://1.2.3.4:9000');
});

test('unknown codes fall back without leaking undefined', () => {
  const r = errorText({ code: 'brand-new-code', message: 'detail' });
  expect(r.headline).toBe('Có lỗi xảy ra.');
  expect(r.detail).toBe('detail');
  expect(JSON.stringify(r)).not.toContain('undefined');
});
```

`ui/src/test/strings.test.ts` — the guard:
```ts
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join } from 'node:path';

const ALLOWED = new Set(['BreadCamera', 'Multiview', 'Tiếng Việt', 'English', 'NDI', 'OMT', 'SRT', 'GPU', 'LUT', 'Lift', 'Gamma', 'Gain']);

function files(dir: string): string[] {
  return readdirSync(dir).flatMap((f) => {
    const p = join(dir, f);
    if (statSync(p).isDirectory()) return f === 'test' ? [] : files(p);
    return p.endsWith('.tsx') && !p.endsWith('.test.tsx') ? [p] : [];
  });
}

test('no hard-coded user-facing text in components', () => {
  const offenders: string[] = [];
  for (const f of files(join(__dirname, '..'))) {
    const src = readFileSync(f, 'utf8');
    for (const m of src.matchAll(/>\s*([A-Za-zÀ-ỹ][^<>{}]*?)\s*</g)) {
      const text = m[1].trim();
      if (text && !ALLOWED.has(text)) offenders.push(`${f}: "${text}"`);
    }
  }
  expect(offenders).toEqual([]);
});
```
Run `npm test` → the errors test fails (missing module); the guard reports any literal text — fix every offender by moving it into `i18n.ts`.

- [ ] **Step 2: Implement**

`ui/src/errors.ts`:
```ts
import type { ErrorInfo } from './api/protocol';
import { hasKey, t } from './i18n';

const OUTPUT = ['output-failed', 'srt-out-failed', 'ndi-failed', 'omt-failed', 'decklink-failed', 'aja-failed', 'vcam-failed', 'fullscreen-failed'];

// Vietnamese-first sentence for the operator; the engine's English detail stays underneath.
export function errorText(e: ErrorInfo): { headline: string; detail: string } {
  const key = OUTPUT.includes(e.code) ? 'err.output-failed' : `err.${e.code}`;
  return { headline: t(hasKey(key) ? key : 'err.unknown'), detail: e.message ?? '' };
}
```
In `ui/src/i18n.ts` add `export function hasKey(k: string): k is Key { return Object.prototype.hasOwnProperty.call(vi, k); }` and the keys `err.input-failed`, `err.input-ended`, `err.input-config`, `err.output-failed`, `err.output-config`, `err.stage-failed`, `err.stage-overrun`, `err.matte-unavailable`, `err.background-image`, `err.color-lut`, `err.ndi-no-video`, `err.ndi-audio-rate`, `err.decklink-no-signal`, `err.capture-failed`, `err.unknown` to both dictionaries with the sentences above.

`ErrorBlock.tsx`: accept `error?: ErrorInfo`; when present render `errorText(error).headline` as the title and `.detail` as the message. Change call sites that pass `xxx.error.message` to pass `error={xxx.error}` instead.

Run `npm test` (all pass, guard clean) and `npm run build`.

- [ ] **Step 3: Commit**

```bash
git add ui/src
git commit -m "feat(ui): Vietnamese-first error sentences for every engine code; hard-coded string guard"
```

---

### Task 3: Soak and fault-injection tool

**Files:**
- Create: `tools/soak/soak_main.cpp`, `scripts/soak.ps1`
- Modify: `CMakeLists.txt` (`bc_soak` executable linking `bc_engine`, `bc_shell`, and the test-media helpers compiled into it)

**Interfaces:**
- Consumes: `writeTestTs`, `TsPusher` (compile `tests/support/test_media.cpp` into `bc_soak` too), `ProcessLauncher`, `Supervisor` (M1 Task 12), `ShowStore` (M1 Task 3), the control API (M1 Task 11).
- Produces: `bc_soak.exe --minutes N [--faults] [--out <csv>]` → exit 0 = pass, 1 = fail (reason printed), 2 = setup error. `scripts/soak.ps1 -Hours 8 [-Faults]` wraps it.

Tool behaviour:
1. Creates a temp show dir with 4 SRT listener cameras (ports 47971–47974), each with `stages` matte on + background blur + colour on (exposure 0.1), and outputs: `srt` listener (47981–47984), `fullscreen` monitor 0 for camera 1 only, plus `ndi`/`omt` when built (added through `addOutput`; refused kinds are logged, not fatal).
2. Starts `breadcamera-engine.exe` (next to the tool) through `ProcessLauncher` + `Supervisor` (the shell's restart policy), connects with `ix::WebSocket`, and starts 4 `TsPusher`s with a 1080p60 clip it writes once.
3. For each SRT output, an `FfmpegInput` caller reads frames (count only) — the outputs have real receivers.
4. Every 10 s: `ping`, the last `stats` message, the engine's working set (`GetProcessMemoryInfo` on the launcher's PID), and the state snapshot (phases, stage states) → one CSV row (`time,overruns,cam,fps,latencyMs,phase,matte,workingSetMB,receiverFrames`).
5. With `--faults`, on a fixed schedule (every 5 min, rotating): stop pusher 1 for 15 s (expect `holding` → `slate` → `live` within 10 s of return); stop SRT receiver 2 for 30 s (expect the other outputs' receiver counts to keep rising); set camera 3's `background.mode` to `replace` with `image` = a missing file (expect `background-image` error while frames continue), then restore; kill the engine process (expect `ping` to succeed again within 3 s and `getShow` to list 4 cameras).
6. Verdict at the end: fail if any non-fault interval had overruns > 0, any camera latency > 50 ms (median of its rows), working set at the end > 105 % of the 10-minute mark, any fault expectation unmet, or **zero** samples collected. Print a summary table and the CSV path.

- [ ] **Step 1: Write the tool**

`tools/soak/soak_main.cpp`:
```cpp
// bc_soak: spec §8.3 soak + §8.4 fault injection against the real engine executable.
#include <windows.h>
#include <psapi.h>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/show_store.h"
#include "core/stage_params.h"
#include "core/utf8.h"
#include "engine/control_server.h"
#include "gpu/device.h"
#include "io/ffmpeg_input.h"
#include "io/srt_params.h"
#include "shell/process_launcher.h"
#include "shell/supervisor.h"
#include "support/test_media.h"

namespace fs = std::filesystem;
using nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
constexpr int kCams = 4;
constexpr int kPort = 47970;

struct Args {
  int minutes = 30;
  bool faults = false;
  std::string out;
};

Args parseArgs(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    if (k == "--minutes" && i + 1 < argc) a.minutes = std::stoi(argv[++i]);
    else if (k == "--faults") a.faults = true;
    else if (k == "--out" && i + 1 < argc) a.out = argv[++i];
  }
  if (a.out.empty()) a.out = (fs::temp_directory_path() / "bcam-soak.csv").string();
  return a;
}

// Control-API client: replies matched by id; state kept from snapshot + patches; last stats.
class EngineClient {
 public:
  EngineClient(int port, const std::string& token) {
    ws_.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/?token=" + token);
    ws_.setMinWaitBetweenReconnectionRetries(250);
    ws_.setMaxWaitBetweenReconnectionRetries(500);
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& m) { onMessage(m); });
    ws_.start();
  }
  ~EngineClient() { ws_.stop(); }

  std::optional<json> request(const std::string& cmd, json args = json::object(), std::chrono::milliseconds timeout = 2000ms) {
    int id;
    {
      std::lock_guard l(m_);
      id = ++next_;
    }
    ws_.send(json{{"id", id}, {"cmd", cmd}, {"args", args}}.dump());
    std::unique_lock l(m_);
    if (!cv_.wait_for(l, timeout, [&] { return replies_.count(id) > 0; })) return std::nullopt;
    json r = replies_[id];
    replies_.erase(id);
    if (!r.value("ok", false)) return std::nullopt;
    return r["result"];
  }
  json state() {
    std::lock_guard l(m_);
    return state_;
  }
  json stats() {
    std::lock_guard l(m_);
    return stats_;
  }
  std::string at(const std::string& ptr) {
    std::lock_guard l(m_);
    json::json_pointer p(ptr);
    return state_.contains(p) && state_.at(p).is_string() ? state_.at(p).get<std::string>() : "";
  }

 private:
  void onMessage(const ix::WebSocketMessagePtr& m) {
    if (m->type != ix::WebSocketMessageType::Message || m->binary) return;
    json j = json::parse(m->str, nullptr, false);
    if (!j.is_object()) return;
    std::lock_guard l(m_);
    const std::string type = j.value("type", "");
    if (type == "snapshot") {
      state_ = j["state"];
    } else if (type == "patch") {
      for (auto& op : j["ops"]) {
        json::json_pointer p(op["path"].get<std::string>());
        if (op["op"] == "replace") state_[p] = op["value"];
        else if (state_.contains(p)) state_.at(p.parent_pointer()).erase(p.back());
      }
    } else if (type == "stats") {
      stats_ = j;
    } else if (j.contains("id") && j["id"].is_number_integer()) {
      replies_[j["id"].get<int>()] = j;
      cv_.notify_all();
    }
  }
  ix::WebSocket ws_;
  std::mutex m_;
  std::condition_variable cv_;
  int next_ = 0;
  std::map<int, json> replies_;
  json state_ = json::object();
  json stats_ = json::object();
};

struct Counter : bc::InputSink {
  std::atomic<int64_t> frames{0};
  void onVideo(bc::VideoFrameIn) override { ++frames; }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
};

std::string camId(int i) { return "soak" + std::to_string(i); }

bc::Show buildShow() {
  bc::Show show;
  for (int i = 0; i < kCams; ++i) {
    bc::CameraConfig c;
    c.id = camId(i);
    c.name = "Soak " + std::to_string(i + 1);
    c.input.kind = "srt";
    c.input.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47971 + i}};
    c.stages = {{"color", bc::normalizeStageParams("color", {{"enabled", true}, {"exposure", 0.1}})},
                {"matte", bc::normalizeStageParams("matte", {{"enabled", true}})},
                {"background", bc::normalizeStageParams("background", {{"mode", "blur"}})}};
    c.outputs.push_back({"srt" + std::to_string(i), "srt", false, true,
                         {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47981 + i}, {"width", 1280}, {"height", 720}, {"bitrateKbps", 6000}}});
    if (i == 0) c.outputs.push_back({"fs0", "fullscreen", false, true, {{"monitor", 0}}});
    show.cameras.push_back(c);
  }
  return show;
}

double workingSetMb(DWORD pid) {
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!h) return 0;
  PROCESS_MEMORY_COUNTERS pmc{};
  GetProcessMemoryInfo(h, &pmc, sizeof(pmc));
  CloseHandle(h);
  return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
  const auto end = Clock::now() + timeout;
  while (Clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(100ms);
  }
  return pred();
}

struct Rig {
  fs::path dir, clip;
  std::vector<std::unique_ptr<bc::test::TsPusher>> pushers;
  std::unique_ptr<bc::GpuDevice> gpu;
  std::unique_ptr<bc::TexturePool> pool;
  std::vector<std::unique_ptr<bc::FfmpegInput>> readers;
  Counter counts[kCams];

  void startPusher(int i) {
    pushers[i] = std::make_unique<bc::test::TsPusher>(clip, "srt://127.0.0.1:" + std::to_string(47971 + i) + "?mode=caller", true);
  }
  void startReader(int i) {
    readers[i] = std::make_unique<bc::FfmpegInput>(*gpu, *pool, bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "127.0.0.1"}, {"port", 47981 + i}}));
    readers[i]->start(counts[i]);
  }
};

// §8.4: input killed → hold → slate → live on return.
std::string faultInputLoss(Rig& rig, EngineClient& e) {
  const std::string p = "/cameras/" + camId(0) + "/phase";
  rig.pushers[0].reset();
  bool held = false;
  const bool slated = waitFor([&] {
    held = held || e.at(p) == "holding";
    return e.at(p) == "slate";
  }, 9000ms);
  rig.startPusher(0);
  const bool back = waitFor([&] { return e.at(p) == "live"; }, 10000ms);
  if (!held || !slated || !back) return "input-loss fault: held=" + std::to_string(held) + " slated=" + std::to_string(slated) + " back=" + std::to_string(back);
  return "";
}

// §8.4: one receiver killed → the others keep receiving.
std::string faultReceiverLoss(Rig& rig) {
  rig.readers[1]->stop();
  int64_t before[kCams];
  for (int i = 0; i < kCams; ++i) before[i] = rig.counts[i].frames;
  std::this_thread::sleep_for(30s);
  std::string err;
  for (int i : {0, 2, 3}) {
    if (rig.counts[i].frames - before[i] < 30 * 50) err += "receiver " + std::to_string(i) + " slowed while receiver 1 was down; ";
  }
  rig.startReader(1);
  return err;
}

// §8.4: a stage forced to fail → bypassed/errored for that camera only, frames continue.
std::string faultStage(Rig& rig, EngineClient& e) {
  const auto cam = camId(2);
  const int64_t before = rig.counts[2].frames;
  e.request("setStage", {{"cameraId", cam}, {"stage", "background"}, {"params", {{"mode", "replace"}, {"image", "C:/definitely/missing.png"}}}});
  const bool flagged = waitFor([&] { return e.at("/cameras/" + cam + "/stages/background/error/code") == "background-image"; }, 4000ms);
  const bool flowing = rig.counts[2].frames - before > 100;
  e.request("setStage", {{"cameraId", cam}, {"stage", "background"}, {"params", {{"mode", "blur"}, {"image", ""}}}});
  const bool othersOk = e.at("/cameras/" + camId(1) + "/stages/background/error/code").empty();
  if (!flagged || !flowing || !othersOk) return "stage fault: flagged=" + std::to_string(flagged) + " flowing=" + std::to_string(flowing) + " othersOk=" + std::to_string(othersOk);
  return "";
}

// §8.4: engine killed → back with its show in < 3 s.
std::string faultEngineKill(bc::ProcessLauncher& launcher, EngineClient& e) {
  launcher.kill();
  const auto t0 = Clock::now();
  const bool back = waitFor([&] { return e.request("ping", json::object(), 300ms).has_value(); }, 10000ms);
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0);
  auto show = e.request("getShow");
  const bool showOk = show && (*show)["cameras"].size() == kCams;
  if (!back || took >= 3000ms || !showOk) return "engine-kill fault: back=" + std::to_string(back) + " took=" + std::to_string(took.count()) + "ms showOk=" + std::to_string(showOk);
  return "";
}
}  // namespace

int main(int argc, char** argv) {
  const Args args = parseArgs(argc, argv);
  ix::initNetSystem();
  Rig rig;
  rig.dir = fs::temp_directory_path() / ("bcam-soak-" + std::to_string(GetCurrentProcessId()));
  fs::create_directories(rig.dir);
  {
    bc::ShowStore store(rig.dir / "shows");
    store.saveNow(buildShow());
  }
  wchar_t self[MAX_PATH];
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  const fs::path engineExe = fs::path(self).parent_path() / "breadcamera-engine.exe";
  if (!fs::exists(engineExe)) {
    std::cerr << "setup: " << engineExe.string() << " not found\n";
    return 2;
  }
  const std::string token = bc::randomToken();
  bc::ProcessLauncher launcher(engineExe, L"--port " + std::to_wstring(kPort) + L" --token " + bc::toWide(token) + L" --show-dir \"" +
                                             (rig.dir / "shows").wstring() + L"\" --discovery \"" + (rig.dir / "engine.json").wstring() + L"\"");
  bc::Supervisor sup(launcher);
  std::atomic<bool> supervising{true};
  std::thread supThread([&] {
    while (supervising) {
      sup.poll(Clock::now());
      std::this_thread::sleep_for(100ms);
    }
  });
  EngineClient engine(kPort, token);
  if (!waitFor([&] { return engine.request("ping", json::object(), 500ms).has_value(); }, 15000ms)) {
    std::cerr << "setup: the engine did not answer within 15 s\n";
    supervising = false;
    supThread.join();
    launcher.kill();
    return 2;
  }
  for (const char* kind : {"ndi", "omt"}) {
    for (int i = 0; i < kCams; ++i) {
      if (!engine.request("addOutput", {{"cameraId", camId(i)}, {"output", {{"kind", kind}}}})) std::cout << "note: " << kind << " output refused on " << camId(i) << "\n";
    }
  }
  rig.clip = rig.dir / "clip.ts";
  bc::test::writeTestTs(rig.clip, 1920, 1080, 60, 10);
  rig.pushers.resize(kCams);
  rig.readers.resize(kCams);
  for (int i = 0; i < kCams; ++i) rig.startPusher(i);
  rig.gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  rig.pool = std::make_unique<bc::TexturePool>(*rig.gpu);
  for (int i = 0; i < kCams; ++i) rig.startReader(i);

  std::ofstream csv(args.out);
  csv << "seconds,overruns,camera,fps,latencyMs,phase,matte,workingSetMB,receiverFrames\n";
  std::vector<std::string> failures;
  std::map<std::string, std::vector<double>> latencies;
  const auto start = Clock::now();
  const auto end = start + std::chrono::minutes(args.minutes);
  auto nextFault = start + 5min;
  int faultIndex = 0, samples = 0;
  int64_t lastOverruns = -1;
  double baselineWs = 0, lastWs = 0;
  auto baselineAt = start + 10min;
  while (Clock::now() < end) {
    std::this_thread::sleep_for(10s);
    const double t = std::chrono::duration<double>(Clock::now() - start).count();
    const json st = engine.stats();
    const int64_t overruns = st.contains("engine") ? st["engine"].value("overruns", int64_t{0}) : 0;
    if (lastOverruns >= 0 && overruns > lastOverruns) failures.push_back("tick overruns (+" + std::to_string(overruns - lastOverruns) + ") at t=" + std::to_string(static_cast<int>(t)) + " s");
    lastOverruns = overruns;
    lastWs = workingSetMb(launcher.pid());
    if (baselineWs == 0 && Clock::now() >= baselineAt) baselineWs = lastWs;
    for (int i = 0; i < kCams; ++i) {
      const auto id = camId(i);
      const json c = st.contains("cameras") && st["cameras"].contains(id) ? st["cameras"][id] : json::object();
      const double lat = c.value("latencyMs", 0.0);
      latencies[id].push_back(lat);
      csv << t << ',' << overruns << ',' << id << ',' << c.value("fps", 0.0) << ',' << lat << ',' << engine.at("/cameras/" + id + "/phase") << ','
          << engine.at("/cameras/" + id + "/stages/matte/state") << ',' << lastWs << ',' << rig.counts[i].frames << '\n';
    }
    csv.flush();
    ++samples;
    if (args.faults && Clock::now() >= nextFault) {
      std::string r;
      switch (faultIndex++ % 4) {
        case 0: r = faultInputLoss(rig, engine); break;
        case 1: r = faultReceiverLoss(rig); break;
        case 2: r = faultStage(rig, engine); break;
        case 3:
          r = faultEngineKill(launcher, engine);
          baselineWs = 0;  // a new process: re-baseline 10 minutes from now
          baselineAt = Clock::now() + 10min;
          break;
      }
      if (!r.empty()) failures.push_back(r);
      std::cout << "fault " << (faultIndex - 1) % 4 << ": " << (r.empty() ? "ok" : r) << "\n";
      lastOverruns = -1;  // a fault window may overrun once while things reconnect
      nextFault = Clock::now() + 5min;
    }
  }

  if (samples == 0) failures.push_back("no samples collected");
  for (auto& [id, v] : latencies) {
    if (v.empty()) continue;
    std::sort(v.begin(), v.end());
    const double median = v[v.size() / 2];
    std::cout << id << ": median latency " << median << " ms\n";
    if (median > 50.0) failures.push_back(id + ": median latency " + std::to_string(median) + " ms > 50");
  }
  if (baselineWs > 0 && lastWs > baselineWs * 1.05) failures.push_back("memory grew from " + std::to_string(baselineWs) + " MB to " + std::to_string(lastWs) + " MB");
  std::cout << "samples " << samples << ", working set " << baselineWs << " → " << lastWs << " MB, CSV " << args.out << "\n";

  for (auto& r : rig.readers) if (r) r->stop();
  for (auto& p : rig.pushers) if (p) p->stop();
  sup.stopSupervising();
  engine.request("shutdown");
  waitFor([&] { return !launcher.isRunning(); }, 5000ms);
  if (launcher.isRunning()) launcher.kill();
  supervising = false;
  supThread.join();

  for (auto& f : failures) std::cout << "FAIL: " << f << "\n";
  std::cout << (failures.empty() ? "PASS\n" : "FAIL\n");
  return failures.empty() ? 0 : 1;
}
```

Add to `CMakeLists.txt`:
```cmake
add_executable(bc_soak tools/soak/soak_main.cpp tests/support/test_media.cpp)
target_include_directories(bc_soak PRIVATE tests)
target_link_libraries(bc_soak PRIVATE bc_engine bc_shell psapi)
add_dependencies(bc_soak breadcamera-engine)
bc_copy_runtime(bc_soak)
```

`scripts/soak.ps1`:
```powershell
param([double]$Hours = 0.5, [switch]$Faults)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -Preset release
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$out = Join-Path $env:TEMP ("bcam-soak-" + (Get-Date -Format 'yyyyMMdd-HHmm') + '.csv')
$exe = Join-Path $PSScriptRoot '..\build\release\bc_soak.exe'
$argsList = @('--minutes', [int]($Hours * 60), '--out', $out)
if ($Faults) { $argsList += '--faults' }
& $exe @argsList
$code = $LASTEXITCODE
Write-Host "CSV: $out"
exit $code
```

- [ ] **Step 2: Smoke it, then the 30-minute evidence run**

1. `bc_soak.exe --minutes 2` → exit 0, a CSV with ≥ 10 rows. Fix setup issues until it passes; a zero-sample run must exit 1 (prove it once by pointing it at a missing engine path — temporary, revert).
2. `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/soak.ps1 -Hours 0.5 -Faults` → exit 0. Paste the summary table and the CSV path into the report. If it fails, report the failing criterion with its rows; do not change the criteria.

- [ ] **Step 3: Commit**

```bash
git add tools scripts/soak.ps1 CMakeLists.txt
git commit -m "test: bc_soak soak + fault-injection tool driving the real engine"
```

---

### Task 4: Installer and licence notices

**Files:**
- Create: `installer/BreadCamera.iss`, `installer/THIRD_PARTY_NOTICES.md`, `installer/LGPL-source-offer.txt`, `scripts/package.ps1`
- Modify: `src/shell/shell_main.cpp` (named mutex the installer can detect: already `Local\BreadCameraShell`; add `AppMutex` support — no code change needed beyond confirming the name)

**Interfaces:**
- Produces: `dist/BreadCamera-Setup-<version>.exe` from `scripts/package.ps1 -Version 0.1.0`.

- [ ] **Step 1: Notices**

`installer/THIRD_PARTY_NOTICES.md` — one section per component in spec §7.1 plus the M2–M6 additions, each with name, version (from `vcpkg.json` baseline / pinned tags), licence, URL, and the licence text or a pointer to the file shipped next to it: FFmpeg (LGPL-2.1+, dynamically linked DLLs, source offer in `LGPL-source-offer.txt`), libsrt (MPL-2.0), ixwebsocket (BSD-3-Clause), nlohmann-json (MIT), WIL (MIT), WebView2 SDK (Microsoft licence), NDI SDK runtime (NDI licence + "NDI® is a registered trademark of Vizrt NDI AB" attribution), NVIDIA Video Effects SDK runtime (NVIDIA SLA + Community Model License), Blackmagic DeckLink API (Blackmagic SDK licence), AJA libajantv2 (MIT), libomt/libvmx (MIT), VCamSample (MIT), MODNet model (Apache-2.0, M6), ONNX Runtime + DirectML (MIT, M6). Copy each licence text from the source tree / package it came from.

`installer/LGPL-source-offer.txt`:
```
BreadCamera uses FFmpeg libraries under the GNU Lesser General Public License v2.1 or later,
linked dynamically (avcodec-*.dll, avformat-*.dll, avutil-*.dll, swscale-*.dll, swresample-*.dll).
The exact FFmpeg source used for this build is the vcpkg port at the baseline recorded in vcpkg.json.
On request we provide the complete corresponding source code: contact bmsolutions.vn.
You may replace these DLLs with your own compatible builds.
```

- [ ] **Step 2: The Inno Setup script**

`installer/BreadCamera.iss`:
```ini
#define AppName "BreadCamera"
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\build\release"
#endif

[Setup]
AppId={{9C1F2B7A-3E44-4A39-9F0B-6A1D2C3B4E50}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=BM Solutions
AppPublisherURL=https://bmsolutions.vn
DefaultDirName={autopf}\BreadMedia\BreadCamera
DefaultGroupName=BreadCamera
OutputDir=..\dist
OutputBaseFilename=BreadCamera-Setup-{#AppVersion}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0.19041
AppMutex=Local\BreadCameraShell,Global\BreadCameraShell
CloseApplications=yes
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile=THIRD_PARTY_NOTICES.md

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#BuildDir}\BreadCamera.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\breadcamera-engine.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\ui\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs
Source: "{#BuildDir}\models\*"; DestDir: "{app}\models"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "{#BuildDir}\vcam\breadcamera-vcam.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#VcRedist}"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "THIRD_PARTY_NOTICES.md"; DestDir: "{app}"
Source: "LGPL-source-offer.txt"; DestDir: "{app}"

[Icons]
Name: "{group}\BreadCamera"; Filename: "{app}\BreadCamera.exe"
Name: "{autodesktop}\BreadCamera"; Filename: "{app}\BreadCamera.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Visual C++ runtime..."
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\breadcamera-vcam.dll"""; Check: IsWin11 and FileExists(ExpandConstant('{app}\breadcamera-vcam.dll')); StatusMsg: "Registering the virtual camera..."
Filename: "{app}\BreadCamera.exe"; Description: "Start BreadCamera"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\breadcamera-vcam.dll"""; RunOnceId: "UnregVcam"; Check: FileExists(ExpandConstant('{app}\breadcamera-vcam.dll'))
Filename: "{sys}\taskkill.exe"; Parameters: "/im breadcamera-engine.exe /f"; RunOnceId: "StopEngine"; Flags: runhidden

[Code]
function IsWin11: Boolean;
var V: TWindowsVersion;
begin
  GetWindowsVersionEx(V);
  Result := (V.Major = 10) and (V.Build >= 22000);
end;

function InitializeSetup: Boolean;
var Code: Integer;
begin
  { The engine survives the shell (spec §2); stop it so its files can be replaced. }
  if CheckForMutexes('Local\BreadCameraEngine') then
  begin
    if MsgBox('BreadCamera is running and its outputs are live. Stop it and continue the installation?', mbConfirmation, MB_YESNO) = IDNO then
    begin
      Result := False;
      exit;
    end;
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/im breadcamera-engine.exe /f', '', SW_HIDE, ewWaitUntilTerminated, Code);
  end;
  Result := True;
end;
```
Note: `{#VcRedist}` is passed by `package.ps1` (`/DVcRedist=<path>`). The engine's mutex is session-local (`Local\`), so `CheckForMutexes` sees it from the installer running in the same session.

- [ ] **Step 3: Packaging script**

`scripts/package.ps1`:
```powershell
param([string]$Version = '0.1.0')
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..')
Push-Location (Join-Path $repo 'ui')
$env:PATH = "C:\Program Files\nodejs;$env:PATH"
$env:NODE_AUTH_TOKEN = (gh auth token)
npm ci; if ($LASTEXITCODE) { exit $LASTEXITCODE }
npm run build; if ($LASTEXITCODE) { exit $LASTEXITCODE }
Pop-Location
& (Join-Path $PSScriptRoot 'build.ps1') -Preset release
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
$redist = Get-ChildItem "$vs\VC\Redist\MSVC\*\vc_redist.x64.exe" | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $redist) { throw 'vc_redist.x64.exe not found in the Visual Studio install' }
$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) { winget install --id JRSoftware.InnoSetup -e --silent --accept-package-agreements --accept-source-agreements }
& $iscc "/DAppVersion=$Version" "/DVcRedist=$($redist.FullName)" (Join-Path $repo 'installer\BreadCamera.iss')
exit $LASTEXITCODE
```
Add `/dist/` to `.gitignore`.

- [ ] **Step 4: Build the installer and verify its contents**

1. `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package.ps1 -Version 0.1.0` → `dist/BreadCamera-Setup-0.1.0.exe`.
2. If this session is elevated: `dist\BreadCamera-Setup-0.1.0.exe /VERYSILENT /SUPPRESSMSGBOXES /DIR="%TEMP%\bcam-install"`, check that `BreadCamera.exe`, `breadcamera-engine.exe`, `ui\index.html`, FFmpeg DLLs and `THIRD_PARTY_NOTICES.md` exist, start `breadcamera-engine.exe --token <16+ chars> --port 47990` from there for 5 s and `ping` it, then uninstall with `unins000.exe /VERYSILENT`. If not elevated, ledger "installer built, install step needs admin" and list the files `ISCC` packed (its `/O` output or `innounp -v` if available). Either way paste the evidence.

- [ ] **Step 5: Commit**

```bash
git add installer scripts/package.ps1 .gitignore
git commit -m "build: Inno Setup installer with VC++ runtime, virtual camera registration and licence notices"
```

---

### Task 5: Verification, the fresh-install human check, follow-ups

**Files:**
- Create: `docs/superpowers/manual-checks/m5-fresh-install.md`
- Modify: `docs/superpowers/follow-ups.md`

- [ ] **Step 1: Full verification**

`scripts/test.ps1` (all; passed/skipped + reasons), `ui/` `npm test` + `npm run build`, `BC_FORCE_WARP=1` run, and the Task 3 30-minute soak result. Paste all summaries.

- [ ] **Step 2: Human gate — fresh Windows 11 install**

`docs/superpowers/manual-checks/m5-fresh-install.md`:
```markdown
# M5 fresh-install check (owner, spec §9 M5 "a clean install works on a fresh Windows 11 machine")

1. On a Windows 11 PC that never had BreadCamera or Visual Studio: run `BreadCamera-Setup-<v>.exe`.
2. The app starts; the window shows "Chưa có camera nào…" and the GPU name in the header.
3. Add an SRT listener camera; push a stream from OBS (`srt://<pc>:9000?mode=caller`) → "Đang phát" within 5 s, preview moving.
4. Add an NDI output (if the NDI runtime is installed) and see it in vMix/OBS; add a Windows virtual camera output and see it in the Camera app.
5. Close the window → outputs keep running (tray icon). Tray → "Thoát…" → the dialog's "Dừng tất cả đầu ra" stops everything.
6. Uninstall from Settings → Apps; `%APPDATA%\BreadCamera\shows` is still there.
7. Switch to English in Settings → the app reloads in English; the glossary terms read Ready / Connecting / Live / Offline.
```

- [ ] **Step 3: Follow-ups**

Append to `docs/superpowers/follow-ups.md`:
```markdown
## M5
- The 8-hour soak (`scripts/soak.ps1 -Hours 8 -Faults`) is queued as its own night run; M5 carries the 30-minute evidence.
- Fresh-install check is a human gate: `docs/superpowers/manual-checks/m5-fresh-install.md`.
- `@bmsolutionsvn/design` still lacks a `breadcamera` product id / account-less `BmAppShell`; the UI uses the theme only (`ui/src/App.tsx`).
- Engine error messages stay English as detail lines under Vietnamese headlines (`ui/src/errors.ts`).
```

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers
git commit -m "docs: M5 fresh-install check and follow-ups"
```
