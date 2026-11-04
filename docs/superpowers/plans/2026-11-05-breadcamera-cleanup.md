# BreadCamera Clean-up: Idle OMT Inputs, a Flaky Test, the Bridge Pin — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Three small follow-ups from the lean-paths runs (2026-11-03 to 11-05).

**Why:**
1. The reviewer's benchmark on main 2d7e47d (2026-11-05, `.superpowers/omt-bench.ps1`): four cameras on OMT-HX inputs with **no outputs and no preview** still decode every frame. GPU 46.7% (video codec 37.1); before the lean work it was 44.5%. `Camera::evaluatePass` keeps decoding when there are no outputs: "only a camera whose whole output set is pass targets may stop". A camera with nothing that uses its pictures should only receive.
2. `PreviewService: a second frame inside 33 ms is suppressed` failed once during a full run (`5 > 5`) and passed when run alone. It is a timing flake.
3. OMT-Bridge pins OMT-HX at `a392952`; OMT-HX `main` is `afc27cc` (the coded/picture switch on one sender, and the GPU UYVY path).

**Tech Stack:** C++20, doctest. Repos: BreadCamera (Tasks 1–2), then OMT-Bridge (Task 3).

## Global Constraints

- Task 1: a camera with **no outputs, no stage on, and no preview wanted** (`previewQuery_` false) stops decoding an OMT input. It asks for the coded receiver, as full pass-through does: `wantCodedOmt_` true, `wantGpuVideo_` false. It stays Live off the coded frames, which already count as its rate. When an output is added, or the preview is wanted again, pictures come back through the existing path within the existing bound. The keyframe request already exists for HX.
- Task 1 covers OMT inputs only. SRT/NDI/SDI inputs with no outputs keep today's behaviour. Log a `Ruling:` if the code shows an SRT input can follow cheaply, but do not build it.
- Task 2: make the test deterministic without loosening its bound. Inject or step the clock the way the file's neighbours do. If the service has no clock seam, add the smallest one.
- Task 3: move the pin only. Commit the pin before building (`build.ps1` runs `git submodule update`).
- Never push.

## Review Focus

1. A camera with no outputs and no preview, on an OMT-HX input: `framesDecoded` stays flat for 1 s, and the camera stays Live with a nonzero rate.
2. Adding an OMT output (Full or HX) to that camera: the output gets pictures within 2 s.
3. The preview turning on for that camera: decoded pictures resume within 1 s.
4. A camera with an effect stage on and no outputs keeps decoding (a stage may need its frames).
5. The Bridge suite is green on the new pin.

---

### Task 1: An OMT input with nothing using its pictures only receives

**Files:** Modify `src/engine/camera.cpp` (`evaluatePass`), `camera.h` if needed. Test: `tests/engine_test.cpp` and/or `tests/omt_input_test.cpp`, beside the pass-through tests.

- [ ] **Step 1: Failing tests** for Review Focus 1–4. Drive `Camera` the way the existing pass-through tests do.
- [ ] **Step 2: Run** `./build/release/bc_tests.exe -tc="*pass-through*,*no consumer*"` (after `scripts/build.ps1 -SkipUi`) → the new ones fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** them, then the full suite (the brief's excludes) → green.
- [ ] **Step 5: Commit** `perf(engine): an OMT input with no consumer only receives`.

### Task 2: The PreviewService suppression test is deterministic

**Files:** `tests/preview_test.cpp`, and `src/engine/preview*.{h,cpp}` only if a clock seam is needed.

- [ ] **Step 1:** Run the test 50 times in a loop (`-tc="PreviewService: a second frame inside 33 ms*"`) beside a busy full suite run, and record how often it fails.
- [ ] **Step 2:** Make the timing come from a controlled clock, with the same 33 ms rule and the same expected counts.
- [ ] **Step 3:** 50 runs pass, then the full suite → green.
- [ ] **Step 4: Commit** `test(preview): the suppression test controls its own clock`.
- [ ] **Step 5: Merge `bcam-cleanup` into `main`** (`merge: idle OMT inputs only receive; preview test is deterministic`). Then do the BreadReplay compile check.

### Task 3: OMT-Bridge moves to OMT-HX `afc27cc`

**Files:** `third_party/OMT-HX` (the pin).

- [ ] **Step 1:** In OMT-Bridge on `ob-omthx-pin-afc27cc`: `git -C third_party/OMT-HX fetch sibling` then `git -C third_party/OMT-HX checkout afc27cc`, stage `third_party/OMT-HX`, commit `chore: OMT-HX afc27cc (coded/picture switch, GPU UYVY)`.
- [ ] **Step 2:** Build and run the suite: `scripts/test.ps1`, or the icu workaround in the brief → green.
- [ ] **Step 3: Merge into `main`** (`merge: OMT-HX afc27cc`).
