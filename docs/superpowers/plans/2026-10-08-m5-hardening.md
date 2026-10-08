# BreadCamera M5 — Hardening: settings, error text, soak and fault runs, installer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make BreadCamera shippable: a Settings dialog (show format, hold time, slate, GPU/backend info), a plain headline for every engine error code, a soak + fault-injection tool that proves spec §8.3/§8.4 against the real executables, and an Inno Setup installer that also registers the virtual camera and ships the licence notices.

**Amended 2026-10-08 after the UI mockup review (owner):** English only (no language setting, no translation tables, no string guard); Settings is a dialog in M1's native window, with the show resolution next to the frame rate; Tasks 1 and 2 rewritten.

**Architecture:** UI work stays in `ui/`; error text is a pure mapping from the engine's stable `{code, message}` to localised copy (the English engine message is kept as detail). The soak tool `bc_soak.exe` drives real `breadcamera-engine.exe` processes over the control API with SRT test streams generated in-process, samples stats and process memory, injects faults on a schedule, and writes a CSV + verdict. The installer is an Inno Setup script built by `ISCC.exe`.

**Tech Stack:** as before; Inno Setup 6 (installed with `winget install JRSoftware.InnoSetup` if missing).

**Spec:** `docs/superpowers/specs/2026-10-08-breadcamera-design.md` (§6.4, §8.3, §8.4, §7 installer, §7.1 licences).

**Branch:** `bcam-m5`, cut from `bcam-m4b`.

## Global Constraints

- All earlier Global Constraints apply.
- The UI is English only; operator-typed names are Unicode. Output states read Ready / Connecting / Live / Offline (UX contract, in English).
- Errors stay persistent blocks; the block shows the localised sentence first and the engine's detail second.
- Soak pass criteria (spec §8.3): 4× 1080p60 SRT, all effects on (matte via the active backend or bypassed with an error on PCs without one — say which), every buildable output type active, **zero tick overruns**, **flat memory** (working set at the end ≤ 105 % of the 10-minute mark), **latency ≤ 50 ms** on every camera.
- Fault criteria (spec §8.4): input killed → hold then slate then live again on return; one output receiver killed → others unaffected; stage forced to fail → bypassed for that camera only; engine killed → back with its show in < 3 s.
- The installer installs per machine (admin), puts the app in `%ProgramFiles%\BreadMedia\BreadCamera`, registers `breadcamera-vcam.dll` only on Windows 11 (build ≥ 22000), and removes the registration on uninstall. It never deletes `%APPDATA%\BreadCamera` (shows are user data).

## Review Focus

1. **A setting that would disrupt a live show** (frame rate or resolution change while cameras are live) — the UI asks first with a verb button ("Change format") and explains that every output restarts — owned by Task 1.
2. **An engine error code the UI has never seen** — falls back to "Something went wrong." plus the engine detail, never `undefined` or a raw code alone — owned by Task 2.
3. **A slate path that cannot be read** — Save still succeeds, the engine shows its slate warning on the Engine page, and the built-in slate is used — owned by Task 1 (engine behaviour from M1).
4. **The soak tool itself failing silently** (engine never started, pushers never connected) — the tool exits non-zero with a reason; a run with zero samples is a failure, not a pass — owned by Task 3.
5. **Installing over a running BreadCamera** — the installer closes the shell/engine first (asks the user), or refuses with a clear message — owned by Task 4.

## File Structure

```
ui/src/dialogs/SettingsDialog.tsx, ui/src/errors.ts
tools/soak/soak_main.cpp            bc_soak.exe
installer/BreadCamera.iss, installer/THIRD_PARTY_NOTICES.md, installer/LGPL-source-offer.txt
scripts/package.ps1                 build release + UI + installer
docs/superpowers/manual-checks/m5-fresh-install.md
```

## Execution notes for the controller

- 5 tasks → batched reviews (1–3, 4–5).
- The full 8-hour soak does **not** run inside this plan: Task 3 runs a 30-minute soak as evidence and leaves `scripts/soak.ps1 -Hours 8` for a separate night (the planner queues it).

---

### Task 1: The Settings dialog (show format, signal loss, engine info)

Rewritten 2026-10-08 for M1's native window (spec §6.4 "Dialogs"; mockup tabs "Settings" and "Change frame rate"). English only; no language setting.

**Files:**
- Create: `ui/src/dialogs/SettingsDialog.tsx`
- Modify: `ui/src/shell/TitleBar.tsx` (optional Show menu), `ui/src/App.tsx` (toolbar Settings button, Show menu, Ctrl+,), `ui/src/api/protocol.ts` (`StateTree.engine.matteBackend`), `ui/src/app.css`
- Test: `ui/src/dialogs/SettingsDialog.test.tsx`

**Interfaces:**
- Consumes: `setSettings {rate?, resolution?, holdMs?, slatePath?}` (M1 Tasks 11, 11A); state `/engine/gpu`, `/engine/matteBackend`, `/engine/previewEncoder`, `/engine/warnings/*`; M1's `Modal`, `Prop`, `ConfirmDialog`, `ErrorBlock`, `Toolbar` (`extra` slot), `TitleBar`.
- Produces: `SettingsDialog({client, onClose})`; `TitleMenus.show?: (MenuItem | 'sep')[]` (rendered between File and Camera when given).

Rules: three categories — **Show** (Frame rate 50 / 59.94 / 60; Resolution 1280×720 / 1920×1080; "Hold the last frame on signal loss" in seconds 0–60; Slate image path with the hint "Leave empty for the built-in slate."), **Engine** (read-only: GPU, Background removal backend or "None", Preview encoder or "None", any engine warnings as warning blocks), **About** ("BreadCamera 0.1 — BM Solutions", "Third-party licences: THIRD_PARTY_NOTICES.md in the install folder."). **Save** sends only what changed; a change of frame rate or resolution asks first: title "Change the show format?", body "Every output restarts and loses picture for about 1 second.", button **Change format**. Cancel and Escape close without saving.

- [ ] **Step 1: Tests**

`ui/src/dialogs/SettingsDialog.test.tsx`:
```tsx
import { fireEvent, render, screen, within } from '@testing-library/react';

import { EngineClient } from '../api/client';
import { FakeWs } from '../test/fakeWs';
import { SettingsDialog } from './SettingsDialog';

function client() {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({
    type: 'snapshot',
    state: { engine: { gpu: 'AMD Radeon RX 6600', matteBackend: '', previewEncoder: 'h264_amf' } },
    show: { settings: { rate: '60000/1001', resolution: '1920x1080', holdMs: 5000, slatePath: '' }, cameras: [] },
  });
  return c;
}
const sent = (cmd: string) => FakeWs.last.sent.map((s) => JSON.parse(s)).filter((r) => r.cmd === cmd);

test('saves the hold time without asking', () => {
  render(<SettingsDialog client={client()} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Hold the last frame on signal loss (s)'), { target: { value: '8' } });
  fireEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(sent('setSettings')[0].args).toEqual({ holdMs: 8000 });
});

test('a resolution change asks first, with a verb button', () => {
  render(<SettingsDialog client={client()} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Resolution'), { target: { value: '1280x720' } });
  fireEvent.click(screen.getByRole('button', { name: 'Save' }));
  expect(sent('setSettings')).toHaveLength(0);
  fireEvent.click(within(screen.getByRole('dialog', { name: 'Change the show format?' })).getByRole('button', { name: 'Change format' }));
  expect(sent('setSettings')[0].args).toEqual({ resolution: '1280x720' });
});

test('the Engine page shows the GPU and the missing matte backend', () => {
  render(<SettingsDialog client={client()} onClose={() => {}} />);
  fireEvent.click(screen.getByRole('tab', { name: 'Engine' }));
  expect(screen.getByText('AMD Radeon RX 6600')).toBeInTheDocument();
  expect(screen.getByText('None')).toBeInTheDocument();
});
```

- [ ] **Step 2: Implement**

`ui/src/dialogs/SettingsDialog.tsx`:
```tsx
import { useState } from 'react';

import type { EngineClient } from '../api/client';
import { useEngine } from '../api/useEngine';
import { ErrorBlock } from '../components/ErrorBlock';
import { Modal } from '../components/Modal';
import { Prop } from '../components/Prop';
import { ConfirmDialog } from './ConfirmDialog';

const RATES = [
  ['50/1', '50'],
  ['60000/1001', '59.94'],
  ['60/1', '60'],
] as const;
const SIZES = [
  ['1280x720', '1280×720'],
  ['1920x1080', '1920×1080'],
] as const;
type Page = 'show' | 'engine' | 'about';

// Settings is a dialog, not a page (spec §6.4).
export function SettingsDialog({ client, onClose }: { client: EngineClient; onClose: () => void }) {
  const { show, state } = useEngine(client);
  const s = { rate: '60/1', resolution: '1920x1080', holdMs: 5000, slatePath: '', ...show?.settings };
  const [page, setPage] = useState<Page>('show');
  const [rate, setRate] = useState(s.rate);
  const [resolution, setResolution] = useState(s.resolution ?? '1920x1080');
  const [hold, setHold] = useState(String(s.holdMs / 1000));
  const [slate, setSlate] = useState(s.slatePath);
  const [confirm, setConfirm] = useState(false);
  const [error, setError] = useState('');
  const engine = state.engine ?? {};

  const holdMs = Math.round(Number(hold) * 1000);
  const holdError = !Number.isFinite(holdMs) || holdMs < 0 || holdMs > 60000 ? 'Hold is 0 to 60 seconds' : '';
  const formatChanged = rate !== s.rate || resolution !== (s.resolution ?? '1920x1080');

  async function save() {
    const args: Record<string, unknown> = {};
    if (rate !== s.rate) args.rate = rate;
    if (resolution !== (s.resolution ?? '1920x1080')) args.resolution = resolution;
    if (holdMs !== s.holdMs) args.holdMs = holdMs;
    if (slate !== s.slatePath) args.slatePath = slate;
    if (Object.keys(args).length === 0) return onClose();
    try {
      await client.request('setSettings', args);
      onClose();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  return (
    <Modal
      title="Settings"
      wide
      onClose={onClose}
      footer={
        <>
          <button className="btn" onClick={onClose}>
            Cancel
          </button>
          <button className="btn primary" disabled={!!holdError} onClick={() => (formatChanged ? setConfirm(true) : save())}>
            Save
          </button>
        </>
      }
    >
      <div className="settings-wrap">
        <div className="cats" role="tablist" aria-label="Settings pages">
          {(['show', 'engine', 'about'] as Page[]).map((p) => (
            <button key={p} role="tab" aria-selected={page === p} className={page === p ? 'on' : ''} onClick={() => setPage(p)}>
              {p === 'show' ? 'Show' : p === 'engine' ? 'Engine' : 'About'}
            </button>
          ))}
        </div>
        <div className="pane" role="tabpanel">
          {error && <ErrorBlock message={error} />}
          {page === 'show' && (
            <>
              <h3>Show format</h3>
              <Prop label="Frame rate">
                {(id) => (
                  <select id={id} className="ctl mono" value={rate} onChange={(e) => setRate(e.target.value)}>
                    {RATES.map(([v, l]) => (
                      <option key={v} value={v}>
                        {l}
                      </option>
                    ))}
                  </select>
                )}
              </Prop>
              <Prop label="Resolution" hint="Every camera is processed at this format. Each output can send a smaller size or half the frame rate.">
                {(id) => (
                  <select id={id} className="ctl mono" value={resolution} onChange={(e) => setResolution(e.target.value)}>
                    {SIZES.map(([v, l]) => (
                      <option key={v} value={v}>
                        {l}
                      </option>
                    ))}
                  </select>
                )}
              </Prop>
              <h3>Signal loss</h3>
              <Prop label="Hold the last frame on signal loss (s)" hint={holdError}>
                {(id) => <input id={id} className="ctl mono" type="number" min={0} max={60} step={0.5} value={hold} onChange={(e) => setHold(e.target.value)} />}
              </Prop>
              <Prop label="Slate image" hint="Leave empty for the built-in slate.">
                {(id) => <input id={id} className="ctl mono" value={slate} placeholder="C:\Shows\slate.png" onChange={(e) => setSlate(e.target.value)} />}
              </Prop>
            </>
          )}
          {page === 'engine' && (
            <>
              <h3>Engine</h3>
              {Object.values(engine.warnings ?? {})
                .filter((w) => w)
                .map((w) => (
                  <ErrorBlock key={w} severity="warning" message={w} />
                ))}
              <Prop label="GPU">{() => <span className="ro">{engine.gpu ?? '—'}</span>}</Prop>
              <Prop label="Background removal">{() => <span className="ro">{engine.matteBackend || 'None'}</span>}</Prop>
              <Prop label="Preview encoder">{() => <span className="ro">{engine.previewEncoder || 'None'}</span>}</Prop>
            </>
          )}
          {page === 'about' && (
            <>
              <h3>BreadCamera 0.1 — BM Solutions</h3>
              <p className="note">Third-party licences: THIRD_PARTY_NOTICES.md in the install folder.</p>
            </>
          )}
        </div>
      </div>
      {confirm && (
        <ConfirmDialog
          title="Change the show format?"
          body="Every output restarts and loses picture for about 1 second."
          confirm="Change format"
          danger={false}
          onCancel={() => setConfirm(false)}
          onConfirm={() => {
            setConfirm(false);
            save();
          }}
        />
      )}
    </Modal>
  );
}
```

`ui/src/api/protocol.ts`: add `matteBackend?: string;` to `StateTree.engine`.

`ui/src/shell/TitleBar.tsx`: `TitleMenus` gains `show?: (MenuItem | 'sep')[];` and, between File and Camera, `{menus.show && <Menu label="Show" items={menus.show} />}`.

`ui/src/App.tsx` (`Workspace`):
- the dialog state type gains `'settings'`;
- `menus.show = [{ label: 'Settings…', shortcut: 'Ctrl+,', onSelect: () => setDialog('settings') }]`;
- `<Toolbar … extra={<button className="tb" title="Settings (Ctrl+,)" onClick={() => setDialog('settings')}><Icon name="settings" />Settings</button>} />` (import `Icon`);
- in the keydown handler: `if (e.ctrlKey && e.key === ',') { e.preventDefault(); setDialog('settings'); }`;
- `{dialog === 'settings' && <SettingsDialog client={client} onClose={() => setDialog(null)} />}`.

`ui/src/app.css` — append:
```css
/* settings (M5) */
.settings-wrap { display: grid; grid-template-columns: 150px 1fr; min-height: 320px; margin: -6px -14px -14px; }
.cats { background: var(--bm-surface); border-right: 1px solid var(--bm-border); padding: 6px; display: grid; align-content: start; gap: 2px; }
.cats button { height: 28px; text-align: left; padding: 0 8px; border-radius: 4px; color: var(--bm-text-muted); }
.cats button.on { background: var(--bm-brand-wash); color: var(--bm-brand); font-weight: 600; }
.pane { padding: 12px 16px; display: grid; gap: 8px; align-content: start; }
.pane h3 { font-size: 12px; font-weight: 600; color: var(--bm-text); }
.pane .prop { grid-template-columns: 200px minmax(0, 1fr); }
```

Run `npm test` and `npm run build` in `ui/`.

- [ ] **Step 3: Commit**

```bash
git add ui/src
git commit -m "feat(ui): Settings dialog with show format (confirmed), signal loss and engine info"
```

---

### Task 2: Error headlines for every engine code

**Files:**
- Create: `ui/src/errors.ts`, `ui/src/errors.test.ts`
- Modify: `ui/src/components/ErrorBlock.tsx` (accept `error?: ErrorInfo`), every `ErrorBlock` call site that shows an engine `ErrorInfo` (Inspector Input/Outputs, Effects, ColourPanel)

**Interfaces:**
- Produces: `errorText(e: ErrorInfo): { headline: string; detail: string }`; `ErrorBlock` props become `{ title?, message?, error?: ErrorInfo, severity?, label? }` (when `error` is given, the headline is the bold title and the engine message the detail).

Headlines (English only): `input-failed` "The source could not be opened."; `input-ended` "The source stopped sending."; `input-config` "The source settings are not valid."; `output-failed`, `srt-out-failed`, `ndi-failed`, `omt-failed`, `decklink-failed`, `aja-failed`, `vcam-failed`, `fullscreen-failed` "The output failed and is retrying."; `output-config` "The output settings are not valid."; `stage-failed` "A processing step failed and is bypassed."; `stage-overrun` "A processing step is too slow and is bypassed."; `matte-unavailable` "Background removal cannot run on this PC."; `background-image` "The background image cannot be read; using the colour."; `color-lut` "The colour correction file cannot be read; the camera runs uncorrected."; `ndi-no-video` "The NDI source sends no video."; `ndi-audio-rate` "NDI audio is not 48 kHz and is ignored."; `decklink-no-signal` "No signal on the DeckLink input."; `capture-failed` "The camera could not be opened (USB or built-in)."; any other code "Something went wrong.".

- [ ] **Step 1: Tests**

`ui/src/errors.test.ts`:
```ts
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
```

- [ ] **Step 2: Implement**

`ui/src/errors.ts`:
```ts
import type { ErrorInfo } from './api/protocol';

const OUTPUT = ['output-failed', 'srt-out-failed', 'ndi-failed', 'omt-failed', 'decklink-failed', 'aja-failed', 'vcam-failed', 'fullscreen-failed'];
const HEADLINES: Record<string, string> = {
  'input-failed': 'The source could not be opened.',
  'input-ended': 'The source stopped sending.',
  'input-config': 'The source settings are not valid.',
  'output-failed': 'The output failed and is retrying.',
  'output-config': 'The output settings are not valid.',
  'stage-failed': 'A processing step failed and is bypassed.',
  'stage-overrun': 'A processing step is too slow and is bypassed.',
  'matte-unavailable': 'Background removal cannot run on this PC.',
  'background-image': 'The background image cannot be read; using the colour.',
  'color-lut': 'The colour correction file cannot be read; the camera runs uncorrected.',
  'ndi-no-video': 'The NDI source sends no video.',
  'ndi-audio-rate': 'NDI audio is not 48 kHz and is ignored.',
  'decklink-no-signal': 'No signal on the DeckLink input.',
  'capture-failed': 'The camera could not be opened (USB or built-in).',
};

// A plain headline for the operator; the engine's own message stays underneath as the detail.
export function errorText(e: ErrorInfo): { headline: string; detail: string } {
  const code = OUTPUT.includes(e.code) ? 'output-failed' : e.code;
  return { headline: HEADLINES[code] ?? 'Something went wrong.', detail: e.message ?? '' };
}
```

`ui/src/components/ErrorBlock.tsx`: add `error?: ErrorInfo` to the props (import the type); when present, `title = errorText(error).headline` and `message = errorText(error).detail`. Change the call sites that pass `xxx.error.message` with a hand-written title (Inspector "Input failed." / "<output> failed.", Effects "<effect> bypassed.") to pass `error={xxx.error}` instead.

Run `npm test` and `npm run build`.

- [ ] **Step 3: Commit**

```bash
git add ui/src
git commit -m "feat(ui): plain headlines for every engine error code"
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
2. The app starts: the dark custom title bar with menus, the empty Multiview ("No cameras yet"), the Inspector, and the GPU name in the status bar. Drag the window by its title bar; minimise, maximise and restore it.
3. Add an SRT listener camera named "Sân khấu trái" (Unicode); push a stream from OBS (`srt://<pc>:9000?mode=caller`) → the input chip turns green within 5 s, preview moving, the name shown unchanged.
4. Add an NDI output (if the NDI runtime is installed) and see it in vMix/OBS; add a Windows virtual camera output and see it in the Camera app with the camera's name.
5. Close the window → outputs keep running (tray icon). Tray → "Quit…" → the dialog's "Quit and stop all outputs" stops everything.
6. Uninstall from Settings → Apps; `%APPDATA%\BreadCamera\shows` is still there.
```

- [ ] **Step 3: Follow-ups**

Append to `docs/superpowers/follow-ups.md`:
```markdown
## M5
- The 8-hour soak (`scripts/soak.ps1 -Hours 8 -Faults`) is queued as its own night run; M5 carries the 30-minute evidence.
- Fresh-install check is a human gate: `docs/superpowers/manual-checks/m5-fresh-install.md`.
- Engine error messages stay as detail lines under the plain headlines (`ui/src/errors.ts`).
```

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers
git commit -m "docs: M5 fresh-install check and follow-ups"
```
