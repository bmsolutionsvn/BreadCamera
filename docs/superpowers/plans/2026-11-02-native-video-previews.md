# Native video previews (BreadCamera) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** BreadCamera's camera previews are presented by the engine straight into DirectComposition swap chains that sit under a transparent, composition-hosted WebView2. No readback, no encode and no decode for previews, and the H.264 path stays as the fallback.

**Architecture:** The engine owns one `SurfacePresenter` (surface handle + swap chain) per (client, preview id), tracked by `SurfaceHub` and driven from the frame tap. The page measures `NativeVideo` boxes, subscribes over the control WebSocket and relays `{pid, handle, rect}` to the shell. The shell's `CompositionHost` duplicates each handle out of the engine and places a visual under the WebView visual.

**Tech Stack:** C++20 (MSVC, VS 2026), D3D11/DXGI 1.3, DirectComposition (`dcomp.lib`), WebView2 SDK 1.0.4258 (composition controller), doctest; React + TypeScript + vitest in `ui/`.

**Spec:** `docs/superpowers/specs/2026-11-02-native-video-previews-design.md` (binding). This plan covers the BreadCamera half (spec §10 steps 1–4). BreadReplay's half is `docs/superpowers/plans/2026-11-02-native-video-previews-breadreplay.md` and runs after this one merges.

## Global Constraints

- Everything in "Global Constraints" of `docs/superpowers/plans/2026-10-11-remaining-milestones-opencode.md` applies (`/W4 /permissive- /utf-8`, doctest, `BC_REQUIRES(...)` skips, control API on 127.0.0.1 with the token, conventional commits, one commit per task).
- Branch `bcam-native-video` from `main`. Never push. Never use `git worktree`.
- Verification command (every task), from `docs/superpowers/plans/2026-10-11-remaining-milestones-opencode.md`:
  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
  ./build/release/bc_tests.exe '--test-case-exclude=*acceptance*,ControlServer: refuses a wrong token*,ControlServer: hostile messages*,FfmpegInput: an SRT listener*,FfmpegInput: stop() returns promptly*'
  ```
  A task that touches `ui/` also runs `cd ui && npm test && npm run build` with `PATH="/c/Program Files/nodejs:$PATH"`.
- No tick or frame-tap call may wait for vsync or for a present: `Present(0, DXGI_PRESENT_DO_NOT_WAIT)`, and `DXGI_ERROR_WAS_STILL_DRAWING` drops the frame.
- Swap chains are `DXGI_FORMAT_R8G8B8A8_UNORM`, filled from `Converter::toBgra8(…, swapRb = false)` (true RGBA). Never `B8G8R8A8` and never `swapRb = true` on this path.
- Lock order: `SurfaceHub` mutex, then `GpuDevice::lock()`.
- Surface ids: `cam:<cameraId>`. Sizes are device pixels, clamped to 16..4096.
- The shell duplicates handles only from the engine it supervises, and only when the message's `pid` equals that engine's pid. At most 64 layers.
- The H.264 path (`PreviewService`, `PreviewHub`, `subscribePreview`, `requestKeyframe`, `PreviewCanvas`) is kept unchanged as the fallback.
- Out of scope (spec §2): touch/pen forwarding, drag-and-drop forwarding, HDR, remote UI, BreadReplay files.

## Review Focus

1. **Shell restarted while the engine runs:** the new page gets new surfaces and the old client's presenters are destroyed on socket close. No leaked handles or presenters after 10 shell restarts. Owned by Task 3 (`dropClient` test) and Task 6 (acceptance).
2. **A stale or hostile `videoLayers` message** (wrong pid, 1000 layers, negative or huge rects, non-numeric handle, missing fields): the shell ignores it or drops only the bad layer and never crashes. Owned by Task 4 (`parseVideoLayers` tests).
3. **A box resized to 0×0, hidden with `display:none`, or the window minimized:** that surface leaves the subscription and nothing presents to it. Owned by Task 5 (vitest) and Task 3 (empty subscription test).
4. **A camera removed while its tile is subscribed:** the presenter stays (no frames) until the page drops it. Nothing throws on the tick. Owned by Task 3 (present to an id with no frame).
5. **Lost keyboard focus or a broken title bar after switching to composition hosting:** hotkeys, Alt+Tab back, title-bar drag and double-click maximize. Owned by Task 6 (acceptance checklist). A unit test cannot reach it.

---

### Task 1: Spike — cross-process composition surface (gate)

**Files:**
- Create: `tools/native_video_probe/probe_main.cpp`
- Modify: `CMakeLists.txt` (new target after `bc_soak`, not inside `BC_LIBS_ONLY`)

**Interfaces:**
- Produces: `bc_native_video_probe.exe` (no arguments = full self-check; `serve` = child mode). Exit code 0 = PASS, 1 = FAIL, 2 = could not judge (no interactive desktop).

- [ ] **Step 1: Implement the probe.** One file, no BreadCamera libraries except `bc_gpu` for `GpuDevice::create(Pref::Hardware)`.
  - `serve` mode: create the device, `DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, nullptr, &h)`, `IDXGIFactoryMedia::CreateSwapChainForCompositionSurfaceHandle` (R8G8B8A8_UNORM, FLIP_SEQUENTIAL, 2 buffers, 256×256, ALPHA_MODE_IGNORE). Clear to magenta `(1,0,1,1)` and present every 16 ms. Print `<pid> <handle-as-decimal>\n` to stdout, flush, then run until stdin closes.
  - Default mode: launch itself with `serve` via `CreateProcessW` with redirected stdin/stdout pipes, and read the line. Create a 256×256 visible top-level window. Use `OpenProcess(PROCESS_DUP_HANDLE, …)`, `DuplicateHandle`, `DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(IDCompositionDesktopDevice))`, `CreateTargetForHwnd(hwnd, TRUE)`, `CreateSurfaceFromHandle`, `visual->SetContent`, `SetRoot` and `Commit`. Pump messages for 1.5 s, then `BitBlt` the window's client rect from `GetDC(nullptr)` and read the centre pixel.
  - Print `PASS` (R>200, G<60, B>200) and exit 0. If the capture is all black and `OpenInputDesktop` fails (locked or no desktop), print `UNJUDGED: no interactive desktop` and exit 2. Otherwise print `FAIL: <step that failed with its HRESULT, or the pixel>` and exit 1. Close the child's stdin and wait for it to exit.
- [ ] **Step 2: Add the target.** `add_executable(bc_native_video_probe tools/native_video_probe/probe_main.cpp)`, linking `bc_gpu dcomp user32 gdi32`.
- [ ] **Step 3: Build and run.** Run the verification command's build line, then `./build/release/bc_native_video_probe.exe`.
  Expected: `PASS`, exit 0.
  - Exit 1 (`FAIL`): **stop the whole run.** Write the output to the ledger as a blocker and report. The owner decides on plan B (spec §3: shared NT texture, with the shell owning the swap chains). Do not start Task 2.
  - Exit 2 (`UNJUDGED`): record it in the ledger. Continue only if every step before the capture succeeded (all HRESULTs S_OK). The owner runs the probe by hand before Task 6.
- [ ] **Step 4: Commit** `tools/native_video_probe/probe_main.cpp CMakeLists.txt` — `feat(tools): cross-process DirectComposition surface probe`.

### Task 2: `SurfacePresenter`

**Files:**
- Create: `src/gpu/surface_presenter.h`, `src/gpu/surface_presenter.cpp`
- Modify: `CMakeLists.txt` (`bc_gpu` sources; `bc_gpu` links `dcomp`)
- Test: `tests/surface_presenter_test.cpp` (add to `bc_tests`)

**Interfaces:**
- Consumes: `bc::GpuDevice` (`device()`, `context()`, `lock()`), `bc::Converter` (`fit`, `toBgra8`), `bc::GpuTexture`.
- Produces (namespace `bc`):
  ```cpp
  class SurfacePresenter {
   public:
    SurfacePresenter(GpuDevice& gpu, Converter& conv, uint32_t w, uint32_t h);  // throws std::runtime_error
    ~SurfacePresenter();                    // closes the surface handle
    HANDLE handle() const;                  // engine-process value
    uint32_t width() const; uint32_t height() const;  // the size the next present will use
    void resize(uint32_t w, uint32_t h);    // clamped 16..4096; applied by the next present
    bool present(const GpuTexture& linear); // false = dropped or broken; never throws, never waits
    bool broken() const;
    static bool available(GpuDevice& gpu, Converter& conv, std::string* reason);  // 16x16 create + one present
    IDXGISwapChain1* swapChainForTest() const;
  };
  ```

- [ ] **Step 1: Write the failing tests** (all `* BC_REQUIRES(bc::test::Req::HardwareGpu)`):
  ```cpp
  TEST_CASE("SurfacePresenter: creates a handle and presents a picture") {
    // gpu, pool, conv as in gpu_test.cpp; linear = conv.solid(320, 180, {1, 0, 0, 1})
    bc::SurfacePresenter p(*gpu, conv, 200, 100);
    CHECK(p.handle() != nullptr);
    CHECK(p.present(*linear));          // first present is never "still drawing"
    CHECK_FALSE(p.broken());
  }
  TEST_CASE("SurfacePresenter: resize takes effect on the next present") {
    bc::SurfacePresenter p(*gpu, conv, 200, 100);
    p.resize(640, 360);
    p.present(*linear);
    DXGI_SWAP_CHAIN_DESC1 d{}; p.swapChainForTest()->GetDesc1(&d);
    CHECK(d.Width == 640); CHECK(d.Height == 360);
    CHECK(d.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
  }
  TEST_CASE("SurfacePresenter: sizes are clamped") {
    bc::SurfacePresenter p(*gpu, conv, 1, 99999);
    CHECK(p.width() == 16); CHECK(p.height() == 4096);
  }
  TEST_CASE("SurfacePresenter: available() reports true on a hardware GPU") {
    std::string why;
    CHECK(bc::SurfacePresenter::available(*gpu, conv, &why));
    CHECK(why.empty());
  }
  ```
- [ ] **Step 2: Run** `bc_tests.exe -tc="SurfacePresenter*"`. Expected: build fails (no `surface_presenter.h`).
- [ ] **Step 3: Implement** per spec §4.1. `present` holds `gpu.lock()` for `fit` → `toBgra8(fit, w, h, false)` → `GetBuffer(0)` → `CopyResource` → `Present(0, DXGI_PRESENT_DO_NOT_WAIT)`. Apply a pending size first with `ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)` (release no buffer references before it). `DXGI_ERROR_WAS_STILL_DRAWING` → return false. Any other failure → `broken_ = true` and return false. Catch every exception inside `present`.
- [ ] **Step 4: Run** `bc_tests.exe -tc="SurfacePresenter*"` → 4 passed. Then the full verification command passes.
- [ ] **Step 5: Commit** — `feat(gpu): SurfacePresenter presents into a composition swap chain`.

### Task 3: `SurfaceHub`, `subscribeSurfaces`, frame-tap presenting

**Files:**
- Create: `src/engine/surface_hub.h`, `src/engine/surface_hub.cpp`
- Modify: `CMakeLists.txt` (`bc_engine` sources), `src/engine/command_router.h` / `.cpp` (command + `Effects` method), `src/engine/engine_main.cpp` (hub, self-test state, frame tap, close handler, `Fx`)
- Test: `tests/surface_hub_test.cpp` (add to `bc_tests`), `tests/command_router_test.cpp`

**Interfaces:**
- Consumes: `SurfacePresenter` (Task 2), `FrameTap` (`id`, `tex`), `ControlServer::Handlers::close`.
- Produces (namespace `bc`):
  ```cpp
  struct SurfaceRequest { std::string id; uint32_t w = 0, h = 0; };
  struct SurfaceGrant { std::string id; uint64_t handle = 0; uint32_t w = 0, h = 0; };
  class SurfaceHub {
   public:
    SurfaceHub(GpuDevice& gpu, Converter& conv);
    std::vector<SurfaceGrant> subscribe(const ClientId& client, const std::vector<SurfaceRequest>& wanted);  // replaces the set
    void dropClient(const ClientId& client);
    std::set<std::string> wanted() const;
    void present(const std::string& id, const GpuTexture& linear);  // every presenter for id; >=15 ms apart per presenter
    bool anyBroken() const;
    size_t presenterCount() const;  // tests
  };
  // CommandRouter::Effects, non-pure; the default returns {"pid":0,"surfaces":[]}:
  virtual nlohmann::json subscribeSurfaces(const ClientId& client, std::vector<SurfaceRequest> wanted);
  ```
  Wire reply of `subscribeSurfaces`: `{"pid": <engine pid>, "surfaces": [{"id", "handle": "<decimal string>", "w", "h"}]}`.
  State: `/engine/nativeVideo` = `true` or the reason string. `/engine/warnings/nativeVideo` = `"a preview stopped presenting"` while `anyBroken()`, otherwise erased.

- [ ] **Step 1: Write the failing tests.**
  `surface_hub_test.cpp` (`BC_REQUIRES(HardwareGpu)`):
  ```cpp
  TEST_CASE("SurfaceHub: subscribe creates, replaces and resizes") {
    bc::SurfaceHub hub(*gpu, conv);
    auto g = hub.subscribe("c1", {{"cam:A", 320, 180}, {"cam:B", 320, 180}});
    REQUIRE(g.size() == 2); CHECK(g[0].handle != 0); CHECK(hub.presenterCount() == 2);
    auto g2 = hub.subscribe("c1", {{"cam:A", 640, 360}});
    CHECK(hub.presenterCount() == 1);
    CHECK(g2[0].handle == g[0].handle);       // kept, only resized
    CHECK(g2[0].w == 640);
    CHECK(hub.wanted() == std::set<std::string>{"cam:A"});
  }
  TEST_CASE("SurfaceHub: an empty subscription and dropClient free everything") {
    bc::SurfaceHub hub(*gpu, conv);
    hub.subscribe("c1", {{"cam:A", 320, 180}});
    hub.subscribe("c2", {{"cam:A", 320, 180}});
    CHECK(hub.presenterCount() == 2);          // per client, never shared
    hub.subscribe("c1", {});
    CHECK(hub.presenterCount() == 1);
    hub.dropClient("c2");
    CHECK(hub.presenterCount() == 0);
    CHECK(hub.wanted().empty());
  }
  TEST_CASE("SurfaceHub: present to an id nobody watches is a no-op") {
    bc::SurfaceHub hub(*gpu, conv);
    hub.present("cam:Gone", *linear);         // no throw
    CHECK_FALSE(hub.anyBroken());
  }
  ```
  `command_router_test.cpp` (fake `Effects` records the call):
  ```cpp
  TEST_CASE("CommandRouter: subscribeSurfaces passes ids and sizes") {
    // args {"surfaces":[{"id":"cam:A","w":320,"h":180}]} → fake receives one request {cam:A,320,180}
  }
  TEST_CASE("CommandRouter: subscribeSurfaces rejects bad input") {
    // each of: no "surfaces"; surfaces not a list; id not text; w or h missing, negative or not a number;
    // more than 64 entries → reply error code "bad-request"
  }
  ```
- [ ] **Step 2: Run** `bc_tests.exe -tc="SurfaceHub*,CommandRouter: subscribeSurfaces*"`. Expected: build fails.
- [ ] **Step 3: Implement `SurfaceHub`** per spec §4.2 and the lock order in Global Constraints. A presenter whose creation throws is left out of the grant list (the page shows "no picture" for it), and `subscribe` does not throw for it.
- [ ] **Step 4: Implement the command** in `CommandRouter::handle`, next to `subscribePreview`, with `CmdError("bad-request", …)` for the cases tested.
- [ ] **Step 5: Wire `engine_main.cpp`:**
  - After `GpuDevice` and `Converter` exist: `std::string why; state.set("/engine/nativeVideo", bc::SurfacePresenter::available(*gpu, conv, &why) ? json(true) : json(why));`
  - `bc::SurfaceHub surfaces(*gpu, conv);`. `Fx::subscribeSurfaces` returns `{pid = GetCurrentProcessId(), surfaces}`, with handles formatted as decimal strings.
  - Frame tap: keep `previews.onTick(...)`, then for each `FrameTap` with `tex`, `surfaces.present("cam:" + f.id, *f.tex)` when `surfaces.wanted()` contains it. Before that, confirm `tex->format == Converter::kLinearFormat`. If it is not, convert it with `toLinear` first, and note the finding in the commit body.
  - The close handler also calls `surfaces.dropClient(id)`.
  - Set or erase `/engine/warnings/nativeVideo` from `surfaces.anyBroken()` in the main loop, next to the other state updates.
  - `setPreviewWanted` is unchanged: a native page never subscribes to `PreviewHub`, so the D5 readback is no longer forced for it.
- [ ] **Step 6: Run** the new tests (5 pass), then the full verification command.
- [ ] **Step 7: Commit** — `feat(engine): SurfaceHub and subscribeSurfaces present camera previews natively`.

### Task 4: `CompositionHost` and shell changes

**Files:**
- Create: `src/shell/composition_host.h`, `src/shell/composition_host.cpp`
- Modify: `CMakeLists.txt` (`bc_shell` gains the source and links `unofficial::webview2::webview2 WIL::WIL dcomp uiautomationcore`), `src/shell/shell_main.cpp`, `src/shell/process_launcher.cpp` (`adopt` adds `PROCESS_DUP_HANDLE`), `src/shell/process_launcher.h` (`HANDLE processHandle() const`)
- Test: `tests/composition_host_test.cpp` (add to `bc_tests`)

**Interfaces:**
- Consumes: `ProcessLauncher::pid()`, new `ProcessLauncher::processHandle()`.
- Produces (namespace `bc`):
  ```cpp
  struct VideoLayer { std::string id; uint64_t handle = 0; int x = 0, y = 0, w = 0, h = 0; };
  // nullopt when the message is not for this engine (pid mismatch or malformed root). Drops bad layers
  // (missing or non-numeric fields, w/h outside 1..16384, |x|,|y| > 32768) and keeps at most 64.
  std::optional<std::vector<VideoLayer>> parseVideoLayers(const nlohmann::json& msg, DWORD enginePid);
  class CompositionHost {
   public:
    // Creates DComp + the composition controller; calls done(controller) or done(nullptr) on failure.
    static void create(HWND hwnd, ICoreWebView2Environment* env,
                       std::function<void(std::unique_ptr<CompositionHost>, ICoreWebView2Controller*)> done);
    // WndProc hook: returns true (and sets result) when it handled the message.
    bool handleMessage(HWND, UINT msg, WPARAM, LPARAM, LRESULT* result);
    void setLayers(const std::vector<VideoLayer>& layers, HANDLE engineProcess);  // posts failures via onLayerFailed
    void setVisible(bool visible);
    void setDpi(UINT dpi);
    std::function<void(const std::string& id)> onLayerFailed;
  };
  ```
  Shell → page failure message: `{"type":"videoLayerFailed","id":"<id>"}`.

- [ ] **Step 1: Write the failing tests** for the pure parser:
  ```cpp
  TEST_CASE("parseVideoLayers: accepts a well-formed message for this engine") {
    auto m = json::parse(R"({"type":"videoLayers","pid":42,"layers":[{"id":"cam:A","handle":"1234","x":10,"y":20,"w":320,"h":180}]})");
    auto r = bc::parseVideoLayers(m, 42);
    REQUIRE(r); REQUIRE(r->size() == 1);
    CHECK((*r)[0].handle == 1234); CHECK((*r)[0].w == 320);
  }
  TEST_CASE("parseVideoLayers: another engine's pid is ignored") {
    CHECK_FALSE(bc::parseVideoLayers(json::parse(R"({"type":"videoLayers","pid":7,"layers":[]})"), 42));
  }
  TEST_CASE("parseVideoLayers: bad layers are dropped, the rest kept") {
    // layers: valid; handle "abc"; w 0; h 99999; missing id; x 1e9 → only the valid one survives
  }
  TEST_CASE("parseVideoLayers: at most 64 layers") {
    // 1000 valid layers → size 64
  }
  TEST_CASE("parseVideoLayers: malformed roots") {
    // not an object; no pid; layers not a list → nullopt, no throw
  }
  ```
- [ ] **Step 2: Run** `bc_tests.exe -tc="parseVideoLayers*"`. Expected: build fails.
- [ ] **Step 3: Implement `parseVideoLayers`**, then `CompositionHost` per spec §5.1: creation, input forwarding (mouse, wheel with screen→client conversion, leave, capture), cursor, `WM_NCHITTEST` via `GetNonClientRegionAtPoint`, focus, `WM_GETOBJECT`, visibility, layers (duplicate → `CreateSurfaceFromHandle` → visual inserted below the WebView visual → close the duplicate), `SetOffsetX/Y` + `SetClip` per layer, unlisted visuals removed, one `Commit()`, and DPI via `put_RasterizationScale`. Transparent default background via `ICoreWebView2Controller2`.
- [ ] **Step 4: Change `shell_main.cpp`:**
  - First line of `wWinMain`: `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`. Initial size `MulDiv(1600, GetDpiForSystem(), 96)` × `MulDiv(940, …)`. `WM_DPICHANGED` → `SetWindowPos` to the suggested rect + `host->setDpi(HIWORD(wp))`.
  - `wc.hbrBackground = CreateSolidBrush(<--bm-bg as RGB>)`. Read the value from the `@bmsolutionsvn/design` tokens that `ui/` imports, and name the token in a comment.
  - `startWebView`: try `CompositionHost::create`. On success, keep the returned `ICoreWebView2Controller` in `g.controller` (the existing settings, virtual host and handler code runs unchanged) and append `&nativeVideo=1` to the URL. On failure, or when the environment variable `BC_NO_COMPOSITION=1` is set (the operator's kill switch and the A/B check in Task 6), run today's `CreateCoreWebView2Controller` path untouched.
  - `wndProc`: call `g.host->handleMessage(...)` first when a host exists. `WM_SIZE` with `SIZE_MINIMIZED` → `setVisible(false)`; any other `WM_SIZE` → `setVisible(true)`.
  - `WebMessageReceived`: parse the string as JSON first (catch errors). `type == "videoLayers"` → `parseVideoLayers(j, g.launcher->pid())` → `g.host->setLayers(*layers, g.launcher->processHandle())`. Every other message keeps today's handling. `onLayerFailed` posts `videoLayerFailed` with `PostWebMessageAsJson`.
- [ ] **Step 5: Run** the parser tests (5 pass), then the full verification command. Launch `build/release/BreadCamera.exe`: the window opens and the UI works (previews stay H.264 until Task 5). Record in the ledger whether composition hosting started.
- [ ] **Step 6: Commit** — `feat(shell): composition-hosted WebView2 with engine video layers underneath`.

### Task 5: `NativeVideo` in the UI

**Files:**
- Create: `ui/src/preview/NativeVideo.tsx`, `ui/src/preview/nativeVideoRegistry.ts`, `ui/src/preview/NativeVideo.test.tsx`
- Modify: `ui/src/api/client.ts` (`subscribeSurfaces`, resend on reconnect), `ui/src/api/protocol.ts` (`engine.nativeVideo?: true | string`), `ui/src/multiview/Multiview.tsx`, `ui/src/calibration/CalibrationView.tsx`, `ui/src/main.tsx` or `App.tsx` (set the `native-video` class), `ui/src/app.css`

**Interfaces:**
- Consumes: wire reply from Task 3; shell messages from Task 4; `postToShell` / `onShellMessage` from `ui/src/bridge.ts`.
- Produces:
  ```ts
  // nativeVideoRegistry.ts
  export function nativeVideoRequested(): boolean;  // URL hash has nativeVideo=1
  export function startNativeVideo(client: EngineClient): () => void;  // App mounts it once
  export function register(id: string, el: HTMLElement): () => void;
  export function onLayerFailed(fn: (id: string) => void): () => void;
  // NativeVideo.tsx
  export function NativeVideo(props: { id: string; native: boolean; fallback: ReactNode }): JSX.Element;
  // client.ts
  subscribeSurfaces(surfaces: { id: string; w: number; h: number }[]): Promise<{ pid: number; surfaces: { id: string; handle: string; w: number; h: number }[] }>;
  ```
  `native` = `nativeVideoRequested() && state.engine?.nativeVideo === true`.

- [ ] **Step 1: Write the failing tests** (`NativeVideo.test.tsx`, the existing `fakeWs` helper, a stub `window.chrome.webview` with `postMessage` spy, `devicePixelRatio = 1.5`, and a stubbed `getBoundingClientRect`):
  - `renders the fallback when native is false` — the fallback is in the DOM and nothing is posted.
  - `subscribes with device-pixel sizes` — a box measured 200×100 CSS px → one `subscribeSurfaces` with `{id:'cam:A', w:300, h:150}`.
  - `relays the reply to the shell` — the reply `{pid:42, surfaces:[{id:'cam:A', handle:'99', w:300, h:150}]}` → `postMessage` called with a string whose JSON is `{type:'videoLayers', pid:42, layers:[{id:'cam:A', handle:'99', x, y, w:300, h:150}]}`.
  - `a position-only change re-posts layers without a new subscription`.
  - `a 0x0 box is left out` and `visibility hidden sends an empty subscription`.
  - `videoLayerFailed shows the no-picture placeholder for that id`.
- [ ] **Step 2: Run** `cd ui && npm test -- NativeVideo`. Expected: FAIL (module missing).
- [ ] **Step 3: Implement** per spec §6.1. One `requestAnimationFrame` flush per dirty frame. Send `subscribeSurfaces` only when the id set or a size changed. Resend on reconnect (the client already replays `previewSub`, so do the same for the last surface list). In dev builds (`import.meta.env.DEV`), warn once per box about any ancestor with an opaque computed `background-color` or a `background-image`.
- [ ] **Step 4: Use it.** In `Multiview`, wrap each `PreviewCanvas` as `<NativeVideo id={'cam:' + id} native={native} fallback={<PreviewCanvas …/>} />`. Skip `setPreviewSubscription` when native. Do the same in `CalibrationView`. Mount `startNativeVideo(client)` once in `App`, and set `document.documentElement.classList.toggle('native-video', native)`.
- [ ] **Step 5: Transparency pass** (spec §6.2). Under `html.native-video`, the html/body/app root and every ancestor of a video box are transparent. Move any colour a container needs onto a non-overlapping sibling, border or pseudo-element. In fallback mode the look is unchanged. Run the app in native mode and fix every dev warning.
- [ ] **Step 6: Run** `cd ui && npm test && npm run build` (all pass), then the full verification command.
- [ ] **Step 7: Commit** — `feat(ui): NativeVideo boxes show engine-presented camera previews`.

### Task 6: BreadCamera acceptance (owner judges)

**Files:**
- Modify: `docs/superpowers/specs/2026-11-02-native-video-previews-design.md` (status line), `docs/superpowers/specs/2026-10-08-breadcamera-design.md` §6.3 (one line pointing to the new spec for native previews)

- [ ] **Step 1: Run** `./build/release/bc_native_video_probe.exe` again (PASS or UNJUDGED) and the full verification command plus the `ui/` checks.
- [ ] **Step 2: Run the app** with a show of 4–8 test sources (the soak tool's SRT streams or files). Record in the ledger: whether `/engine/nativeVideo` is true, how many tiles show a picture, and the CPU and GPU % from `Get-Counter` for the engine process over 30 s. Do the same with `BC_NO_COMPOSITION=1` set (the H.264 fallback).
- [ ] **Step 3: Kill and relaunch the shell** 10 times with `taskkill /im BreadCamera.exe /f`. Each time, check that the engine survives and that the engine's `HandleCount` (`Get-Process breadcamera-engine`) does not grow by more than 20 across the 10 runs. Record the numbers.
- [ ] **Step 4: Update the spec's status line** ("implemented; owner acceptance pending", plus the date) and add the §6.3 pointer. Commit — `docs: native video previews implemented, acceptance pending`.
- [ ] **Step 5: Owner checklist.** Copy spec §9 "Owner acceptance" items 1–8 into the final report, unchecked. The owner runs them. Do not mark them passed.
