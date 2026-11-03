# BreadCamera Lean Paths Phase 5b: SDI UYVY Pass-Through in RAM — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A camera with no enabled stage, fed by a DeckLink or AJA input delivering UYVY, hands each picture as UYVY in RAM to outputs that take it at that size and rate (DeckLink/AJA in the same mode, NDI, and plain OMT if lean phase 2 switched it), with no upload, no GPU conversion and no readback. The GPU is used only for a visible preview.

**Architecture:** Phase 5a's pass targets gain a second kind. While UYVY targets exist, the camera says `wantsCpuVideo()`, and `onCpuVideo` (input thread) copies the UYVY picture once into a `CpuImage` (`CpuLayout::Uyvy`), shared by every UYVY target through `OutFrame::uyvy`, with the pass audio FIFO's samples. The outputs already deliver `OutFrame::uyvy` (phases 1–2).

**Tech Stack:** C++20, DeckLink SDK, AJA ntv2, NDI SDK, doctest.

**Spec:** `BreadCamera/docs/superpowers/specs/2026-11-03-breadcamera-lean-paths-design.md` (§3.2, phase 5, SDI UYVY)

**Runs after:** lean phase 5a (`2026-11-03-lean-pass-through-omt.md`) is merged.

## Global Constraints

- A UYVY target: an output with `needsCpu()` and `wantsUyvy(w, h)` true for the input's picture size, not keyed, not half-rate; for DeckLink and AJA outputs also `outRate` equal to the input's rate. The camera has no enabled stage, and the input is Live and delivering `CpuPixel::UYVY` (not RGB 4:4:4).
- One copy per picture, shared by all targets (the input's buffer is valid only during `onCpuVideo`).
- Clock drift between two cards: the output queue keeps the newest frames (it drops the oldest), and a card with no new frame repeats its last one. No new frame sync code.
- Signal loss: pass-through stops while the input is not Live; the tick path's hold and slate run as today.
- Never push. Do not touch `F:\Coding\BreadMedia\BreadReplay`.

## Review Focus

1. **Bit-exactness:** a UYVY target receives the input's bytes unchanged. Test in Task 1.
2. **Mixed outputs:** a UYVY target and an OMT HX output on one camera; the HX output still gets tick frames (the GPU path still runs). Test in Task 1.
3. **Size or rate mismatch** (input 1280×720 to a 1920×1080 DeckLink): not a target; the tick path scales as today. Test in Task 1.
4. **The input losing signal:** targets fall back to the tick path (hold, then slate), with no frame from before the loss replayed later. Test in Task 1.
5. **Two-card sync over 10 minutes** (repeats and drops do not grow beyond clock drift): hardware only; the owner runs it. Named in the final report.

---

### Task 1: UYVY pass targets

**Files:** Modify `src/engine/camera.h/.cpp`, `src/io/decklink_input.cpp` and `src/io/aja_input.cpp` (only if they skip `onCpuVideo` when the camera asks for it), `tests/support/fake_input.h`; test `tests/engine_test.cpp`.

**Interfaces:**
- Consumes: 5a's `passTargets_`/`passMutex_`, pass audio FIFO, `setPreviewQuery`; phase 1's `CpuLayout::Uyvy`, `OutFrame::uyvy`, `wantsUyvy`.
- Produces:
  - `Camera::wantsCpuVideo()`: true while UYVY targets exist.
  - `Camera::onCpuVideo(const CpuVideoFrame&)`: for `CpuPixel::UYVY`, one `CpuImage` (width, height, UYVY rows of `width * 2`; copy row by row from `stride`), then `OutFrame{uyvy, audio}` to each UYVY target under `passMutex_`. Other formats: ignored here.
  - `wantsGpuVideo()` (from 5a) also counts UYVY targets as needing no GPU picture.
  - Per tick, the target list is rebuilt with both kinds; UYVY eligibility needs the input's last `CpuVideoFrame` size and rate (store them atomically in `onCpuVideo`).
  - `FakeInput`: can deliver `onCpuVideo` UYVY frames (its existing GPU frames stay).

- [ ] **Step 1: Failing tests** (`engine_test`, fake input and outputs):
  - `"UYVY pass-through: an SDI-like input reaches a UYVY output byte for byte"`: fake input sends 1920×1080 UYVY (a pattern) through `onCpuVideo`; a `FakeOutput` with `wantsUyvy(1920, 1080)` → its delivered `uyvy` bytes equal the input's; `wantsGpuVideo()` false; no readback slot.
  - `"UYVY pass-through: a GPU output on the same camera keeps the tick path"`: plus a fake GPU output → it gets tick frames.
  - `"UYVY pass-through: a size mismatch is not passed through"`: input 1280×720, output wanting 1920×1080 only → tick path.
  - `"UYVY pass-through: signal loss falls back to the slate"`: the input goes Offline → within the hold time the output gets tick frames (the slate after `holdMs`), no pass frame after the loss.
- [ ] **Step 2: Run** `--test-case=UYVY pass-through*` → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** them, the 5a pass-through tests, then the full verification command → green.
- [ ] **Step 5: Commit, merge into `main`** on `bcam-lean-pass-through-uyvy` (`feat(engine): SDI UYVY passes through in RAM`, `merge: lean phase 5b, UYVY pass-through`). The final report names the two-card 10-minute check as the owner's.
