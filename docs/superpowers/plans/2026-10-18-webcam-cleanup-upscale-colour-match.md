# Webcam Clean up, Upscale and Colour Match Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. In BreadCamera this plan is handed to OpenCode: follow the per-task procedure, verification command, stop conditions and human gates in `docs/superpowers/plans/2026-10-11-remaining-milestones-opencode.md`.

**Goal:** Add the Clean up and Upscale add-in effects and a Match mode for the Colour effect, so remote webcam guests look sharp and match the other cameras.

**Architecture:** Two new stages run in `Camera::beginTick` before Colour: Clean up (RTX Video SDK at 1×, RTX only) and Upscale (RTX Video SDK super resolution, else AMD FSR 1 shaders, else a plain fit — always at the target size). Every GPU stage is timed with D3D11 timestamp queries and its guard sees GPU time. Match reads back three tiny 128×72 images per camera every 6 ticks, solves an exposure/temperature/tint correction on the CPU against a reference camera (or auto balance), eases it, and feeds it into the existing `ColorGrader`.

**Tech Stack:** MSVC C++20, D3D11 (fxc `cs_5_0`), NVIDIA RTX Video SDK 1.1 (NGX, optional), AMD FidelityFX FSR 1 (MIT HLSL), doctest, CMake/vcpkg; React + TypeScript + Vitest in `ui/`.

**Spec:** `docs/superpowers/specs/2026-10-18-webcam-cleanup-upscale-colour-match-design.md` (amends `2026-10-08-breadcamera-design.md`).

**Branch:** `bcam-webcam-fx`, cut from `bcam-m5` (it holds the per-camera size work this plan builds on). One commit per task.

## Global Constraints

- MSVC C++20 `/W4 /permissive- /utf-8`; the tick never blocks; the main path never copies a full frame to the CPU (spec §3.1). Match's readbacks are 128×72 and asynchronous.
- Every SDK behind `BC_WITH_<NAME>`; a missing SDK never fails the build. RTX code compiles when the SDK is present and is listed "untested on hardware" in `docs/superpowers/follow-ups.md` (§7.2).
- **No RTX Video SDK file is committed** — no `nvsdk_ngx*.h`, `.lib`, `.dll` or anything from its `samples/` (spec "RTX Video SDK licence obligations"). The SDK lives in untracked `third_party/rtx_video_sdk/`.
- A show file written before this change loads and behaves exactly as before (new Colour fields default to Calibrate).
- Never black: a failing or overrunning stage is bypassed for that camera only; Upscale's bypass keeps the target size (spec D3a).
- Upscale targets are exactly `720` and `1080` (height); width follows the input aspect, rounded to an even number.
- Match clamps: exposure ±1 EV, temperature ±60, tint ±60 (Colour field units). Easing time constant 3 s. Match never writes stage params (D16).
- UI copy: English, UK spelling; Clean up unavailable text is exactly `Needs an NVIDIA RTX card.` for a non-NVIDIA GPU.
- Conventional commit prefixes; never `git add -A`; never `--no-verify`.

## Review Focus

1. **A webcam that renegotiates its size mid-show while Upscale is on** (640×360 → 960×540): the frame stays at the target size and no output re-opens. Pinned in Task 3.
2. **Odd input shapes** — 4:3 (640×480 → 1440×1080), odd widths (854×480 → 1922×1080), and inputs already at or above the target (1280×720 with 720p → unchanged). Pinned in Task 3.
3. **Match to a camera that is removed from the show, or that loses its input**: the matching camera holds its last correction and the UI says so. Pinned in Tasks 7 and 8.
4. **No Matte and skin-coloured walls** (a wood-panelled room): the correction stays inside the clamps and the picture never goes green or purple. Pinned in Task 6.
5. **A show from an AMD PC that already has Clean up on a camera**: loads, Clean up shows bypassed with a readable error, nothing crashes. Pinned in Task 4.

---

### Task 1: GPU timing for stages

**Files:**
- Create: `src/gpu/gpu_timer.h`, `src/gpu/gpu_timer.cpp`, `tests/gpu_timer_test.cpp`
- Modify: `src/engine/gpu_tools.h` (add `GpuDevice* gpu = nullptr;` last), `src/engine/camera.h`, `src/engine/camera.cpp`, `src/engine/engine_main.cpp` (pass `gpu.get()` into `GpuTools`), `CMakeLists.txt`
- Test: `tests/engine_color_test.cpp`

**Interfaces:**
- Produces:
  - `class GpuTimer { explicit GpuTimer(GpuDevice&); void begin(); void end(); std::optional<std::chrono::nanoseconds> poll(); }` — a ring of 4 query sets (`D3D11_QUERY_TIMESTAMP_DISJOINT` + two `D3D11_QUERY_TIMESTAMP`). `begin`/`end` take `gpu.lock()`. When all 4 sets are pending, `begin` and its `end` record nothing. `poll` uses `GetData(..., D3D11_ASYNC_GETDATA_DONOTFLUSH)`, never waits, returns the newest finished non-disjoint measurement once, and discards older finished ones.
  - `GpuTools::gpu` (nullable). When null, stages keep today's CPU timing.
  - `double Camera::stageGpuMs(const std::string& kind) const` — last measurement in ms, 0 when none.
  - State: `/cameras/<id>/stages/<kind>/gpuMs` (number, rounded to 0.1) written in `publishStatus` for every stage that has a timer.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/gpu_timer_test.cpp
TEST_CASE("GpuTimer: measures a dispatch and reports it once") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::GpuTimer t{*gpu};
  t.begin();
  auto f = conv.solid(1920, 1080, {0.2f, 0.2f, 0.2f, 1});
  conv.toBgra8(*f, 1920, 1080);
  t.end();
  std::optional<std::chrono::nanoseconds> got;
  for (int i = 0; i < 200 && !got; ++i) { gpu->context()->Flush(); got = t.poll(); Sleep(5); }
  REQUIRE(got);
  CHECK(got->count() > 0);
  CHECK_FALSE(t.poll());  // reported once
}
TEST_CASE("GpuTimer: five begins without a poll never block and the fifth records nothing") { /* begin/end ×5, then poll up to 1 s: at most 4 results come back, the newest first */ }
```
```cpp
// tests/engine_color_test.cpp — guard sees GPU time
TEST_CASE("Camera: the colour stage publishes its GPU time") {
  // Existing colour rig with GpuTools{.conv = conv, .comp = comp, .grader = grader, .gpu = gpu.get()}
  // (designated initialisers, so Tasks 3–4 can insert `up` and `rtx` before `gpu`); colour on with exposure 1.
  // Tick for 1 s of ticks, publishStatus(): state /cameras/<id>/stages/color/gpuMs is a number >= 0,
  // and stageGpuMs("color") > 0.
}
```

- [ ] **Step 2: Run them and watch them fail**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "GpuTimer*,Camera: the colour stage publishes*"`
Expected: FAIL to compile (`GpuTimer`, `stageGpuMs` missing).

- [ ] **Step 3: Implement `GpuTimer` and wire Colour and Background through it**

In `Camera`, one `GpuTimer` per GPU stage (`color`, `background`; Tasks 3–4 add `cleanup`, `upscale`), created lazily when `tools_.gpu` is set. Wrap the grader/compositor call in `begin()`/`end()`; each tick `poll()` every timer and feed a result into that stage's guard: `guard.succeeded(took, budget, now)` with today's budgets (colour 4 ms, background `period/4`). With `tools_.gpu == nullptr` keep today's `Clock::now() - t0` path. **Matte keeps its batch timing**: Maxine runs on CUDA and ONNX off the tick, neither of which D3D11 timestamps can see (ruling on spec D14).

- [ ] **Step 4: Run the tests and watch them pass**

Run: the verification command from the OpenCode handoff plan.
Expected: PASS, including every existing `Camera:` and `Engine*` case.

- [ ] **Step 5: Commit** — `feat(engine): time GPU stages with D3D11 timestamp queries`

---

### Task 2: Clean up and Upscale schemas, and stage availability

**Files:**
- Modify: `src/core/stage_params.h`, `src/core/stage_params.cpp`, `src/engine/command_router.h`, `src/engine/command_router.cpp`, `src/engine/engine_main.cpp` (`Fx::stageUnavailable` returns `""` for now; Task 4 fills it)
- Test: `tests/stage_params_test.cpp`, `tests/command_router_test.cpp`

**Interfaces:**
- Produces:
  - `stageSchemas()` in pipeline order: `cleanup`, `upscale`, `color`, `matte`, `background`.
  - `cleanup`: label `Clean up`, icon `cleanup`, description `Removes compression blockiness and webcam noise. Needs an NVIDIA RTX card.`, `onAdd {"enabled": true}`, fields: `enabled` (bool, default false).
  - `upscale`: label `Upscale`, icon `upscale`, description `Raises a low-resolution camera to 720p or 1080p. AI on NVIDIA RTX cards; a sharp standard upscaler on other GPUs.`, `onAdd {"enabled": true, "target": "1080"}`, fields: `enabled` (bool, default false), `target` (enum `["720","1080"]`, labels `["720p","1080p"]`, default `"1080"`, label `Target`).
  - New field type `"text"`: a string of at most 64 characters (`normalizeStageParams` rejects anything else with `'<key>' must be text of at most 64 characters`).
  - `nlohmann::json schemaToJson(const StageSchema& s, const std::string& unavailable = "")` — adds `"unavailable": <reason>` only when non-empty.
  - `CommandRouter::Effects::stageUnavailable(const std::string& kind)` — virtual, **not** pure, default returns `""` (existing test fakes keep compiling).
  - `getStageSchemas` fills `unavailable` from it. `setStage` for a stage **not yet on the camera** whose `stageUnavailable` is non-empty throws `CmdError("unavailable", reason)`. Changing a stage already on the camera is always allowed (a show moved from an RTX PC stays editable).

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/stage_params_test.cpp
TEST_CASE("stage schemas: clean up and upscale come first, in pipeline order") {
  std::vector<std::string> kinds;
  for (auto& s : bc::stageSchemas()) kinds.push_back(s.kind);
  CHECK(kinds == std::vector<std::string>{"cleanup", "upscale", "color", "matte", "background"});
  auto up = bc::normalizeStageParams("upscale", {{"enabled", true}});
  CHECK(up["target"] == "1080");
  CHECK_THROWS(bc::normalizeStageParams("upscale", {{"target", "2160"}}));
}
TEST_CASE("schemaToJson: unavailable appears only when set") {
  auto* s = bc::findStageSchema("cleanup");
  CHECK_FALSE(bc::schemaToJson(*s).contains("unavailable"));
  CHECK(bc::schemaToJson(*s, "Needs an NVIDIA RTX card.")["unavailable"] == "Needs an NVIDIA RTX card.");
}
```
```cpp
// tests/command_router_test.cpp — a fake Effects whose stageUnavailable("cleanup") returns "Needs an NVIDIA RTX card."
TEST_CASE("CommandRouter: an unavailable effect cannot be added, but one already on a camera can be changed") {
  // getStageSchemas -> the cleanup entry has unavailable == "Needs an NVIDIA RTX card."
  // setStage cleanup on a camera without it -> error code "unavailable", message "Needs an NVIDIA RTX card."
  // a show loaded with stages.cleanup already present -> setStage cleanup {enabled:false} succeeds
}
```

- [ ] **Step 2: Run them and watch them fail**

Run: `scripts/test.ps1 -Filter "stage schemas*,schemaToJson*,CommandRouter: an unavailable effect*"`
Expected: FAIL (kinds list is `color, matte, background`; no `unavailable`).

- [ ] **Step 3: Implement** the schemas, the `text` type, `schemaToJson`'s parameter, `Effects::stageUnavailable` and the `setStage` check. Update any existing test that asserts the old schema count or order.

- [ ] **Step 4: Run the tests and watch them pass** — verification command. Expected: PASS.

- [ ] **Step 5: Commit** — `feat(core): Clean up and Upscale schemas, and stage availability in the API`

---

### Task 3: Upscale on any GPU (FSR 1), with a stable size

**Files:**
- Create: `src/core/upscale_size.h`, `src/gpu/upscaler.h`, `src/gpu/upscaler.cpp`, `src/gpu/shaders/fsr_easu.hlsl`, `src/gpu/shaders/fsr_rcas.hlsl`, `src/gpu/shaders/ffx/ffx_a.h`, `src/gpu/shaders/ffx/ffx_fsr1.h` (vendored unchanged from AMD FidelityFX FSR 1.0.2, MIT, with their licence header), `tests/upscaler_test.cpp`, `tests/engine_upscale_test.cpp`
- Modify: `src/engine/gpu_tools.h` (add `Upscaler* up = nullptr;` after `grader`), `src/engine/camera.h`, `src/engine/camera.cpp`, `src/engine/engine_main.cpp`, `CMakeLists.txt` (two `bc_shader` lines; the `ffx/*.h` files in the custom command's `DEPENDS`), `installer/THIRD_PARTY_NOTICES.md` (FSR 1, MIT)

**Interfaces:**
- Consumes: Task 1's `GpuTimer`; Task 2's `upscale` schema.
- Produces:
  - `struct FrameSize { uint32_t w, h; };` and `inline FrameSize upscaleTarget(uint32_t inW, uint32_t inH, uint32_t targetH)` in `core/upscale_size.h`: returns `{inW, inH}` when `targetH <= inH`; else `{w, targetH}` where `w = 2 * lround(inW * targetH / (2.0 * inH))`.
  - `class Upscaler { Upscaler(GpuDevice&, TexturePool&); TextureRef apply(const GpuTexture& linear, uint32_t w, uint32_t h); }` — linear RGBA16F in and out. EASU then RCAS (sharpness 0.2 stops, FSR's default). EASU and RCAS run on gamma-encoded values: encode with the pipeline's 1/2.4 on load and decode with 2.4 on store.
  - `Camera`: `upscaleParams_`, `upscaleGuard_`, `StageGuard& upscaleGuard()`, and `TextureRef enhance(const TextureRef& in, bool fresh, Clock::time_point now)` called in `beginTick` on the converted frame before Colour. Its result is cached and reused while `!fresh` and the size matches (as the grade is).
  - Fallback chain for Upscale, always at the target size (D3a): RTX (Task 4) → `tools_.up->apply` → `tools_.conv.fit`. On a non-RTX GPU, `upscaleGuard_` guards the FSR call; while it is bypassed, `conv.fit` keeps the size.
  - `lastSize_` becomes the **enhanced** frame's size. While Upscale is on and no frame has been seen, the fallback size is `{targetH * 16 / 9, targetH}`.
  - State: `/cameras/<id>/stages/upscale/state` and `/error` (like the other stages), and `/stages/upscale/note` = `Input is already 1080p` (or `720p`) when `upscaleTarget` returns the input size, else `null`.
  - Budget: `upscale` gets `sched.rate().period() / 4`, measured by its `GpuTimer` (`// ponytail:` comment naming the fixed share, as Background has).

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/upscaler_test.cpp
TEST_CASE("upscaleTarget: keeps aspect, rounds to even, never shrinks") {
  CHECK(bc::upscaleTarget(640, 360, 1080).w == 1920);
  CHECK(bc::upscaleTarget(640, 480, 1080).w == 1440);
  CHECK(bc::upscaleTarget(854, 480, 1080).w == 1922);
  auto same = bc::upscaleTarget(1280, 720, 720);
  CHECK((same.w == 1280 && same.h == 720));
  auto big = bc::upscaleTarget(1920, 1080, 720);
  CHECK((big.w == 1920 && big.h == 1080));
}
TEST_CASE("Upscaler: output is the asked size and a flat colour stays flat") {
  // Rig as in color_grader_test.cpp plus bc::Upscaler up{*gpu, pool};
  // apply(solid 640x360 {0.2,0.4,0.1}) -> 1920x1080; readback centre pixel within ±2 of the input's.
}
TEST_CASE("Upscaler: a hard edge stays sharp and does not ring") {
  // 64x36: left half black, right half white. Upscale to 192x108. Along row 54 the values are
  // non-decreasing (±2) and go from <10 to >245 within 6 px.
}
```
```cpp
// tests/engine_upscale_test.cpp — FakeInput/FakeOutput rig as the `Camera:` cases in tests/engine_test.cpp
TEST_CASE("Camera: Upscale raises a 640x360 input to 1920x1080") {}
TEST_CASE("Camera: Upscale keeps the target size when its stage is bypassed") {
  // upscaleGuard().failed("forced", now) then tick: lastFrame() is still 1920x1080.
}
TEST_CASE("Camera: Upscale keeps the target size when the input renegotiates mid-show") {
  // input 640x360 -> 960x540 mid-run: lastFrame() stays 1920x1080; the FakeOutput saw no size change.
}
TEST_CASE("Camera: the slate is at the target size while Upscale is on") {
  // input stops past the hold time: lastFrame() is 1920x1080; before any frame: 1920x1080.
}
TEST_CASE("Camera: Upscale to 720p on a 1280x720 input changes nothing and says so") {
  // lastFrame() 1280x720; publishStatus -> /stages/upscale/note == "Input is already 720p".
}
```

- [ ] **Step 2: Run them and watch them fail**

Run: `scripts/test.ps1 -Filter "upscaleTarget*,Upscaler*,Camera: Upscale*,Camera: the slate is at the target*"`
Expected: FAIL to compile.

- [ ] **Step 3: Implement** `upscale_size.h`, the two shaders and `Upscaler`, then `Camera::enhance` with the fallback chain and the `lastSize_` change. Construct `bc::Upscaler` in `engine_main.cpp` and pass `&up` in `GpuTools`.

- [ ] **Step 4: Run the tests and watch them pass** — verification command. Expected: PASS on the RX 6600 and on WARP.

- [ ] **Step 5: Commit** — `feat(engine): Upscale effect with FSR 1 and a stable output size`

---

### Task 4: RTX Video SDK — Clean up, and AI Upscale on RTX

**Files:**
- Create: `cmake/FindRtxVideo.cmake` (pattern: `cmake/FindMaxine.cmake`), `src/gpu/rtx_video.h`, `src/gpu/rtx_video.cpp`, `tests/rtx_video_test.cpp`
- Modify: `CMakeLists.txt` (`BC_WITH_RTXVIDEO` like `BC_WITH_MAXINE`; `BC_WITH_RTXVIDEO=1/0` on `bc_gpu`; link `lib/Windows/x64/nvsdk_ngx_d.lib`; copy `bin/Windows/x64/rel/nvngx_vsr.dll` next to the engine and the tests), `src/engine/gpu_tools.h` (add `RtxVideo* rtx = nullptr;` after `up`), `src/engine/camera.h`, `src/engine/camera.cpp`, `src/engine/engine_main.cpp`, `tests/support/requires.h`, `tests/support/requires.cpp` (`Req::RtxVideo`), `installer/THIRD_PARTY_NOTICES.md`, `docs/superpowers/follow-ups.md`

**Interfaces:**
- Consumes: Task 3's `Camera::enhance`, `Upscaler`, fallback chain; Task 2's `Effects::stageUnavailable`.
- Produces:
  - `RTXVIDEO_ROOT` defaults to `${CMAKE_SOURCE_DIR}/third_party/rtx_video_sdk` (the SDK zip's top folder: `include/`, `lib/`, `bin/`). Found when `include/nvsdk_ngx_helpers_vsr.h`, the lib and the DLL exist. The installer's existing `{#BuildDir}\*.dll` line ships the DLL.
  - `class RtxVideo { static std::unique_ptr<RtxVideo> create(GpuDevice&, TexturePool&, Converter&, std::string& whyNot); ~RtxVideo(); TextureRef run(const GpuTexture& linear, uint32_t w, uint32_t h); }`.
    - `create` returns null and sets `whyNot` to exactly: `Needs an NVIDIA RTX card.` (`!gpu.isNvidia()` or a WARP device), `This build has no NVIDIA RTX Video support.` (`BC_WITH_RTXVIDEO=0`), or `Needs an NVIDIA RTX card with driver R550 or later.` (NGX init fails or `VSR.Available` is 0).
    - NGX: `NVSDK_NGX_D3D11_Init` with the application id the SDK samples pass and the data path `%LOCALAPPDATA%\BreadCamera\ngx`; `NVSDK_NGX_D3D11_GetCapabilityParameters`; create the VSR feature once; the destructor releases the feature, then shuts NGX down.
    - `run`: `conv.toBgra8(linear)` → `NGX_D3D11_EVALUATE_VSR_EXT` with `NVSDK_NGX_VSR_Quality_High` into a pooled `DXGI_FORMAT_B8G8R8A8_UNORM` texture with UAV binding at `w × h` → `conv.toLinear(out, PixelLayout::BGRA8, true)`. `w × h` equal to the input runs VSR at 1× (Clean up). Throws `std::runtime_error` with the NGX result code on failure. Hold `gpu.lock()` across the three calls.
    - Written from the headers and the programming guide; nothing copied from the SDK's `samples/`.
  - `Camera`: `cleanupParams_`, `cleanupGuard_`, `StageGuard& cleanupGuard()`. Order inside `enhance`: Clean up (`rtx->run(in, in.w, in.h)`), then Upscale (`rtx->run(cleaned, target)`, falling back to Task 3's chain). Not fused (ruling: two VSR passes until an RTX run shows the cost matters; a `ponytail:` comment says so). With `tools_.rtx == nullptr` and Clean up on: `cleanupGuard_.failed(ErrorInfo{"needs-rtx", "Clean up needs an NVIDIA RTX card; it is bypassed on this PC."}, now)` whenever the guard would run.
  - `Fx::stageUnavailable("cleanup")` returns `whyNot` when `rtx` is null, else `""`; every other kind `""`.
  - `THIRD_PARTY_NOTICES.md` gains "NVIDIA RTX Video SDK runtime": `nvngx_vsr.dll` is NVIDIA's, under the NVIDIA RTX SDKs License; no reverse engineering; not to be redistributed apart from BreadCamera.
  - `follow-ups.md`: "Untested on hardware: RTX Video Clean up and Upscale. First RTX run checks: (1) VSR at 1× reduces blockiness, else Clean up moves to Maxine Artifact Reduction; (2) low-light noise left behind, else Maxine Webcam Denoising (spec D4); (3) GPU ms per camera for Clean up + Upscale on a 3060."

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/rtx_video_test.cpp
TEST_CASE("RtxVideo: not created on a non-NVIDIA device, with the plain reason") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Warp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  std::string why;
  CHECK(bc::RtxVideo::create(*gpu, pool, conv, why) == nullptr);
  CHECK(why == "Needs an NVIDIA RTX card.");
}
TEST_CASE("RtxVideo: upscales 640x360 to 1920x1080 and cleans at 1x" BC_REQUIRES(bc::test::Req::RtxVideo)) {
  // run(solid 640x360) -> 1920x1080, centre within ±3 of the input; run(solid 640x360, 640, 360) -> 640x360.
}
```
```cpp
// tests/engine_upscale_test.cpp
TEST_CASE("Camera: Clean up without an RTX card is bypassed with a readable error") {
  // GpuTools with rtx == nullptr; stages.cleanup {enabled:true}; tick, publishStatus:
  // /stages/cleanup/state == "bypassed", /stages/cleanup/error/code == "needs-rtx"; frames keep flowing.
}
```

- [ ] **Step 2: Run them and watch them fail** — `scripts/test.ps1 -Filter "RtxVideo*,Camera: Clean up*"`. Expected: FAIL to compile.

- [ ] **Step 3: Implement** `FindRtxVideo.cmake`, the option, `RtxVideo`, the camera wiring, `Fx::stageUnavailable`, the notices and the follow-up.

- [ ] **Step 4: Run the tests and watch them pass** — verification command. Expected: PASS here; the `Req::RtxVideo` case skipped with its printed reason. Record the `BC_WITH_RTXVIDEO=` configure line.

- [ ] **Step 5: Commit** — `feat(gpu): RTX Video SDK Clean up and AI Upscale (untested on hardware)`

---

### Task 5: UI for Clean up and Upscale

**Files:**
- Modify: `ui/src/api/protocol.ts`, `ui/src/dialogs/AddEffectDialog.tsx`, `ui/src/inspector/Effects.tsx`, `ui/src/shell/StatusBar.tsx`, `ui/src/components/Icon.tsx` (icons `upscale`, `cleanup`), `ui/src/dialogs/SettingsDialog.tsx`, `ui/src/app.css`
- Test: `ui/src/Effects.test.tsx`, `ui/src/Workspace.test.tsx`

**Interfaces:**
- Consumes: `StageSchemaJson.unavailable?: string`; state `stages.<kind>.gpuMs?: number`, `stages.upscale.note?: string | null`.
- Produces:
  - Add effect: a row is selectable only if not added, its `requires` are added, **and** it has no `unavailable`. Unselectable rows get class `off` (replacing `done`). An unavailable row shows its reason in the `.req` line.
  - CSS: `.pick-row.off :is(.ic, b, p) { opacity: .5 }` replaces `.pick-row.done { opacity: .5 }`, so the `.req` reason keeps full contrast (D8).
  - Effects group header: `3.2 ms` (one decimal, mono) before the state label when `gpuMs > 0` and the state is `active`. Upscale group body shows `note` as a `.note` line when set.
  - Status bar: when `overruns > 0`, append ` · heaviest: CAM <n> <label> <ms> ms` for the stage with the largest `gpuMs` across all cameras.
  - Settings → About: a line `Clean up and Upscale on NVIDIA RTX cards use the NVIDIA RTX Video SDK.` (plain text, no logo).

- [ ] **Step 1: Write the failing tests**

```tsx
// ui/src/Effects.test.tsx
it('shows an unavailable effect dimmed, not selectable, with its reason readable', () => {
  // schemas: cleanup with unavailable 'Needs an NVIDIA RTX card.'
  // Add effect: radio 'Clean up' is disabled; text 'Needs an NVIDIA RTX card.' is in the document;
  // its row has class 'off' and the reason element is not inside an element with opacity set.
});
it('shows the GPU time and the upscale note', () => {
  // state stages.upscale {state:'active', gpuMs: 3.24, note:'Input is already 1080p'}
  // -> '3.2 ms' and 'Input is already 1080p' are shown.
});
```
```tsx
// ui/src/Workspace.test.tsx
it('names the heaviest stage when ticks overrun', () => {
  // overruns 12; CAM 2 upscale gpuMs 6.2, CAM 1 color gpuMs 0.4 -> status bar contains 'heaviest: CAM 2 Upscale 6.2 ms'
});
```

- [ ] **Step 2: Run them and watch them fail** — `cd ui && npm test`. Expected: the three new cases FAIL.

- [ ] **Step 3: Implement** the dialog, CSS, Effects header and note, status bar, icons and About line.

- [ ] **Step 4: Run** `cd ui && npm test && npx tsc --noEmit && npm run build`. Expected: PASS, types clean.

- [ ] **Step 5: Commit** — `feat(ui): Clean up and Upscale in Add effect, GPU time per effect, heaviest stage`

---

### Task 6: Skin statistics and the match solver (CPU)

**Files:**
- Create: `src/core/skin_match.h`, `src/core/skin_match.cpp`, `tests/skin_match_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Rgb`, `toLinearLight` (`core/color_math.h`). `bc_core` must not depend on `bc_gpu`, so the functions take raw BGRA bytes; Task 7 passes `CpuImage::bgra.data()`.
- Produces:
  - `struct SkinStats { Rgb mean; int count = 0; double frameLuma = 0; };` — `mean`: linear RGB mean of skin pixels; `frameLuma`: mean linear BT.709 luminance of every pixel.
  - `std::optional<SkinStats> skinStats(const uint8_t* bgra, const uint8_t* alphaBgra, uint32_t w, uint32_t h)` — `alphaBgra` may be null. A pixel is skin when, on its gamma-encoded 8-bit values with BT.601 full-range YCbCr: `Y >= 40`, `77 <= Cb <= 127`, `133 <= Cr <= 173`, and (with alpha) the alpha image's red byte `>= 191` (alpha 0.5 after `toBgra8`'s gamma encode). Returns `nullopt` when fewer than 0.5% of pixels are skin. `// ponytail:` comment: a fixed YCbCr skin box, a face detector if it misfires on real footage.
  - `struct MatchCorrection { float exposure = 0, temperature = 0, tint = 0; };`
  - `MatchCorrection solveMatch(const SkinStats& cam, const SkinStats& ref)` and `MatchCorrection autoBalance(const SkinStats& cam)`, both clamped to ±1 EV / ±60 / ±60.
  - `MatchCorrection ease(const MatchCorrection& cur, const MatchCorrection& target, double dtSeconds, double tauSeconds = 3.0)` — `cur + (target - cur) * (1 - exp(-dt / tau))` per field.
  - `Rgb skinTarget()` — sRGB (224, 172, 145) through `toLinearLight` (a function: `toLinearLight` is not `constexpr`). `// ponytail:` one reference skin chroma for auto balance; tune on real webcams.

The white-balance solve, which the signatures do not determine. `ColorGrader` multiplies R by `1 + T/400`, G by `1 − tint/400` and B by `1 − T/400` (`src/gpu/color_grader.cpp:66-68`):

```
ρ = (ref.r/ref.g) / (cam.r/cam.g)    β = (ref.b/ref.g) / (cam.b/cam.g)
w = 2 / (ρ + β)      u = ρ · w
temperature = 400 · (u − 1)      tint = 400 · (1 − w)
exposure = log2( lum(ref.mean) / lum(cam.mean · {u, w, 2 − u}) )
```

`autoBalance` uses the same white-balance solve with `skinTarget()` as `ref`, and `exposure = log2(0.18 / frameLuma)` (0 when `frameLuma <= 0`). Clamp after solving.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/skin_match_test.cpp — images built as std::vector<uint8_t> BGRA in the test
TEST_CASE("skinStats: skin pixels are counted, a blue background is not") {
  // 128x72: left half sRGB(224,172,145), right half sRGB(40,60,200).
  // count == 64*72; mean ≈ toLinearLight of (224,172,145)/255 within 1%.
}
TEST_CASE("skinStats: too little skin gives nothing; alpha excludes skin outside the person") {
  // 30 skin pixels in 128x72 -> nullopt. Full skin image with an alpha image whose left half is 0 -> count == 64*72.
}
TEST_CASE("solveMatch: identical cameras need no correction") { /* all three fields |x| < 1e-3 */ }
TEST_CASE("solveMatch: a blue-cast camera is warmed until its skin chroma matches the reference") {
  // cam = ref.mean * {0.85, 1.0, 1.2}; c = solveMatch(cam, ref): c.temperature > 0;
  // applying {1+T/400, 1-tint/400, 1-T/400} to cam.mean gives ref's R/G and B/G within 2%.
}
TEST_CASE("solveMatch: twice as bright a reference is +1 EV, and extremes clamp") {
  // ref.mean = cam.mean * 2 -> exposure ≈ 1.0 (±0.02); ref.mean = cam.mean * 16 -> exposure == 1.0;
  // a 3x red cast -> |temperature| == 60.
}
TEST_CASE("autoBalance: a wood-panelled room stays inside the clamps") {
  // every pixel sRGB(181,120,70) -> |exposure| <= 1, |temperature| <= 60, |tint| <= 60.
}
TEST_CASE("ease: 63% of the way after one time constant, never overshoots, dt 0 does nothing") {}
```

- [ ] **Step 2: Run them and watch them fail** — `scripts/test.ps1 -Filter "skinStats*,solveMatch*,autoBalance*,ease*"`. Expected: FAIL to compile.

- [ ] **Step 3: Implement** `skin_match.cpp`.

- [ ] **Step 4: Run the tests and watch them pass** — verification command. Expected: PASS.

- [ ] **Step 5: Commit** — `feat(core): skin statistics and the colour match solver`

---

### Task 7: Match in the engine

**Files:**
- Modify: `src/core/stage_params.cpp` (Colour fields), `src/engine/camera.h`, `src/engine/camera.cpp`, `src/engine/engine.cpp`, `src/engine/command_router.cpp`
- Test: `tests/engine_color_test.cpp`, `tests/command_router_test.cpp`, `tests/stage_params_test.cpp`

**Interfaces:**
- Consumes: Task 6's `skinStats`, `solveMatch`, `autoBalance`, `ease`; `ReadbackSlot`; Task 3's `enhance` (Match measures the enhanced frame).
- Produces:
  - Colour schema gains hidden fields `mode` (enum `["calibrate","match"]`, default `"calibrate"`) and `matchTo` (`text`, default `""` = Auto balance).
  - `Camera`: every 6th tick, while the Colour stage is on in Match mode **or** another camera matches this one, begin three `ReadbackSlot` copies at 128×72 via `conv.toBgra8`: the frame entering Colour, the frame leaving Colour, and the previous tick's refined alpha (when there is one). Finish them on later ticks without waiting. `std::optional<SkinStats> matchOutputStats() const` returns stats of the frame leaving Colour (with alpha). `void setMatchReference(std::optional<SkinStats> ref, bool refHasPicture)`. `void setMatchedBy(bool)` marks a camera others match to, so it measures its output even when not in Match mode itself.
  - On each finished measurement: target = `solveMatch(own, ref)` when `matchTo` is set, the reference has a picture and both stats exist; `autoBalance(own)` when `matchTo` is empty and own stats exist; otherwise the target is unchanged (hold). `correction_ = ease(correction_, target, 6 / fps)`.
  - Grade in Match mode: `gradeFrom(params)` **without the LUT** (the stored LUT returns when the mode goes back to Calibrate), plus `exposure`, `temperature`, `tint` from `correction_`; `gradeDirty_` when any field moves by more than 1e-3. The correction lives only in the camera and resets on `reconfigure` of the Colour stage; stage params are never written (D16).
  - State `/cameras/<id>/stages/color/match`: `"matching"`, `"auto"`, `"holding-no-face"` (own stats missing) or `"holding-reference-down"` (reference missing or without a picture); `null` in Calibrate mode.
  - `Engine::run`, before half 1: for each camera in Match mode with `matchTo` set, `setMatchReference(ref ? ref->matchOutputStats() : nullopt, ref && ref->tickPhase() == FeedPhase::Live)`, and `setMatchedBy(true)` on the reference (false on every camera no one matches).
  - `CommandRouter` `setStage` on `color` with `mode == "match"` and a non-empty `matchTo` throws `CmdError("invalid", …)` with exactly: `A camera cannot match itself.` (own id); `No camera <id>.` (unknown); `<name> already matches this camera, directly or through another camera.` (following `matchTo` from the target leads back to this camera). Removing a camera that others match is allowed (they hold).

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/stage_params_test.cpp
TEST_CASE("colour: an old colour stage loads in Calibrate mode") {
  auto p = bc::normalizeStageParams("color", {{"enabled", true}});
  CHECK(p["mode"] == "calibrate");
  CHECK(p["matchTo"] == "");
}
```
```cpp
// tests/command_router_test.cpp
TEST_CASE("CommandRouter: Match refuses itself, unknown cameras and loops") {
  // cams a, b, c. a->a: "A camera cannot match itself."  a->zz: "No camera zz."
  // a->b ok; b->a: "<name of a> already matches this camera, directly or through another camera."
  // b->c ok; c->a: refused (a->b->c->a).
}
```
```cpp
// tests/engine_color_test.cpp — two FakeInputs, no Matte (whole-frame skin)
TEST_CASE("Engine: Match brings a blue-cast camera to the reference's skin colour, slowly") {
  // A solid sRGB(200,172,175) (blue cast), B solid sRGB(224,172,145). A colour {enabled, mode:match, matchTo:B}.
  // After 10 s of ticks: A's output centre R/G and B/G within 5% of B's.
  // Between any two consecutive outputs no channel moves by more than 2/255.
  // A's config().stages["color"] is unchanged throughout.
}
TEST_CASE("Engine: Match holds when the reference loses its picture or is removed") {
  // After convergence, pause B's input past the hold time: A's correction stays within 1e-3 over 2 s;
  // state color/match == "holding-reference-down". Remove B from the show: same, no exception.
}
TEST_CASE("Engine: Match with no reference auto-balances and reports it") {
  // matchTo "": state color/match == "auto"; a solid blue image with no skin -> "holding-no-face".
}
```

- [ ] **Step 2: Run them and watch them fail** — `scripts/test.ps1 -Filter "colour: an old colour stage*,CommandRouter: Match*,Engine: Match*"`. Expected: FAIL.

- [ ] **Step 3: Implement** the fields, camera measurement and grade, engine wiring and router validation.

- [ ] **Step 4: Run the tests and watch them pass** — verification command. Expected: PASS, including every existing calibration case.

- [ ] **Step 5: Commit** — `feat(engine): Match mode for the Colour effect`

---

### Task 8: UI for Match

**Files:**
- Modify: `ui/src/calibration/ColourPanel.tsx`, `ui/src/api/protocol.ts`
- Create: `ui/src/ColourMatch.test.tsx`

**Interfaces:**
- Consumes: Colour params `mode`, `matchTo`; state `stages.color.match`; the camera list and numbering the Inspector already uses (`CAM n`).
- Produces:
  - A segmented control **Calibrate | Match** at the top of the Colour panel (`role="radiogroup"`, `aria-label="Colour mode"`), sending `setStage color {mode}`.
  - In Match mode: a **Match to** `<select>` with `Auto balance` (value `""`) then every other camera as `CAM <n> · <name>`, sending `setStage color {matchTo}`; a router error shows in an `ErrorBlock`. A status line: `matching` → `Matching CAM <n>`, `auto` → `Auto balance`, `holding-no-face` → `Holding — no face found`, `holding-reference-down` → `Holding — CAM <n> has no picture`. The calibrate controls are hidden in Match mode.

- [ ] **Step 1: Write the failing tests**

```tsx
// ui/src/ColourMatch.test.tsx
it('switches to Match and picks a camera', async () => {
  // click 'Match' -> request setStage {stage:'color', params:{mode:'match'}};
  // select 'CAM 2 · Guest' -> setStage {params:{matchTo:'<id of cam 2>'}}; 'Calibrate' button not shown.
});
it('says when it is holding', () => {
  // state color.match 'holding-reference-down', matchTo cam 2 -> 'Holding — CAM 2 has no picture'.
});
it('shows the router refusal', async () => {
  // request rejects with 'Guest already matches this camera, directly or through another camera.' -> text shown.
});
```

- [ ] **Step 2: Run them and watch them fail** — `cd ui && npm test`. Expected: FAIL.

- [ ] **Step 3: Implement** the mode switch, picker and status line.

- [ ] **Step 4: Run** `cd ui && npm test && npx tsc --noEmit && npm run build`. Expected: PASS.

- [ ] **Step 5: Commit** — `feat(ui): Match mode in the Colour effect`

---

### Task 9: Acceptance checklist (owner runs it; not marked passed by the worker)

**Files:**
- Create: `docs/superpowers/manual-checks/webcam-fx.md`

- [ ] **Step 1: Write the checklist** with these checks, each with a result line left blank, headed "Approved 2026-10-18 — not tested yet":
  1. **Load (RTX 3060, 1080p60, 30 min):** 2 cameras with Clean up + Upscale (720p → 1080p) + Colour Match + Matte + Background, plus 2 cameras with Matte + Background; tick overruns 0; latency ≤ 50 ms (flash-frame); GPU ms per stage from the Inspector.
  2. **Match:** two webcams under different light, one matched to the other, look the same side by side; turning a lamp on is followed within a few seconds with no visible jump.
  3. **Fallback on RTX:** start BreadCamera with `nvngx_vsr.dll` moved out of the install folder: Upscale still delivers the target size (FSR 1), Clean up shows dimmed in Add effect. (Mid-show bypass keeping the size is pinned by Task 3's tests.)
  4. **AMD build PC:** Upscale runs on FSR 1; Clean up shows dimmed with `Needs an NVIDIA RTX card.`
  5. **First RTX run questions** from `follow-ups.md` (VSR at 1×, low-light noise, GPU ms).
- [ ] **Step 2: Run check 4 on this PC** and record the result. Leave 1–3 and 5 open as human gates (RTX).
- [ ] **Step 3: Commit** — `docs: webcam effects acceptance checklist`
