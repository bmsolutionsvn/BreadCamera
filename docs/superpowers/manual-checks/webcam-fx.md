# Webcam effects acceptance — Clean up, Upscale and Colour Match (owner)

Approved 2026-10-18 — not tested yet.

The owner runs this on real hardware; a robot cannot judge a picture. The build PC here is an AMD
Radeon RX 6600 with no NVIDIA card, so checks 1, 2, 3 and 5 are **human gates** and check 4 is the
one this PC can answer. Every automated half is already pinned by the webcam-fx tests (named below).

## Checks

1. **Load (RTX 3060, 1080p60, 30 min).** Two cameras each with Clean up + Upscale (720p → 1080p) +
   Colour Match + Matte + Background, plus two cameras with Matte + Background. Tick overruns 0;
   latency ≤ 50 ms (flash-frame); read GPU ms per stage from the Inspector.
   *Result:* ______

2. **Match.** Two webcams under different light, one matched to the other, look the same side by
   side; turning a lamp on is followed within a few seconds with no visible jump.
   *Result:* ______

3. **Fallback on RTX.** Start BreadCamera with `nvngx_vsr.dll` moved out of the install folder:
   Upscale still delivers the target size (FSR 1), Clean up shows dimmed in Add effect.
   (Mid-show bypass keeping the size is pinned by Task 3's tests.)
   *Result:* ______

4. **AMD build PC.** Upscale runs on FSR 1; Clean up shows dimmed with `Needs an NVIDIA RTX card.`
   *Result:* **passed 2026-10-19, AMD Radeon RX 6600.** `getStageSchemas` over the control API
   (headless `breadcamera-engine`, token-authenticated WebSocket) returns `cleanup` with
   `"unavailable": "Needs an NVIDIA RTX card."` — the exact string — and `upscale` with no
   `unavailable`, so it is offered and falls back to FSR 1. The engine reports
   `gpu: "AMD Radeon RX 6600"`, `matteBackend: "onnx-directml"`, `overruns: 0`. The dimming and the
   reason text on the row are pinned by `ui/src/Effects.test.tsx`; the FSR path by Task 3's
   `upscaler_test.cpp` and `engine_upscale_test.cpp`. The on-screen confirmation is still the
   owner's.

5. **First RTX run questions** from `docs/superpowers/follow-ups.md` (VSR at 1A-, low-light noise,
   GPU ms).
   *Result:* ______

## Notes

- Accept the whole branch only after 1, 2, 3 and 5 are signed off on an RTX PC; check 4 alone does
  not clear the RTX paths, which are untested on hardware (see the Task 4 commit and
  `docs/superpowers/follow-ups.md`).
- Match's clamps (exposure ±1 EV, temperature ±60, tint ±60) are the binding spec's, not the
  plan's looser example; Task 6 kept the clamp and recorded the ruling in its commit body.