# BreadReplay: OMT-HX Output Gets Its GPU Frame Again — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** BreadReplay's unkeyed OMT-HX output sends video again.

**Why:** Since the OMT copy-paths merge (BreadCamera 91a4ef0, 2026-11-03), an unkeyed HX `OmthOutput` says `needsCpu() == false` and `wantsGpuRgba8() == true`: it encodes `OutFrame::rgba8` on the GPU and sends nothing from `bgra`. BreadCamera's camera fills `rgba8`, but BreadReplay's `OutputPlayer::submit` (`src/play/output_player.cpp`) only asks `needsCpu()` and fills `bgra`, so an HX output gets frames with no picture. Found by the encode-on-demand run's review (2026-11-04).

**Architecture:** `OutputPlayer::submit` also makes the GPU frame, once per tick, when any output `wantsGpuRgba8()`, as BreadCamera's `Camera::endTick` does: `Converter::toBgra8(linear, w, h, /*swapRb=*/false)` (true RGBA order). `Renderer` gets a small `toRgba8` beside `toCpu`.

**Tech Stack:** C++20, BreadCamera libs, doctest.

**Spec:** none; the rule is BreadCamera's `Output` contract (`src/io/output.h`): `rgba8` is set when any output `wantsGpuRgba8()`, and `needsCpu()` is false while it says true.

## Global Constraints

- `rgba8` is `R8G8B8A8_UNORM` in true RGBA order at the show raster (`outW × outH`), made only when some output wants it, once per tick, shared by all outputs.
- `bgra` keeps today's rule (`needsCpu()`).
- BreadReplay output frames have no alpha; keyed outputs keep the CPU path as they decide themselves.
- Never push. Do not change `ui/`.

## Review Focus

1. **An HX OMT output on air:** an omthx receiver gets pictures with the right colour. Test in Task 1 (hardware HEVC).
2. **An idle HX output** (encode on demand): no `rgba8` is made while nobody pulls (`wantsGpuRgba8()` false then). Test in Task 1.
3. **Mixed outputs** (an HX OMT output and a CPU output): both get their picture from one tick. Test in Task 1.
4. **Nothing on air** (`pic` null): no conversion, no throw. Test in Task 1.

---

### Task 1: `OutputPlayer` fills `rgba8`

**Files:** Modify `BreadReplay/src/play/renderer.h/.cpp`, `src/play/output_player.cpp`; test `tests/output_player_test.cpp`.

**Interfaces:**
- Produces: `bc::TextureRef Renderer::toRgba8(const bc::TextureRef& linear);` — `conv_.toBgra8(*linear, outW_, outH_, false)`.
- `OutputPlayer::submit`: `needGpu` = any output's `wantsGpuRgba8()`; when `needGpu && pic`, `frame.rgba8 = renderer_.toRgba8(pic)`.

- [ ] **Step 1: Failing tests**
  - A fake output with `needsCpu() == false`, `wantsGpuRgba8() == true` → after a tick with a picture, its frame has `rgba8` set at `outW × outH` and no `bgra`; a fake that says false for both gets neither.
  - The same fake plus a CPU fake → each gets its picture from the same tick.
  - `pic` null → neither picture, no throw.
  - Hardware HEVC (early return otherwise): a real `bc::OmthOutput` (Hx, unkeyed) driven by the player with a solid red picture, an omthx `Receiver` connected → a picture arrives within 3 s, centre red within ±8.
- [ ] **Step 2: Run** `./build/release/br_tests.exe -tc="OutputPlayer*"` (after `scripts/build.ps1`) → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** them, then the full BreadReplay suite → green.
- [ ] **Step 5: Commit on `br-hx-output-gpu-frame`, merge into `main`** (`fix(play): HX OMT outputs get the GPU frame`, `merge: BreadReplay HX output sends video again`).
