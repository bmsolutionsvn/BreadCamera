# Per-camera size (project 2a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the show-wide resolution. Each camera's frame is its input's size; the show **rate** stays until project 2b.

**Architecture:** `ShowSettings` loses `width`/`height` and `Camera::setFormat` goes; `beginTick` keeps `toLinear(input)` at its own size. The one shared slate texture becomes a source the camera fits to its own size only on the no-picture path. SRT re-opens on a size change; the preview caps its encode size at 1280×720.

**Tech Stack:** MSVC C++20, D3D11, FFmpeg, doctest, CMake/Ninja/vcpkg, Vitest/React.

**Spec:** `docs/superpowers/specs/2026-10-16-per-camera-size-design.md`

## Global Constraints

- MSVC C++20 `/W4 /permissive- /utf-8 /EHsc`; the tick is never blocked.
- One composited render per camera per tick unchanged.
- A show file written before this change still loads (legacy `settings.resolution` ignored).
- `Show::kSchemaVersion` stays 1; new shows do not emit `settings.resolution`.
- UK English copy; conventional commits.

## Review Focus

1. **A camera with no picture** must show the slate at the camera's size, not the old show size (D2).
2. **A mid-show input size change** must re-open SRT and reset the readback (D6 + project 1).
3. **A 4K camera's preview** must not encode at 4K (D8 cap).
4. **Calibration** must keep its 960×540 capture, not switch to the native size (D5).
5. **A legacy show file** with `settings.resolution` must load (D7).

---

### Task 1: Remove the show-wide resolution

**Files:**
- Modify: `src/core/show.h` (`ShowSettings`: drop `width`/`height`), `src/core/show.cpp` (ignore a legacy `resolution`; stop emitting it)
- Modify: `src/engine/camera.h`/`.cpp` (drop `setFormat`; `beginTick` keeps the input size; the slate is fitted per camera)
- Modify: `src/engine/engine.h`/`.cpp` (slate source; drop `settings_.width/height`; drop `/engine/resolution`)
- Modify: `src/engine/command_router.cpp` (ignore a legacy `resolution`)
- Modify: `ui/src/dialogs/SettingsDialog.tsx`, `ui/src/shell/StatusBar.tsx`, `ui/src/api/protocol.ts`, `ui/src/multiview/Multiview.tsx`
- Test: `tests/show_test.cpp`, `tests/engine_test.cpp`, `ui/src/dialogs/SettingsDialog.test.tsx`, `ui/src/Workspace.test.tsx`

**Interfaces:**
- Produces: `ShowSettings` with no `width`/`height`; `Camera::beginTick(now, slateSource)` fits the slate to the camera's size; `Camera` tracks its last frame size (`lastSize_`, default 1280×720) for the no-picture path.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/show_test.cpp
TEST_CASE("a legacy show resolution is ignored on load") {
  auto j = R"({"schemaVersion":1,"settings":{"rate":"60/1","resolution":"1920x1080"},
               "cameras":[]})"_json;
  bc::Show s = bc::showFromJson(j);           // must not throw
  CHECK(s.settings.rate == bc::Rate{60, 1});
}
```
```cpp
// tests/engine_test.cpp — the frame is the input size, not a show size
TEST_CASE("Camera: the frame keeps the input's size") {
  // A fake input at 640x360 -> the camera's composited frame is 640x360 (no scale to a show size).
}
```

- [ ] **Step 2: Run them and watch them fail**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "legacy show resolution*,Camera: the frame keeps*"`
Expected: FAIL to compile (the fields/behaviour are gone) — or fail for the right reason.

- [ ] **Step 3: Implement**

`ShowSettings` loses the fields; `showFromJson` ignores `resolution` and `showToJson` stops emitting it. `beginTick` drops the `fit` to `width_/height_`; keep the `toLinear` result as the frame. `Engine` keeps a slate **source** texture (the loaded image at a base size, or the solid at a base size) and hands it to `beginTick`, which fits it to the camera's size only when there is no picture (cache by size). Drop `setFormat`, `settings_.width/height`, `/engine/resolution`. UI: remove the Resolution field, its validation/protocol field, the status-bar size label, and letterbox a non-16:9 tile.

- [ ] **Step 4: Run the tests and watch them pass**

Run: `scripts/test.ps1 -Filter "legacy show resolution*,Camera: the frame keeps*,show*,Engine*,Camera*"`
Expected: PASS.

- [ ] **Step 5: Run the UI suite**

Run: `npm test` (in `ui`) and `npx tsc --noEmit`.
Expected: PASS, types clean.

- [ ] **Step 6: Commit**

```bash
git add src/core/show.h src/core/show.cpp src/engine/camera.h src/engine/camera.cpp \
        src/engine/engine.h src/engine/engine.cpp src/engine/command_router.cpp \
        ui/src/dialogs/SettingsDialog.tsx ui/src/shell/StatusBar.tsx ui/src/api/protocol.ts \
        ui/src/multiview/Multiview.tsx tests/show_test.cpp tests/engine_test.cpp \
        ui/src/dialogs/SettingsDialog.test.tsx ui/src/Workspace.test.tsx
git commit -m "feat(engine): the frame is the camera's input size"
```

---

### Task 2: SRT re-opens on a frame-size change

**Files:**
- Modify: `src/io/srt_output.cpp`/`.h` (re-open the encoders when the frame size differs from `encW_`/`encH_`)
- Test: `tests/srt_output_test.cpp`

**Interfaces:**
- Consumes: Task 1's per-camera frame size.
- Produces: `SrtOutput::submit` re-inits when `f.bgra->width/height` changes.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("SrtOutput: re-opens when the frame size changes") {
  // open with 640x360, submit a 1280x720 frame, assert the encoder re-initialised (new dimensions).
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `scripts/test.ps1 -Filter "SrtOutput: re-opens*"`
Expected: FAIL — the encoder keeps the first size.

- [ ] **Step 3: Implement**

In `SrtOutput::submit`, if the incoming `CpuImage` size differs from `encW_`/`encH_`, stop and re-open the encoders at the new size (the same path as the first open).

- [ ] **Step 4: Run the tests and watch them pass**

Run: `scripts/test.ps1 -Filter "SrtOutput*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/io/srt_output.cpp src/io/srt_output.h tests/srt_output_test.cpp
git commit -m "fix(io): SRT re-opens its encoders when the frame size changes"
```

---

### Task 3: Cap the preview encode size

**Files:**
- Modify: `src/engine/preview_service.cpp`/`.h` (open the encoder at `min(frame, 1280×720)`; downscale the shared image on the worker with swscale)
- Test: `tests/preview_test.cpp`

**Interfaces:**
- Consumes: Task 1's per-camera frame size.
- Produces: the preview encodes a 4K frame at 1280×720 (never native 4K).

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("the preview caps its encode size at 1280x720") {
  // feed a 3840x2160 CpuImage; assert the encoded packet is 1280x720, not 3840x2160.
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `scripts/test.ps1 -Filter "preview caps its encode size*"`
Expected: FAIL — it encodes at the frame size.

- [ ] **Step 3: Implement**

On the encoder worker, when the shared image exceeds 1280×720, swscale it down before encoding; open the encoder at the capped size. Keep the off-tick worker and the count cap.

- [ ] **Step 4: Run the tests and watch them pass**

Run: `scripts/test.ps1 -Filter "preview caps its encode size*,Preview*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/engine/preview_service.cpp src/engine/preview_service.h tests/preview_test.cpp
git commit -m "perf(engine): cap the preview encode size"
```

---

### Task 4: Whole-suite check and docs

**Files:**
- Modify: `docs/superpowers/follow-ups.md`, `docs/superpowers/specs/2026-10-08-breadcamera-design.md` (§1.3/§4.3: no show-wide resolution)

- [ ] **Step 1: Update the docs** (the show has no resolution; each camera is its input size; SRT re-opens on a size change; the preview caps at 1280×720; calibration keeps 960×540).

- [ ] **Step 2: Run the whole C++ suite** — `scripts/test.ps1`; expect PASS except the documented `e2e_m1/m2` overrun flake (report it by name if it appears).

- [ ] **Step 3: Run the UI suite** — `npm test` in `ui/`.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/follow-ups.md docs/superpowers/specs/2026-10-08-breadcamera-design.md \
        docs/superpowers/specs/2026-10-16-per-camera-size-design.md \
        docs/superpowers/plans/2026-10-16-per-camera-size.md
git commit -m "docs: record the per-camera size change"
```

---

## Self-Review

- **Spec coverage:** D1 fields gone + no scale (Task 1), D2 per-camera slate (Task 1), D3 router/UI (Task 1), D4 letterbox (Task 1), D5 calibration untouched (Task 1 leaves `capture(...,960,540)`), D6 SRT re-open (Task 2), D7 schema (Task 1), D8 preview cap (Task 3), D9 status bar (Task 1).
- **Step scan:** each step names files, signatures and the test assertion or command.
- **Type consistency:** `beginTick(now, slateSource)` and `lastSize_` are used as defined in Task 1; Tasks 2–3 consume the frame size it produces.
- **Review Focus:** no-picture slate (Task 1), size change (Tasks 1–2), 4K preview (Task 3), calibration (Task 1 leaves it), legacy show (Task 1).
- **Proportion:** the design carries the reasoning; the plan carries signatures and tests.