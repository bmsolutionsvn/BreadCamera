# One size per camera Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every camera produce one frame size per tick, read it back once, and have every output *and* the preview consume that one image — no per-output resize, no preview render or readback.

**Architecture:** Drop the per-output `width`/`height` so `Camera::endTick` has a single variant (the composited frame). Keep one readback slot per camera, shared by every CPU output and (through the frame tap, now carrying the `CpuImage`) the preview, which encodes it at the frame size on a worker thread.

**Tech Stack:** MSVC C++20, D3D11, FFmpeg (H.264 preview), doctest, CMake/Ninja/vcpkg, Vitest/React.

**Spec:** `docs/superpowers/specs/2026-10-16-one-size-per-camera-design.md`

## Global Constraints

- MSVC C++20 `/W4 /permissive- /utf-8 /EHsc`; Windows only.
- The tick is never blocked (spec §4.1); `start()`/`stop()` never block long.
- One composited render per camera per tick is unchanged (spec §3.2).
- A show file written before this change (an output with `width`/`height`) must still load and run — the size is ignored, not an error.
- UK English copy; conventional commits.

## Review Focus

Failure modes the spec implies but no single task's happy path exercises; each gets a test in the task that owns the code:

1. **A camera whose outputs are all GPU-only** (e.g. Fullscreen) with a visible preview must still produce one readback (D5).
2. **An old show file with a per-output size** must load and run at the frame size (migration).
3. **The preview past the encoder cap** must skip that tile and warn, not disable every preview (D9).
4. **Half-rate outputs** must keep their cadence with the single shared readback (D6).
5. **The preview's ≤1-tick image lag** must not stall or double-encode (D8).

---

### Task 1: Remove the per-output size

**Files:**
- Modify: `src/core/show.h` (`OutputConfig`: drop `width`/`height`)
- Modify: `src/core/show.cpp` (`showFromJson`/`outputToJson`: ignore a legacy `width`/`height`)
- Modify: `src/engine/camera.cpp` (`endTick`: one variant)
- Modify: `src/engine/command_router.cpp` (`validateOutput`: drop the size checks)
- Modify: `ui/src/api/protocol.ts`, `ui/src/dialogs/AddOutputDialog.tsx`, `ui/src/inspector/Inspector.tsx` (drop the size field/copy)
- Test: `tests/command_router_test.cpp`, `tests/show_test.cpp`, `ui/src/dialogs/AddOutputDialog.test.tsx`

**Interfaces:**
- Produces: `OutputConfig` with no `width`/`height`; `Camera::endTick` builds one `Variant` (the frame) and no `fit`.

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/show_test.cpp
TEST_CASE("a show file with a legacy per-output size still loads at the frame size") {
  auto j = R"({"settings":{"width":1920,"height":1080,"fps":"60/1"},
               "cameras":[{"id":"a","name":"A","input":{"kind":"srt","params":{"url":"srt://x"}},
               "outputs":[{"id":"o","kind":"srt","width":1280,"height":720,"params":{}}]}]})"_json;
  bc::Show s = bc::showFromJson(j);            // must not throw
  CHECK(s.cameras.size() == 1);
}
```
```cpp
// tests/command_router_test.cpp
TEST_CASE("validateOutput ignores a per-output size") {
  bc::OutputConfig o; o.kind = "omt"; o.params = {{"mode", "hx"}};
  CHECK_NOTHROW(bc::validateOutput(o));        // no width/height to validate
}
```

- [ ] **Step 2: Run them and watch them fail**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "legacy per-output size,validateOutput ignores*"`
Expected: FAIL to compile (the test references fields/behaviour that do not exist yet) — or pass against the old struct; adjust until it fails for the right reason.

- [ ] **Step 3: Implement**

`OutputConfig` loses `width`/`height`; `showFromJson`/`outputToJson` simply ignore those JSON keys (no error); `validateOutput` drops its size checks; the UI drops the size control and the "Composited · same as show" subtitle becomes the only behaviour. In `Camera::endTick` replace the `variants`/`needed`/`fit` block (`camera.cpp:242-306`) with: `frame` is the one image; one readback (Task 2); each output submits `OutFrame{index, rate, frame, cpu, audio, 48000, 2, hasAlpha}`.

- [ ] **Step 4: Run the tests and watch them pass**

Run: `scripts/test.ps1 -Filter "legacy per-output size,validateOutput ignores*,show*,CommandRouter*"`
Expected: PASS.

- [ ] **Step 5: Run the UI suite**

Run: `npm test` (in `ui`)
Expected: PASS (the size-field test removed/updated).

- [ ] **Step 6: Commit**

```bash
git add src/core/show.h src/core/show.cpp src/engine/camera.cpp src/engine/command_router.cpp \
        ui/src/api/protocol.ts ui/src/dialogs/AddOutputDialog.tsx ui/src/inspector/Inspector.tsx \
        tests/show_test.cpp tests/command_router_test.cpp ui/src/dialogs/AddOutputDialog.test.tsx
git commit -m "feat(engine): one frame size per camera, drop the per-output size"
```

---

### Task 2: One readback, shared with the preview

**Files:**
- Modify: `src/engine/camera.h` (`readbacks_` → one `ReadbackState`; `endTick` takes `bool previewWanted`)
- Modify: `src/engine/camera.cpp` (one readback when any CPU output or a visible preview needs it)
- Modify: `src/engine/engine.h`/`engine.cpp` (a `previewWanted_` callback; the tap carries the `CpuImage`)
- Modify: `src/engine/engine_main.cpp` (wire `previewWanted_` from the `PreviewHub`)
- Modify: `src/engine/preview_service.h`/`.cpp` (consume the tap's `CpuImage`; no `toBgra8`, no `readback`)
- Test: `tests/engine_test.cpp` / `tests/preview_test.cpp`

**Interfaces:**
- Consumes: Task 1's single-variant `endTick`.
- Produces: `Engine::setPreviewWanted(std::function<bool(const std::string&)>)`; the tap type becomes
  `std::function<void(uint64_t, const std::vector<bc::FrameTap>&)>` with
  `struct FrameTap { std::string id; TextureRef tex; std::shared_ptr<const CpuImage> cpu; };`

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/engine_test.cpp — a GPU-only camera with a visible preview does exactly one readback
TEST_CASE("a GPU-only camera does one readback when a preview wants it") {
  // Build a camera whose only output needsCpu() == false (fullscreen), set previewWanted -> true,
  // tick, and assert cam.readbackSlotCount() == 1 (was 0 today).
}
```
```cpp
// tests/preview_test.cpp — the preview encodes the tap's image, not its own readback
TEST_CASE("the preview encodes the shared image at the frame size") {
  // Drive PreviewService::onTick with a FrameTap carrying a known CpuImage; assert an encoded
  // packet is emitted and no converter call is made for the preview.
}
```

- [ ] **Step 2: Run them and watch them fail**

Run: `scripts/test.ps1 -Filter "GPU-only camera*,preview encodes the shared*"`
Expected: FAIL to compile — `previewWanted`/`FrameTap` do not exist.

- [ ] **Step 3: Implement**

Keep one `ReadbackState` per camera. In `endTick`, if any output `needsCpu()` **or** `previewWanted(id)`, run the existing async slot once for the frame and hand the same `shared_ptr<const CpuImage>` to every CPU output and out through the tap. `PreviewService::onTick` uses `tap.cpu` directly (drop `conv_.toBgra8`/`conv_.readback`), encoding at the frame's size; keep the wire `size` but ignore it (D7).

- [ ] **Step 4: Run the tests and watch them pass**

Run: `scripts/test.ps1 -Filter "GPU-only camera*,preview encodes the shared*,Preview*,Engine*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/engine/camera.h src/engine/camera.cpp src/engine/engine.h src/engine/engine.cpp \
        src/engine/engine_main.cpp src/engine/preview_service.h src/engine/preview_service.cpp \
        tests/engine_test.cpp tests/preview_test.cpp
git commit -m "feat(engine): one readback per camera, shared with the preview"
```

---

### Task 3: Focus view — click a tile for one camera, large

**Files:**
- Modify: `ui/src/multiview/Multiview.tsx` (a `focused` id; a click focuses; a "Grid" button and Esc clear it)
- Modify: `ui/src/App.tsx` (focus state; the Esc handler)
- Modify: `ui/src/app.css` (the single-tile layout)
- Test: `ui/src/Workspace.test.tsx`

**Interfaces:**
- Consumes: project 1's one-size preview.
- Produces: focus is UI state (`focused: string | null`); while focused the preview subscription is `[focused]` only; Esc, a "Grid" button, or clicking the focused picture returns to the grid (subscription = all visible ids).

- [ ] **Step 1: Write the failing test** — append to `Workspace.test.tsx`:

```tsx
test('clicking a tile focuses one camera; Esc returns to the grid', async () => {
  // two cameras; click the second tile -> only that camera is subscribed / shown large;
  // fire Escape -> both cameras are subscribed again.
});
```

- [ ] **Step 2: Run it and watch it fail**

Run: `npm test -- Workspace` (in `BreadCamera/ui`)
Expected: FAIL — there is no focus behaviour.

- [ ] **Step 3: Implement**

`Multiview` gains a `focused` prop + `onFocus`; when set, render the one camera full-size with a "Grid" button, and `setPreviewSubscription([focused], 'small')` (others stop). Clicking the focused picture, Esc, or the button calls `onFocus(null)` → subscription returns to all ids.

- [ ] **Step 4: Run it and watch it pass**

Run: `npm test -- Workspace`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add ui/src/multiview/Multiview.tsx ui/src/App.tsx ui/src/app.css ui/src/Workspace.test.tsx
git commit -m "feat(ui): focus one camera from the Multiview"
```

---

### Task 4: Preview encode off the tick, with a cap

**Files:**
- Modify: `src/engine/preview_service.h`/`.cpp` (a worker thread + a one-slot queue per encoder; a cap on concurrent encoders)
- Test: `tests/preview_test.cpp`

**Interfaces:**
- Consumes: Task 2's tap.
- Produces: `PreviewService` encodes on a worker; past `kMaxPreviewEncoders` visible cameras it skips the tile and sets `state_.set("/engine/warnings/preview", …)`.

- [ ] **Step 1: Write the failing test**

```cpp
TEST_CASE("the preview cap warns instead of disabling every preview") {
  // Subscribe more cameras than the cap; assert the excess tiles are skipped and the warning is
  // set, while the within-cap tiles still encode.
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `scripts/test.ps1 -Filter "preview cap warns*"`
Expected: FAIL — no cap exists.

- [ ] **Step 3: Implement**

Move `enc->encode(...)` to a per-encoder worker (the frame tap drops the newest `CpuImage` into a slot and returns immediately). Cap the number of live encoders.

- [ ] **Step 4: Run the tests and watch them pass**

Run: `scripts/test.ps1 -Filter "preview cap warns*,Preview*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/engine/preview_service.h src/engine/preview_service.cpp tests/preview_test.cpp
git commit -m "perf(engine): encode previews off the tick, cap concurrent previews"
```

---

### Task 5: Whole-suite check and the follow-up note

**Files:**
- Modify: `docs/superpowers/follow-ups.md`
- Modify: `docs/superpowers/specs/2026-10-08-breadcamera-design.md` (§4.3 per-output size removed)

- [ ] **Step 1: Update the docs**

Note in `follow-ups.md`: the per-output size is gone (one frame size per camera); the preview shares the one readback and is superseded by project 3 (native Multiview). Amend spec §4.3.

- [ ] **Step 2: Run the whole C++ suite**

Run: `scripts/test.ps1`
Expected: PASS (the pre-existing M1/M2 acceptance overrun bounds may still fail — record them by name; they are unrelated).

- [ ] **Step 3: Run the UI suite**

Run: `npm test` (in `ui/`)
Expected: PASS.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/follow-ups.md docs/superpowers/specs/2026-10-08-breadcamera-design.md
git commit -m "docs: record the one-size-per-camera fan-out"
```

---

## Self-Review

- **Spec coverage:** D1 per-output size removed (Task 1), D2 one readback (Task 2), D3 tap carries the image (Task 2), D4 encode off-tick (Task 3), D5 previewWanted (Task 2), D6 half-rate kept (Task 1 leaves `halfRate`), D7 wire size ignored (Task 2), D8 lag accepted (Task 2), D9 cap+warn (Task 3).
- **Step scan:** each step names files, signatures and the test assertion or command.
- **Type consistency:** `FrameTap{id,tex,cpu}` and `setPreviewWanted` are used identically in Tasks 2–3.
- **Review Focus:** GPU-only + preview (Task 2 test), legacy size (Task 1 test), cap+warn (Task 3 test), half-rate (Task 2 keeps the submit branch), ≤1-tick lag (Task 2's async slot unchanged).
- **Proportion:** the design carries the evidence and calls; the plan carries signatures and tests.
