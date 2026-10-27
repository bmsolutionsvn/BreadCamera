# Enhance: merge Clean up into Upscale — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. In BreadCamera this plan is handed to OpenCode: follow the per-task procedure, verification command, stop conditions and human gates in `docs/superpowers/plans/2026-10-11-remaining-milestones-opencode.md`.

**Goal:** Replace the separate Clean up and Upscale effects with one effect, **Enhance**, that runs NVIDIA VSR once per frame: at a larger size when the camera is below the target, at the same size (clean-up only) when it is not, with a Fast / Balanced / High / Ultra quality.

**Architecture:** The `upscale` stage kind stays (saved shows and state paths keep working); it gains a `native` target and a `quality` field, and its label becomes "Enhance". `Camera::enhance` makes one `RtxVideo::run` call per fresh frame; the FSR 1 → fit fallback for resizing is unchanged. The `cleanup` stage, its guard, timer and state are deleted; a show that still has `cleanup` is migrated on load. The three VSR fixes that shipped in the 0.2.8 test build but never reached `main` are ported from branch `rescue-0.2.8`.

**Tech Stack:** MSVC C++20, D3D11 (fxc `cs_5_0`), NVIDIA RTX Video SDK 1.1 (NGX VSR, optional), doctest, CMake/vcpkg; React + TypeScript + Vitest in `ui/`.

**Spec:** `docs/superpowers/specs/2026-10-18-webcam-cleanup-upscale-colour-match-design.md` (D2–D7, D14), amended by the decisions below (Task 4 writes them into the spec).

**Branch:** `bcam-enhance`, cut from `main`. One commit per task. Read-only source for Task 1: `rescue-0.2.8` (commit `8f0696b`) — **never merge that branch**; it carries ~800 unrelated, unreviewed lines.

## Decisions (owner, 2026-10-27)

- **E1.** One effect. Kind `upscale` (unchanged), label `Enhance`, icon `upscale`. Description: `Sharpens and removes compression blockiness, and raises a low-resolution camera to 720p or 1080p. AI on NVIDIA RTX cards; a sharp standard upscaler on other GPUs.`
- **E2.** Fields, in order: `enabled` (bool, default false, "On"); `target` enum `native | 720 | 1080`, labels `Same size | 720p | 1080p`, default `1080`, label "Target"; `quality` enum `fast | balanced | high | ultra`, labels `Fast | Balanced | High | Ultra`, default `high`, label "Quality". `onAdd` stays `{enabled: true, target: "1080"}`.
- **E3.** Quality → `NVSDK_NGX_VSR_QualityLevel`: fast 1, balanced 2, high 3, ultra 4 (`high` is today's fixed level, so existing shows look the same).
- **E4.** One VSR pass per fresh frame. Output size `want` = input size for `native`, else `upscaleTarget(inW, inH, 720|1080)`. VSR runs when an RTX device exists and either `want` differs from the input (resize) or (`want` equals the input and input height ≤ **1080**). Above 1080 lines and no resize, nothing runs (a 4K clean-up pass would overrun the budget).
- **E5.** Fallbacks: a resize without RTX, or after VSR fails/bypasses, goes FSR 1 → plain fit, always at `want` (spec D3a, unchanged). A same-size frame without RTX, or after VSR fails/bypasses, passes through unchanged.
- **E6.** One guard (`upscaleGuard_`), one GPU timer (`upscaleTimer_`), one budget (`period / 4`) for the whole effect. State stays at `/cameras/<id>/stages/upscale/{state,error,gpuMs,note}`.
- **E7.** Note texts (exact): resize → none. Same size and VSR runs → `AI clean-up only`. `native` without RTX → `Same size needs an NVIDIA RTX card`. 720/1080 target, input already at/above it, no RTX → `Input is already 720p` / `Input is already 1080p` (unchanged). Input above 1080 lines, no resize, RTX present → `Input is above 1080p`. Without RTX the two no-RTX texts win (a 4K `native` camera on AMD says `Same size needs an NVIDIA RTX card`).
- **E8.** Migration on show load: a camera with `cleanup` and no `upscale` gets `upscale = {enabled: <cleanup.enabled>, target: "native"}`; a camera with both keeps `upscale` as it is. `cleanup` is then dropped. `schemaVersion` stays 1. `setStage`/`removeStage` with `cleanup` after this change fail as an unknown stage.
- **E9.** Enhance is never "unavailable" (FSR covers non-NVIDIA GPUs), so the RTX reason no longer reaches Add effect.

## Global Constraints

- MSVC C++20 `/W4 /permissive- /utf-8`; the tick never blocks; the main path never copies a full frame to the CPU.
- Every SDK behind `BC_WITH_<NAME>`; a missing SDK never fails the build. **No RTX Video SDK file is committed** (headers, libs, DLLs, samples).
- A show file written before this change loads (E8) and an Upscale-only camera behaves exactly as before on a non-RTX PC.
- Never black: a failing or overrunning Enhance is bypassed for that camera only, keeping `want` size.
- UI copy: English, UK spelling, exact strings from E1, E2, E7.
- Conventional commit prefixes; never `git add -A`; never `--no-verify`. RTX-only tests use `BC_REQUIRES(bc::test::Req::RtxVideo)` and are skipped on the RX 6600.

## Review Focus

1. **A saved show with Clean up on and Upscale off** (the typical 1080p webcam): loads as Enhance, target Same size, on. Pinned in Task 3 (`Show: a Clean up camera loads as Enhance at Same size`).
2. **A saved show with both Clean up and Upscale**: loads with Upscale's settings, Clean up gone, no error. Pinned in Task 3.
3. **A 4K camera with Enhance at Same size on an RTX PC**: nothing runs, note `Input is above 1080p`, picture unchanged. Pinned in Task 2 via `enhanceNote` and `enhancePlan` (pure, so it runs without an RTX card).
4. **Red and blue swapped through VSR** (the bug the 0.2.8 build fixed): pinned on every GPU in Task 1 (`Converter: toBgra8 without the swap keeps RGBA order`) and on RTX in `rtx_video_test`.
5. **A webcam that renegotiates from 1920×1080 to 640×360 mid-show with Enhance 1080p**: switches from clean-only to resize without re-opening outputs. Pinned in Task 2.

---

### Task 1: Port the VSR fixes from the 0.2.8 build

**Files:**
- Modify: `src/gpu/rtx_video.h`, `src/gpu/rtx_video.cpp`, `src/gpu/converter.h`, `src/gpu/converter.cpp`, `src/gpu/shaders/linear_to_bgra8.hlsl`
- Test: `tests/gpu_test.cpp`, `tests/rtx_video_test.cpp`

**Interfaces:**
- Produces:
  - `TextureRef Converter::toBgra8(const GpuTexture& linear, uint32_t outW, uint32_t outH, bool swapRb = true);` — `true` keeps today's BGRA byte order for every existing caller; `false` writes true RGBA into the `R8G8B8A8_UNORM` texture.
  - `TextureRef RtxVideo::run(const GpuTexture& linear, uint32_t w, uint32_t h, int quality = 3);` — `quality` clamped to 1..4 and passed as `QualityLevel`.

Port these three changes from `git diff 2f84465 8f0696b -- src/gpu/` (read it, do not merge the branch):
1. `toBgra8` gains `swapRb` (shader reads `gRow0.x`: `> 0.5` keeps RGB order, else `.bgr`). `RtxVideo::run` calls it with `swapRb = false`.
2. `RtxVideo::Impl` keeps one VSR input (`D3D11_BIND_SHADER_RESOURCE`) and output (`B8G8R8A8_UNORM`, SRV + UAV) texture, re-acquired only when size/format changes; the converted frame is `CopyResource`d into the kept input inside the existing `gpu_.lock()`.
3. The `quality` parameter as above.

- [ ] **Step 1: Write the failing tests**

In `tests/gpu_test.cpp`:
```cpp
TEST_CASE("Converter: toBgra8 without the swap keeps RGBA order") {
  // linear red; readback() returns the texture's bytes as stored
  auto red = g.conv.solid(4, 4, {1, 0, 0, 1});
  auto img = g.conv.readback(*g.conv.toBgra8(*red, 4, 4, /*swapRb=*/false));
  CHECK(img.bgra[0] == 255);  // byte 0 is R in RGBA order
  CHECK(img.bgra[2] == 0);
  auto def = g.conv.readback(*g.conv.toBgra8(*red, 4, 4));
  CHECK(def.bgra[2] == 255);  // default stays BGRA: byte 2 is R
}
```
(Build `g` the way the neighbouring `gpu_test` cases do.) In `tests/rtx_video_test.cpp`, extend the RTX case: a solid `{0.6f, 0.1f, 0.05f, 1}` (strongly red) through `run(*in, 640, 360, 4)` and `run(*in, 1920, 1080, 1)` keeps each centre channel within 3 of the input.

- [ ] **Step 2: Run them and watch them fail** — verification command. Expected: the new `gpu_test` case fails to compile (no `swapRb` parameter).
- [ ] **Step 3: Implement** the three ported changes.
- [ ] **Step 4: Run the tests and watch them pass** — verification command. Expected: PASS on the RX 6600 and on WARP; the RTX case is skipped there.
- [ ] **Step 5: Commit** — `fix(gpu): VSR reads true RGBA, keeps its textures, takes a quality level`

---

### Task 2: Enhance runs VSR once per frame

**Files:**
- Modify: `src/core/upscale_size.h`, `src/core/stage_params.cpp`, `src/engine/camera.h`, `src/engine/camera.cpp`
- Test: `tests/upscaler_test.cpp`, `tests/stage_params_test.cpp`, `tests/engine_upscale_test.cpp`

**Interfaces:**
- Consumes: `RtxVideo::run(..., int quality)` (Task 1).
- Produces (all in `src/core/upscale_size.h`, pure, no GPU):
  - `int vsrLevel(std::string_view quality);` — E3; unknown → 3.
  - `struct EnhancePlan { FrameSize want; bool resize; bool vsr; };`
  - `EnhancePlan enhancePlan(uint32_t inW, uint32_t inH, uint32_t targetH, bool rtx);` — `targetH == 0` means `native`. Implements E4: `vsr = rtx && (resize || inH <= 1080)`.
  - `std::optional<std::string> enhanceNote(uint32_t inW, uint32_t inH, uint32_t targetH, bool rtx);` — E7 exact texts.
  - Schema `upscale` per E1/E2 (the `cleanup` schema stays until Task 3).

- [ ] **Step 1: Write the failing tests**

`tests/upscaler_test.cpp`:
```cpp
TEST_CASE("enhancePlan: one VSR pass, clean-only at the same size, nothing above 1080") {
  auto p = bc::enhancePlan(640, 360, 1080, true);
  CHECK(p.resize); CHECK(p.vsr); CHECK(p.want.w == 1920); CHECK(p.want.h == 1080);
  p = bc::enhancePlan(1920, 1080, 1080, true);
  CHECK_FALSE(p.resize); CHECK(p.vsr);
  p = bc::enhancePlan(1280, 720, 0, true);   // native
  CHECK_FALSE(p.resize); CHECK(p.vsr); CHECK(p.want.w == 1280);
  p = bc::enhancePlan(3840, 2160, 0, true);
  CHECK_FALSE(p.vsr);
  p = bc::enhancePlan(640, 360, 1080, false);
  CHECK(p.resize); CHECK_FALSE(p.vsr);
  CHECK(bc::vsrLevel("fast") == 1); CHECK(bc::vsrLevel("balanced") == 2);
  CHECK(bc::vsrLevel("high") == 3); CHECK(bc::vsrLevel("ultra") == 4);
}
TEST_CASE("enhanceNote: exact texts") {
  CHECK_FALSE(bc::enhanceNote(640, 360, 1080, true));
  CHECK(*bc::enhanceNote(1920, 1080, 1080, true) == "AI clean-up only");
  CHECK(*bc::enhanceNote(1280, 720, 0, false) == "Same size needs an NVIDIA RTX card");
  CHECK(*bc::enhanceNote(1280, 720, 720, false) == "Input is already 720p");
  CHECK(*bc::enhanceNote(1920, 1080, 1080, false) == "Input is already 1080p");
  CHECK(*bc::enhanceNote(3840, 2160, 0, true) == "Input is above 1080p");
  CHECK(*bc::enhanceNote(3840, 2160, 0, false) == "Same size needs an NVIDIA RTX card");
}
```
`tests/stage_params_test.cpp`: `normalizeStageParams("upscale", {{"enabled", true}})` gives `target == "1080"` and `quality == "high"`; `{{"target","native"},{"quality","ultra"}}` is accepted; `{{"quality","best"}}` throws; `schemaToJson(*findStageSchema("upscale"))["label"] == "Enhance"`.

`tests/engine_upscale_test.cpp`:
- `Camera: Enhance at Same size without an RTX card passes the picture through and says why` — `target: native`, 640×360 frames, no `rtx` in `GpuTools`: `lastFrame()` is 640×360, state `/cameras/a/stages/upscale/note == "Same size needs an NVIDIA RTX card"`, `state == "active"`.
- `Camera: Enhance follows a webcam that drops from 1080p to 360p` — engine run, `target: 1080`, fake input 1920×1080 then 640×360: output stays 1920×1080 and `opens` is unchanged (mirror the existing renegotiation test).
- The existing Upscale tests keep passing unchanged (they now exercise Enhance).

- [ ] **Step 2: Run them and watch them fail** — verification command. Expected: compile errors for `enhancePlan`/`enhanceNote`/`vsrLevel`; schema asserts fail.
- [ ] **Step 3: Implement**
  - The three helpers and the E1/E2 schema.
  - `Camera::loadStageParams`: `upscaleTargetH_` = 0 for `native`, 720, or 1080; store `upscaleQuality_ = vsrLevel(...)`.
  - `Camera::enhance` per E4–E6: when `!enabled` return `in`; `plan = enhancePlan(in->width, in->height, upscaleTargetH_, tools_.rtx != nullptr)`; if `!plan.resize && !plan.vsr` reset `lastEnhanced_` and return `in`; keep the repeat-tick reuse; if `plan.vsr && upscaleGuard_.shouldRun(now)` make the **one** `tools_.rtx->run(*in, want.w, want.h, upscaleQuality_)` call, timed and guarded exactly like today's RTX upscale branch; if it did not run or threw: `resize` → today's FSR → fit chain, else `in`.
  - Delete the Clean up block in `enhance` and stop reading `cleanupParams_` there (the member is removed in Task 3).
  - `publishStatus`: note = `enhanceNote(lastConverted_->width, lastConverted_->height, upscaleTargetH_, tools_.rtx != nullptr)` when on and a frame exists, else null.
- [ ] **Step 4: Run the tests and watch them pass** — verification command. Expected: PASS on the RX 6600 and on WARP.
- [ ] **Step 5: Commit** — `feat(engine): Enhance runs one VSR pass, clean-only at the same size`

---

### Task 3: Remove Clean up and migrate old shows

**Files:**
- Modify: `src/core/stage_params.cpp`, `src/core/show.cpp`, `src/engine/camera.h`, `src/engine/camera.cpp`, `src/engine/engine_main.cpp`, `src/engine/gpu_tools.h` (comment), `src/gpu/rtx_video.h` (comments)
- Test: `tests/show_test.cpp`, `tests/stage_params_test.cpp`, `tests/command_router_test.cpp`, `tests/engine_upscale_test.cpp`

**Interfaces:**
- Consumes: the Enhance schema (Task 2).
- Produces: `stageSchemas()` is `{autoframe, upscale, color, matte, background}` (5). `cameraFromJson` applies E8 before normalising stages.

- [ ] **Step 1: Write the failing tests**

`tests/show_test.cpp`:
```cpp
TEST_CASE("Show: a Clean up camera loads as Enhance at Same size") {
  // cameraFromJson on a camera whose stages are {"cleanup": {"enabled": true}}
  // → stages has no "cleanup"; stages["upscale"]["target"] == "native",
  //   ["enabled"] == true, ["quality"] == "high"
}
TEST_CASE("Show: Clean up and Upscale together load as the Upscale settings") {
  // stages {"cleanup": {"enabled": true}, "upscale": {"enabled": false, "target": "720"}}
  // → no "cleanup"; upscale == normalizeStageParams("upscale", {{"enabled", false}, {"target", "720"}})
}
```
`tests/stage_params_test.cpp`: update the order tests to `{"autoframe", "upscale", "color", "matte", "background"}`, size 5, `stageSchemas()[1].kind == "upscale"`, colour at index 2; `normalizeStageParams("cleanup", {})` throws. Move the `schemaToJson: unavailable…` case onto `findStageSchema("matte")`.
`tests/command_router_test.cpp`: `UnavailFx` returns the reason for `"matte"`; the unavailable-effect case uses `"matte"` throughout (the mechanism is unchanged); `getStageSchemas` size is 5.
`tests/engine_upscale_test.cpp`: delete `Camera: Clean up without an RTX card is bypassed with a readable error` (its replacement is in Task 2).

- [ ] **Step 2: Run them and watch them fail** — verification command. Expected: the two migration cases fail (`stages` still contains `cleanup`), the order/size asserts fail (6 schemas), and `normalizeStageParams("cleanup", {})` does not throw.
- [ ] **Step 3: Implement**
  - Delete the `cleanup` schema.
  - `show.cpp`: a file-local `json migrateLegacyStages(json stages)` implementing E8, called on `stages` in `cameraFromJson` before the normalise loop. A non-object `cleanup` value counts as `enabled: false`.
  - `Camera`: delete `cleanupParams_`, `cleanupBudget_`, `cleanupGuard_` + `cleanupGuard()`, `cleanupTimer_`, `cleanupGpuMs_`, `pollCleanupTimer` and its call, the `"cleanup"` branch of `stageGpuMs`, the `/stages/cleanup/*` publishing and the `cleanupGuard_ = StageGuard{}` reset.
  - `engine_main.cpp`: delete the `stageUnavailable` override and the `cleanupWhy` member/constructor argument (the base default returns empty, E9); keep `rtxWhy` and print it once to stderr when `rtx` is null, like the face detector line.
  - Update comments that name Clean up in `gpu_tools.h`, `rtx_video.h`, `rtx_video.cpp`, `engine_main.cpp`.
- [ ] **Step 4: Run the tests and watch them pass** — verification command. Expected: PASS; `git grep -n -i "cleanup" -- src tests` shows only `srt_cleanup`/FFmpeg/lkc/ffx hits and the migration code.
- [ ] **Step 5: Commit** — `refactor(engine): Clean up is part of Enhance; old shows migrate on load`

---

### Task 4: UI, docs and owner checks

**Files:**
- Modify: `ui/src/shell/StatusBar.tsx`, `ui/src/components/Icon.tsx`, `ui/src/dialogs/SettingsDialog.tsx`, `ui/src/Effects.test.tsx`, `docs/superpowers/specs/2026-10-18-webcam-cleanup-upscale-colour-match-design.md`, `docs/superpowers/manual-checks/webcam-fx.md`, `docs/superpowers/follow-ups.md`

- [ ] **Step 1: Write the failing test** — in `Effects.test.tsx`, change the `upscale` fixture to label `Enhance` with the E2 fields (`target` options `native/720/1080`, `quality` options `fast/balanced/high/ultra`), and in `shows the GPU time and the upscale note` look up `group` by name `Enhance` and also `expect(within(group).getByLabelText('Quality')).toHaveValue('high')` with `quality: 'high'` in the show fixture. Leave the `cleanup` fixture: it stands in for any unavailable effect.
- [ ] **Step 2: Run it and watch it fail** — `cd ui && npm test`. Expected: FAIL only if the Inspector does not render the new fields; if it passes straight away (the Inspector is schema-driven), note that and continue.
- [ ] **Step 3: Implement**
  - `StatusBar.tsx`: remove `cleanup`; `upscale: 'Enhance'`.
  - `Icon.tsx`: remove the `cleanup` icon.
  - `SettingsDialog.tsx`: note text `Enhance on NVIDIA RTX cards uses the NVIDIA RTX Video SDK.`
  - Spec: append `## Amendment 2026-10-27 — Enhance` with E1–E9 verbatim; mark Clean up sections superseded.
  - `manual-checks/webcam-fx.md`: replace Clean up steps with Enhance steps — on the RTX PC: Same size + Ultra on a 1080p webcam shows a VSR watermark when the dev `nvngx_vsr.dll` from `dist/vsr-test/` is installed (`VSR QualityLevel 4`), Fast shows level 1; a 720p webcam at 1080p shows one watermark, not two; AMD PC: Same size shows the E7 note and the picture passes through.
  - `follow-ups.md`: "Enhance quality levels 1–4 and the RGBA input fix: untested on RTX hardware until the owner's 5070 run."
- [ ] **Step 4: Run** `cd ui && npm test && npx tsc --noEmit && npm run build`, then the verification command. Expected: PASS, types clean.
- [ ] **Step 5: Commit** — `feat(ui): Enhance replaces Clean up and Upscale; docs and owner checks`

**Human gate (owner, RTX 5070):** run the new `manual-checks/webcam-fx.md` Enhance steps with the dev VSR DLL, and `bc_tests.exe --test-case="RtxVideo*"` on that PC. The branch is not merged until the owner reports both.
