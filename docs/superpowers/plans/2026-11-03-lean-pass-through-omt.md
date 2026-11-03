# BreadCamera Lean Paths Phase 5a: OMT → OMT Pass-Through — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A camera with no enabled stage, fed by an OMT source, hands the source's coded frames to its OMT outputs of the same mode (Full → Full, HX → HX) with no decode and no encode. It decodes only while a preview is visible or another output needs a picture. Turning an effect on switches those outputs to the normal tick path at a tick boundary, never sending black.

**Architecture:** `OutFrame` gains a shared `omthx::CodedFrame`. The camera publishes, under a small mutex, the outputs that pass through (its "pass targets"). The input thread calls `Camera::onCodedOmt`, which submits each coded frame (with the audio queued since the last one) to those targets and returns whether any target wants a keyframe. `OmtInput` opens one `Coded` receiver while the camera wants coded frames, and decodes with OMT-HX's `FrameDecoder(Nv12)` only while `wantsGpuVideo()`. OMT-HX lets one sender switch between coded frames and pictures.

**Tech Stack:** C++20, OMT-HX (`Sender::sendCoded`, `FrameDecoder`), doctest.

**Spec:** `BreadCamera/docs/superpowers/specs/2026-11-03-breadcamera-lean-paths-design.md` (§3.2, phase 5, coded OMT first)

**Runs after:** lean phase 4 is merged, and the OMT-Bridge phase 1 and 2 plans are merged in OMT-HX (`FrameDecoder` with `Nv12`).

## Global Constraints

- A camera passes through only while: no stage is enabled (`enabled` false for every entry in `stages`); the input is `omt` and Live; the output takes the source's mode (`OmthOutput` with the same `Mode`; keyed or not, coded frames keep their alpha); the output is not half-rate.
- Size and rate are the source's by construction (coded frames are not resized).
- Pass targets get no tick frames. Other outputs on the same camera keep the tick path.
- Switching paths never sends black: on leaving pass-through, a target sends nothing until the tick path has a fresh decoded frame, so its receivers hold their last picture. Entering pass-through clears the camera's held frame, so a later switch back waits for a fresh frame rather than repeating a stale one.
- Plain OMT stays standard on the wire. Never push. Do not touch `F:\Coding\BreadMedia\BreadReplay`.

## Review Focus

1. **Turning an effect on mid-stream:** a receiver on the output sees no black and no slate frame. Test in Task 3 (frame-by-frame).
2. **An output removed or reconfigured while the input thread submits to it:** no use after free. Task 2 guards targets with a mutex taken by every slot change; test by reconfiguring in a loop while frames flow.
3. **A receiver joining the pass-through output mid-GOP** gets a picture within about one GOP; sooner with keyframe requests relayed. Test in Task 3.
4. **Audio** on a pass-through output keeps flowing and stays within one frame of the video. Test in Task 3 (audio frames arrive; timestamps within one frame period of the video's).
5. **The queue dropping a coded frame** (`consumeDrop`) asks the source for a keyframe, since the receiver's picture breaks until the next one. Test in Task 2 (a slow output).

---

### Task 1: OMT-HX — one sender may switch between coded frames and pictures

**Files:** Modify `OMT-HX/include/omthx/omthx.h` (the `sendCoded` comment), `OMT-HX/src/link.cpp`; test `OMT-HX/tests/link_test.cpp`.

**Interfaces:**
- Behaviour: `Sender::Impl` remembers whether its last video frame was coded. Picture after coded → the next encode is a keyframe (`policy = KeyframePolicy()`). Coded after picture → `forward = true` (a relay asks its source for a keyframe). The `sendCoded` comment drops "not both" and says this.

- [ ] **Step 1: Failing test** (hardware HEVC): `"a sender switches from coded frames to pictures and back"`: relay sender B gets coded frames from an HX source (Coded receiver) for 1 s, then pictures (`sendVideo` BGRA) for 1 s, then coded again; a `Uyvy` receiver on B gets pictures in all three spells (at least 10 each); `keyframeRequested()` is true soon after the switch back to coded.
- [ ] **Step 2: Run** → the third spell or the flag fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** it, then the full OMT-HX suite → green.
- [ ] **Step 5: Commit on `omthx-switch-coded`, merge into OMT-HX `main`** (`feat: a sender may switch between coded frames and pictures`, `merge: coded/picture switch`).

### Task 2: The camera's pass-through plumbing

**Files:**
- Modify: `src/io/output.h`, `src/io/input.h`, `src/engine/camera.h/.cpp`, `src/io/omt_output.h/.cpp`
- Test: `tests/engine_test.cpp` (with `FakeOutput` extended), `tests/omt_output_test.cpp`

**Interfaces:**
- Produces:
  - `output.h`: forward-declare `namespace omthx { struct CodedFrame; enum class Mode; }`. `OutFrame::coded` (`std::shared_ptr<const omthx::CodedFrame>`; when set, the frame carries no picture). `Output`: `virtual bool takesCodedOmt(omthx::Mode) const { return false; }`, `virtual bool takeKeyframeRequest() { return false; }` (any thread, true once).
  - `input.h`: `InputSink::wantsCodedOmt() const { return false; }`; `virtual bool onCodedOmt(std::shared_ptr<const omthx::CodedFrame>) { return false; }` — input thread; true = a downstream receiver wants a keyframe.
  - `Camera`:
    - `passMutex_` and `std::vector<Output*> passTargets_`; the tick thread rebuilds the list each tick from the Global Constraints, and every place that stops, resets or replaces an output slot removes it from the list under `passMutex_` first.
    - `wantsCodedOmt()`: some slot's output takes coded frames of either mode, and no stage is enabled.
    - `wantsGpuVideo()`: false only when every output is a pass target, no stage is enabled, and no preview is wanted. Preview comes from a `std::function<bool()>` the engine sets with `setPreviewQuery` (the same `previewWanted_(id)` the engine passes to `endTick`).
    - `onCodedOmt`: under `passMutex_`, submit `OutFrame{coded, audio}` to each target whose `takesCodedOmt(frame.mode)`. The audio is a copy of a pass audio FIFO that `onAudio` fills while targets exist. Return the OR of the targets' `takeKeyframeRequest()`.
    - The tick fan-out and the `anyCpu`/`anyGpu` loop skip pass targets.
    - On entering pass-through, clear `lastFrame_` and `sawFrame_` (Global Constraints).
  - `FakeOutput`: a flag to take coded frames of a mode, a count of coded deliveries, and a settable keyframe request.
  - `OmthOutput`: `takesCodedOmt(m)` = `m == mode_`. `deliver` with `f.coded` → `send_->sendCoded(*f.coded)`, audio with `ts = f.coded->timestamp`, then the tally/receivers update as today. Keyframe request = `send_->keyframeRequested()` after the send, or `consumeDrop()`; held in an atomic for `takeKeyframeRequest()`.

- [ ] **Step 1: Failing tests**
  - `engine_test` with a fake input that calls `onCodedOmt` (Full mode, a dummy `CodedFrame`) and one coded-taking `FakeOutput`, no stages → the output counts coded deliveries and no tick picture; `wantsGpuVideo()` false; with `setPreviewQuery` returning true → `wantsGpuVideo()` true.
  - The same with a second, plain CPU `FakeOutput` → `wantsGpuVideo()` true; the CPU output gets tick frames, the coded one none.
  - Enabling a stage → within two ticks the coded output leaves the targets (`onCodedOmt` no longer reaches it) and `wantsCodedOmt()` turns false.
  - A target's keyframe request makes `onCodedOmt` return true once.
  - Reconfiguring the camera's outputs 100 times on the tick thread while another thread calls `onCodedOmt` → no crash.
  - `omt_output_test`: `takesCodedOmt` true for its own mode only; a started HX `OmthOutput` given `OutFrame`s with `coded` from a real HX source's `Coded` receiver → an omthx receiver on it gets pictures, sender `stats().encoder == "none"` (hardware HEVC).
- [ ] **Step 2: Run** `--test-case=*pass-through*,OmthOutput*` (name the engine tests with "pass-through") → build errors.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** them, then the full verification command → green.
- [ ] **Step 5: Commit** on `bcam-lean-pass-through-omt`: `feat(engine): cameras pass OMT coded frames to OMT outputs`.

### Task 3: `OmtInput` reads coded frames and decodes on demand

**Files:** Modify `src/io/omt_input.h/.cpp`, `src/engine/engine.cpp` (`setPreviewQuery` when a camera is made); test `tests/omt_input_test.cpp`, `tests/engine_test.cpp` or a new e2e test.

**Interfaces:**
- Consumes: `wantsCodedOmt`, `onCodedOmt`, `wantsGpuVideo` (Task 2); `omthx::FrameDecoder(PixelFormat::Nv12)`.
- Behaviour: while `sink_->wantsCodedOmt()` (checked when opening, and once a second; a change reopens the receiver), open `PixelFormat::Coded`. Each coded frame → `onCodedOmt` (true → `requestKeyframe()`). While `wantsGpuVideo()`, decode it with one `FrameDecoder(Nv12)` and upload exactly as the Nv12 path does today; turning decoding on resets the decoder and calls `requestKeyframe()`. `wantsCodedVmx` (BreadReplay's recorder) keeps its own path, unchanged.

- [ ] **Step 1: Failing tests** (real network):
  - `"OMT pass-through: an idle preview decodes nothing"` (plain OMT, no hardware): an omthx `Mode::Full` source → a camera with one Full `OmthOutput`, no stages, no preview → an omthx receiver on the output gets pictures; the input's decoder stats show no decoded frames (add `OmtInput::framesDecoded()`).
  - `"OMT pass-through: an effect turned on mid-stream sends no black"` (hardware HEVC): an HX source drawing a bright moving pattern → camera with an HX output → a receiver records the centre luma of every picture for 3 s; at 1 s enable the colour stage; every recorded luma is above the slate's and black's (> 40); pictures keep arriving after the switch (the tick path's `stats().encoder` is not `"none"` at the end).
  - `"OMT pass-through: a late receiver gets a picture"`: connect the receiver 2 s after the stream starts → a picture within 1 s.
  - `"OMT pass-through: audio flows"`: the source sends a tone; the receiver gets audio frames, and their timestamps are within one frame period of the nearest video timestamp.
- [ ] **Step 2: Run** → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** them, `--test-case=OmtInput*`, then the full verification command → green.
- [ ] **Step 5: Commit, merge into `main`** (`feat(io): OMT inputs pass coded frames and decode on demand`, `merge: lean phase 5a, OMT pass-through`).
