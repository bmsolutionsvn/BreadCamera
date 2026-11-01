# Webcam effects acceptance — Enhance and Colour Match (owner)

Approved 2026-10-18, amended 2026-10-27 (Enhance replaces Clean up and Upscale) — not tested yet.

The owner runs this on real hardware; a robot cannot judge a picture. The build PC here is an AMD
Radeon RX 6600 with no NVIDIA card, so checks 1, 2, 3 and 6 are **human gates** and check 4 is the
one this PC can answer. Every automated half is already pinned by the webcam-fx tests (named below).

## Checks

1. **Load (RTX 3060, 1080p60, 30 min).** Two cameras each with Enhance (1080p) + Colour Match +
   Matte + Background, plus two cameras with Matte + Background. Tick overruns 0; latency ≤ 50 ms
   (flash-frame); read GPU ms per stage from the Inspector. Enhance must show one VSR pass, never
   two (its GPU ms stays about what Clean up + Upscale cost before).
   *Result:* ______

2. **Match.** Two webcams under different light, one matched to the other, look the same side by
   side; turning a lamp on is followed within a few seconds with no visible jump.
   *Result:* ______

3. **Fallback on RTX.** Start BreadCamera with `nvngx_vsr.dll` moved out of the install folder:
   a resize still delivers the target size (FSR 1) and Enhance is still offered in Add effect (E9);
   at Same size the picture passes through and the note becomes
   `Same size needs an NVIDIA RTX card` (nothing runs).
   (Mid-show bypass keeping the size is pinned by the Enhance tests.)
   *Result:* ______

4. **AMD build PC.** Enhance at Same size on a 1080p webcam shows the note
   `Same size needs an NVIDIA RTX card` and the picture passes through; at 1080p a camera already at
   1080p shows `Input is already 1080p`; a 4K camera at Same size shows
   `Same size needs an NVIDIA RTX card` (the no-RTX text wins, E7). Nothing is dimmed in Add effect.
   *Result:* ______

5. **Quality levels with the dev VSR DLL (RTX PC only).** Install the dev `nvngx_vsr.dll` from
   `dist/vsr-test/`: Same size + **Ultra** on a 1080p webcam shows a VSR watermark marked
   `VSR QualityLevel 4`; **Fast** shows `VSR QualityLevel 1`. A 720p webcam at 1080p shows **one**
   watermark, not two.
   *Result:* ______

6. **First RTX run questions** from `docs/superpowers/follow-ups.md` (VSR at 1×, low-light noise,
   GPU ms, and the RGBA input fix).
   *Result:* ______

## Notes

- Accept the whole branch only after 1, 2, 3, 5 and 6 are signed off on an RTX PC; checks 4 alone do
  not clear the RTX paths, which are untested on hardware (see `docs/superpowers/follow-ups.md`).
- Match's clamps (exposure ±1 EV, temperature ±60, tint ±60) are the binding spec's, not the
  plan's looser example; Task 6 kept the clamp and recorded the ruling in its commit body.