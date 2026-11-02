# Native video previews (BreadReplay) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** BreadReplay's Preview, Output and angle tiles are presented natively through BreadCamera's `SurfacePresenter` / `SurfaceHub` / `CompositionHost`, with H.264 and raw angle frames kept only for fallback clients.

**Architecture:** `engine_main.cpp` owns a `bc::SurfaceHub` and answers `subscribeSurfaces` with the client id. `Engine::previewLoop` presents its linear pictures to wanted ids, and encodes or sends raw frames only while a fallback client has asked for them. The shell adopts `bc::CompositionHost`, and the UI gets its own `NativeVideo` copy.

**Tech Stack:** C++20, D3D11, DirectComposition, WebView2 1.0.4258, doctest; React + TypeScript + vitest.

**Spec:** `F:\Coding\BreadMedia\BreadCamera\docs\superpowers\specs\2026-11-02-native-video-previews-design.md` (binding; §4.5, §5, §6 and §10 step 5).

**Starts only when both are true:** (1) BreadCamera branch `bcam-native-video` is merged into BreadCamera `main` (plan `2026-11-02-native-video-previews.md`); (2) the parallel BreadReplay UX work (live-edge Preview, angle-tile colours, timeline labels, custom context menus) is merged into BreadReplay `main` and `git status` in BreadReplay is clean. If either is not true, stop and report.

## Global Constraints

- Repo `F:\Coding\BreadMedia\BreadReplay`, branch `br-native-video` from `main`. Never push. Never use `git worktree`. BreadCamera files are used, not edited. If one needs a change, stop and report it.
- Verification command (every task): `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1` then `./build/release/br_tests.exe`. UI tasks also run `npm --prefix ui test` and `npm --prefix ui run build` (Node 24, `PATH="/c/Program Files/nodejs:$PATH"`).
- Surface ids: `preview`, `output`, `angle:<inputId>`. The 33 ms `previewLoop` stays. Angles present every tick when native, and every 3rd tick (today's rate) for raw fallback frames.
- `previewLoop` never waits on a present (Global Constraints of the BreadCamera plan apply to every `SurfaceHub::present` call).
- Binary preview messages (`encodePreviewMessage`, `BRAP`) go only to clients that sent `wantPreviews`. The H.264 encoders open lazily on the first such client and close when none is left.

## Review Focus

1. **The shell's own heartbeat socket** must not count as a preview client: no encode runs when only the shell and a native page are connected. Task 1 test.
2. **An angle input added or removed while subscribed:** the page resubscribes with the new id list. A removed input's presenter is dropped and no present throws. Task 1 test plus Task 3 vitest.
3. **The Output player with nothing on air** (`lastPicture()` null): nothing is presented and the visual keeps its last frame. No throw. Task 1 test.
4. **Engine restart while the operator is mid-review:** the page reconnects and resubscribes, and the shell gets layers with the new pid. Task 3 vitest (reconnect resends), Task 4 acceptance.
5. **Hotkeys after the switch to composition hosting** (the operator UI is keyboard-driven). Task 4 acceptance.

---

### Task 1: Engine presents natively, encodes only for fallback clients

**Files:**
- Modify: `CMakeLists.txt` (`br_engine` adds `${BR_BREADCAMERA_DIR}/src/engine/surface_hub.cpp`), `src/engine/engine.h` / `engine.cpp` (`setSurfaces`, `setBinaryWanted`, `previewLoop`, `statusJson`), `src/engine/engine_main.cpp`
- Test: `tests/engine_test.cpp`

**Interfaces:**
- Consumes: `bc::SurfaceHub` (`subscribe`, `dropClient`, `wanted`, `present`), `bc::SurfacePresenter::available`, `bc::SurfaceRequest`/`SurfaceGrant` — all from BreadCamera `main`.
- Produces:
  ```cpp
  void Engine::setSurfaces(bc::SurfaceHub* hub);            // null = native video off
  void Engine::setBinaryWanted(std::function<bool()> fn);   // true while any client sent wantPreviews
  // statusJson() gains "nativeVideo": true | "<reason>"
  ```
  Control API, handled in `engine_main.cpp`'s text handler before `engine.handle(...)` (it needs the client id):
  - `{"cmd":"subscribeSurfaces","surfaces":[{"id","w","h"}]}` → `{"ok":true,"pid":<pid>,"surfaces":[{"id","handle":"<decimal>","w","h"}]}`. Same validation as BreadCamera's router (list, text ids, positive numeric w/h, at most 64), otherwise `{"ok":false,"error":…}`.
  - `{"cmd":"wantPreviews"}` → `{"ok":true}`, and adds the client to `binaryClients`. Close removes it and calls `surfaces.dropClient`.

- [ ] **Step 1: Write the failing tests** in `engine_test.cpp` (`BC_REQUIRES(HardwareGpu)`, the existing engine fixture):
  - `Engine: no binary preview without a wantPreviews client`: `setBinaryWanted([]{return false;})` and run the loop ~200 ms → the binary sender is never called.
  - `Engine: binary previews resume for a fallback client`: flip the function to true → the sender receives a `preview` message within 500 ms.
  - `Engine: presents to wanted surfaces`: a real `bc::SurfaceHub` with `subscribe("c", {{"preview",320,180},{"output",320,180}})` and recorded input → after 300 ms, no exception logged and `hub.anyBroken()` is false.
  - `Engine: output with nothing on air presents nothing and does not throw`.
  - `Engine: statusJson reports nativeVideo`.
- [ ] **Step 2: Run** `br_tests.exe -tc="Engine: *wantPreviews*,Engine: presents*,Engine: output with nothing*,Engine: statusJson reports nativeVideo"`. Expected: build fails.
- [ ] **Step 3: Implement** per spec §4.5. On BreadReplay `main` (3f337b9, e13486f), `previewLoop` has three `guarded(...)` parts (preview / output / angles), each logged once, plus `followLiveEdge()`. Put each present inside its own part's `guarded` block, so a failing present blanks only that part. Keep `followLiveEdge()` and the per-segment frame sizes (`InputStore::frameSize`) as they are; `SurfacePresenter` letterboxes any size. Preview: render as today, then `surfaces_->present("preview", *pic)` when wanted. `toCpu` + encode only when `binaryWanted_()`. Output: `present("output", *pic)`. Angles: when any `angle:` id is wanted, `uploadBgra8` → `toLinear(…, bc::PixelLayout::BGRA8, true)` → `present("angle:" + id, …)` every tick. Build the `BRAP` message only when `binaryWanted_()`, on the existing every-3rd-tick cadence. Create the encoders lazily on the first tick where `binaryWanted_()` is true, and reset them when it turns false. A new encoder's first frame is a keyframe. Keep `Engine::requestKeyframes()` and `engine_main`'s call to it on connect (BreadReplay `main`, "a window that connects late gets Preview pictures at once"); also call it when a client sends `wantPreviews`, so a second fallback window gets a picture at once.
- [ ] **Step 4: Wire `engine_main.cpp`:** the self-test result goes into the engine (for `statusJson`), `bc::SurfaceHub surfaces(*gpu, conv)`, `engine.setSurfaces(available ? &surfaces : nullptr)`, a `binaryClients` set guarded by the existing `clientsMutex` (the binary sender iterates it instead of `clients`), and `setBinaryWanted`.
- [ ] **Step 5: Run** the new tests, then the full verification command.
- [ ] **Step 6: Commit** — `feat(engine): native Preview, Output and angle surfaces; encode only for fallback clients`.

### Task 2: Shell adopts `CompositionHost`

**Files:**
- Modify: `CMakeLists.txt` (the `BreadReplay` target adds `${BR_BREADCAMERA_DIR}/src/shell/composition_host.cpp` and links `dcomp uiautomationcore`), `src/shell/shell_main.cpp`

**Interfaces:**
- Consumes: `bc::CompositionHost`, `bc::parseVideoLayers`, `ProcessLauncher::identity()` (all from BreadCamera `main`; it returns `{pid, process}` read together under the launcher lock, and the caller must `CloseHandle(process)`). Pass `identity().pid` to `parseVideoLayers` and `identity().process` to `setLayers` from the same call, never from two separate reads.

- [ ] **Step 1: Mirror BreadCamera's Task 4 Step 4** in BreadReplay's `shell_main.cpp`: DPI awareness and scaled initial size, `WM_DPICHANGED`, the background brush from BreadReplay's `--bm-bg`, `CompositionHost::create` with the `BC_NO_COMPOSITION=1` kill switch and the windowed fallback, `&nativeVideo=1`, `handleMessage` first in `wndProc`, minimize visibility, and JSON-first `WebMessageReceived` routing for `videoLayers`. Keep BreadReplay's own differences: no tray, the TaskDialog quit, re-attach via `engine_discovery`, and the WebView2 settings that turn off browser accelerator keys, zoom control and the status bar (they apply to the composition controller's `ICoreWebView2Settings` the same way). Right-button down/up must be forwarded like the left button (spec §5.1), so the page's `onContextMenu` menus (`ui/src/ContextMenu.tsx`) still open on the monitors and angle tiles. When adopting, the launcher's `adopt` already opens the engine with `PROCESS_DUP_HANDLE`.
- [ ] **Step 2: Run** the verification command. Launch `build/release/BreadReplay.exe` and record in the ledger whether composition hosting started and whether the UI and hotkeys respond.
- [ ] **Step 3: Commit** — `feat(shell): composition-hosted WebView2 with native video layers`.

### Task 3: `NativeVideo` in BreadReplay's UI

**Files:**
- Create: `ui/src/screen/NativeVideo.tsx`, `ui/src/screen/nativeVideoRegistry.ts`, `ui/src/screen/NativeVideo.test.tsx`
- Modify: `ui/src/api/client.ts` (`subscribeSurfaces`, `wantPreviews` on open for fallback mode, resend on reconnect), `ui/src/api/types.ts` (`nativeVideo?: true | string` on the status), `ui/src/screen/Monitors.tsx`, `ui/src/screen/AngleRow.tsx`, `ui/src/App.tsx`, `ui/src/app.css`

**Interfaces:**
- Same exports and behaviour as BreadCamera's `ui/src/preview/NativeVideo.tsx` and `nativeVideoRegistry.ts` (copy them, then adapt to BreadReplay's `Client.send(cmd): Promise<Reply>`). `native` = `nativeVideoRequested() && status?.nativeVideo === true`. When not native, the client sends `{"cmd":"wantPreviews"}` on every open.

- [ ] **Step 1: Write the failing tests:** BreadCamera's six `NativeVideo` cases, ported with ids `preview`, `output` and `angle:<id>`, plus:
  - `fallback mode sends wantPreviews on open and on reconnect`;
  - `native mode never sends wantPreviews`;
  - `the angle list changing resubscribes with the new ids`.
- [ ] **Step 2: Run** `npm --prefix ui test -- NativeVideo`. Expected: FAIL.
- [ ] **Step 3: Implement and use it.** `Monitors`: `<NativeVideo id="preview" … fallback={<MonitorCanvas …/>} />`, and the same for `output`. `AngleRow`: one per input inside the button, with the canvas as the fallback. The `.tag`, `.tc`, `.ident` and `.remain` overlays stay on top. Run the transparency pass (spec §6.2) under `html.native-video`, and fix every dev warning in native mode.
- [ ] **Step 4: Run** `npm --prefix ui test && npm --prefix ui run build`, then the verification command.
- [ ] **Step 5: Commit** — `feat(ui): native Preview, Output and angle tiles`.

### Task 4: BreadReplay acceptance (owner judges)

- [ ] **Step 1:** Run the full verification command and the UI checks. Record the counts.
- [ ] **Step 2:** Run the app with 4+ recorded inputs. Record whether `nativeVideo` is true, whether each tile shows a picture, and engine CPU and GPU % over 30 s, both native and with `BC_NO_COMPOSITION=1`.
- [ ] **Step 3:** Kill and relaunch the shell 10 times (re-attach via `engine.json`). Each time, check that the engine survives and that the engine `HandleCount` grows by no more than 20 across the 10 runs. Record it.
- [ ] **Step 4: Angle streaks.** Moving sources showed horizontal streaks on the old raw-BGRA angle tiles. With native tiles, record whether a moving source's angle tile still streaks. Native tiles upload the same `FrameSource::preview` image, so streaks are expected to remain: the other session traced them to the `FrameSource::preview` (VMX DC-prefix) image under motion. Interlacing is ruled out (progressive testsrc2/mandelbrot sources), and the Preview monitor decodes the same recordings cleanly. Log it as a finding and do not fix it in this plan.
- [ ] **Step 5:** Copy spec §9 owner items 1–8 into the report, adapted to BreadReplay in the UI's own words (the Preview monitor, the ON AIR monitor, the Camera tiles, the right-click menus over video, operator hotkeys, timeline wheel and scrub). Code, engine and command names are unchanged (`output`, `angle:<id>`, `Monitors`, `AngleRow`). Unchecked: the owner judges them.
