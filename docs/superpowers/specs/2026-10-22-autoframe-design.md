# BreadCamera — Auto frame (AI face framing) — Design

Date: 2026-10-22. Status: first increment implemented and built (2026-10-22).

> Implemented: `AutoFramer` (core, 6 tests), `FaceDetector` (YuNet via OpenCV `objdetect`, off-tick
> readback + worker), `Converter::crop` (`crop_linear.hlsl`), the `autoframe` stage schema, camera
> wiring before Colour/upscale, model fetch + CMake copy. Verified: full release build, AutoFramer tests
> 6/6, focused suite 48/48, engine starts with the detector loaded (no "unavailable" line). Detection
> quality on real faces and the image-stabilization half still need a camera and are the next increment.

## 1. Purpose

A per-camera AI stage that keeps the speaker's face in the shot. It is BreadCamera's
differentiator: vMix and OBS move a source or a PTZ head, but neither ships face-aware
auto-framing that works with any camera, in software, with no operator.

The operator picks the framing (Wide / Medium / Close) and where the face sits (Centre,
Left third, Right third). Everything else is automatic. A jittery tripod or a presenter
who shifts in their chair does not make the shot hunt: movement below a dead zone is
ignored and the rest glides.

## 2. Behaviour

- On: the face is found every third tick on a 320×180 copy; the framing is held between
  detections.
- The biggest confident face is the subject. A face smaller than 60 % confidence is not
  trusted.
- Zoom keeps the face at 26 % (Wide), 38 % (Medium) or 52 % (Close) of the picture height,
  and never goes closer than a quarter of the frame.
- Zoom has hysteresis: a face that changes size by less than 12 % does not pump the lens.
- The face centre is placed at the anchor (x: 0.5 / 0.33 / 0.67, y: 0.42). If the face is
  already in the dead zone (4 % of the crop), nothing moves.
- Movement is eased with a time constant (100–1500 ms, default 350) and capped at 1.2 crop
  widths per second, so a face crossing the frame is followed, not snapped to.
- Loss of the face holds the last framing for one second (a blink, a turn), then eases back
  to the full frame.
- The crop is clamped inside the picture at all times; output size and rate are unchanged.
- The stage runs before Colour, matte and Upscale, so every effect and output sees the
  framed picture.

## 3. How

- Face detection: YuNet 2023mar (OpenCV Zoo, MIT), through OpenCV `FaceDetectorYN`
  (`objdetect`). Local only; no frame leaves the machine.
- The model is `models/yunet.onnx` next to the engine, fetched by
  `scripts/fetch-sdks.ps1` with a pinned SHA-256.
- A 320×180 BGRA copy is read back off the tick (`ReadbackSlot`), as the ONNX matte does.
  One worker thread owns the detector; the tick never waits on it.
- `AutoFramer` is pure C++ (no GPU, no OpenCV): face box + elapsed time → crop rect. Its
  tests pin the dead zone, smoothing, loss/recovery, edge clamping and frame-rate
  independence.
- `Converter::crop` applies the rect with `crop_linear.hlsl` (one compute pass).

## 4. Non-goals (this increment)

- Image stabilization (translation-only optical stabilization) — Phase 2, using the same
  crop application with a motion-estimated offset.
- Body/pose following, face recognition, or per-person identity.
- Zooming past the input's real resolution.
- A crop box drawn on the preview; the stage card shows the effect instead.

## 5. Failure and cost

- No OpenCV `objdetect`, or no model: the stage reports `unavailable` with the fetch hint;
  every other stage is unaffected.
- Detection failure or a bad frame: the last framing holds; the stage never throws into the
  tick.
- Cost: one 320×180 readback every third tick, ~2–4 ms CPU on the detection thread, one
  compute pass per tick on the GPU.
- Licence: YuNet model and OpenCV are MIT/Apache-2.0 and already a BreadCamera dependency.
