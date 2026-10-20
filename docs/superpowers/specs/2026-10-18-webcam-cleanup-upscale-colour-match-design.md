# BreadCamera — webcam clean-up, upscale and colour match — Design

Date: 2026-10-18. Status: approved by the owner (2026-10-18).
Builds the top of the approved add-in roadmap (spec §10.1 items 1–2) and lifts "continuous colour
matching" out of §10's out-of-scope list for remote cameras. Two parts, built in order: **A** (Clean up
and Upscale), then **B** (colour Match).

## Intent

Remote guests join on home webcams over BreadCall/SRT. On the program output they look **blurry**
and their **colour does not match** the other cameras. The owner's reference case is an esports
broadcast with two remote casters side by side, both with replaced backgrounds: one webcam is warm,
sharp and contrasty, the other soft, flat, washed-out and cool. The viewer compares the two faces
directly, so they must match **each other**; there is no studio camera or colour chart to match to.

BreadCamera has not been used on a real show yet. Real shows run on an **NVIDIA RTX 30-series or newer**
card (owner, 2026-10-18); the build machine is an AMD RX 6600 (spec §7.2).

## Goals

- **Clean up** (new add-in): removes compression blockiness and webcam noise from a camera.
- **Upscale** (new add-in): raises a camera to a chosen target resolution.
- **Match** (new mode of the existing Colour effect): continuously matches a camera's colour to another
  camera, or balances it on its own when there is no reference, and follows slowly when the guest's
  light changes.
- None of the three can black out a camera: an effect that fails or overruns is bypassed for that
  camera only (spec §3.6, existing `StageGuard`).

## Non-goals

- A native rewrite of the whole UI. The owner asked for native "if WebView is too laggy"; WebView cannot
  slow the outputs (the engine is its own process), and the laggy part — previews — is already replaced
  by the approved project 3 (native zero-copy Multiview). A full native UI is a separate project if
  project 3 is not enough.
- Targets above 1080p (spec §10 keeps 4K out of scope).
- Audio noise suppression, relighting, eye contact, captions (remain §10.1 roadmap items).
- Strong colour grading or looks: Match corrects, it does not grade.
- Changing the chart calibration (§5.3). It stays for studio/SDI cameras.

## Pipeline order

```
Input → Decode → To RGBA → Clean up → Upscale → Colour → Matte → Background → Outputs
```

Clean up runs before Upscale so the upscaler does not sharpen blockiness. Colour stays before Matte
(spec §3.2). Matte then runs at the upscaled size: better edges, more cost.

## Decisions

Marked **owner** where the owner decided; otherwise the planner's call.

| # | Decision | Why |
|---|---|---|
| D1 | **Clean up and Upscale are two separate add-in effects** (spec §5.4), each with its own schema, guard and bypass. | The operator may want only one; each bypasses independently. |
| D2 | **Upscale has a target picker: 720p or 1080p** (owner: "when select Upscale, we can select resolutions target"; planner: list stops at 1080p, owner did not ask for more). The target is the **height**; the width follows the input's aspect (a 4:3 webcam becomes 1440×1080, never stretched). The camera's frame becomes that size and every output sends it. | Per-camera size (2026-10-16) removed the show size the roadmap's "upscale to the show format" relied on. |
| D3 | **A target not larger than the input does nothing**, and the effect shows "Input is already 1080p" (or 720p). | Upscale never shrinks a picture. |
| D3a | **Upscale's size is stable on air.** When Upscale is bypassed (failure or overrun), it falls back to the FSR 1 path (D6) **at the same target size**, not to a pass-through at the input size. While Upscale is on, the slate is fitted to the target size too. Only the operator changing the target (or removing the effect) changes the camera's frame size. | A size change re-opens SRT outputs and changes the NDI/OMT format the switcher sees (per-camera-size D6); a guard retry every 5 s would flip the size on air. Narrows §3.6's "bypassed" for this one effect. |
| D4 | **On RTX, Clean up and Upscale use the NVIDIA RTX Video SDK** (v1.1: artifact reduction and super resolution), on D3D11 textures — no CUDA interop, no CPU copy. **Maxine Webcam Denoising is not in this plan**; it is a follow-up, picked up only if the first RTX run on real webcam footage shows low-light noise the SDK leaves behind. | RTX Video SDK takes D3D11 natively, which matches the engine (§3.1), supports RTX 20+, and is built for real-time playback. Denoise can only be judged on an RTX card, and the build machine has none. |
| D5 | **Licence gate: passed (2026-10-18).** The RTX Video SDK v1.1.0 ships under the *NVIDIA RTX SDKs License* (v. 2024-02-23, with the RTX supplement v. 2023-12-20). It allows commercial use and shipping the SDK's runtime in object code inside BreadCamera (§1c, §2a). The obligations are in "RTX Video SDK licence obligations" below. | Same rule as §7.1. Read from `NVIDIA_RTX_Video_SDK_License.pdf` in the SDK download (behind an NVIDIA Developer login). |
| D6 | **On non-RTX GPUs, Upscale uses AMD FidelityFX Super Resolution 1** (EASU + RCAS, MIT, plain HLSL compute shaders). | Vendor-neutral, cheap, testable on the AMD build machine and in CI on WARP. No AMF dependency. |
| D7 | **Clean up is RTX-only.** On other GPUs it stays listed in Add effect but **dimmed and not selectable**, with "Needs an NVIDIA RTX card" on its row (owner). | A good AI clean-up on the RX 6600 via ONNX/DirectML needs a CPU round-trip today (`OnnxMatte`) and is unproven at 60 fps. |
| D8 | **The dimmed, not-selectable rule applies to every effect the operator cannot add**: an effect the GPU cannot run, and a missing dependency (§5.4's "explains instead of hiding"). The reason stays readable (not lowered below the UX contract's contrast). | One rule for "can't add this", consistent with §5.4 (owner asked for dimmed + not selectable). |
| D9 | **Match is a second mode of the Colour effect**: Calibrate (chart, §5.3, unchanged) or **Match**. Match has a **Match to** setting: another camera in the show, or **Auto balance** (none). | No new effect for the operator to learn; the chart path stays for studio cameras. |
| D10 | **Match measures skin, not the whole picture**: skin-tone pixels inside the camera's matte (or the whole frame when the camera has no Matte), on both the camera and its reference. | Clothes differ (white shirt vs dark suit in the owner's example) and the background is replaced, so whole-frame statistics would match the wrong thing. |
| D11 | **Match drives the existing grade** — the hidden Colour fields exposure, temperature, tint, lift, gamma, gain, saturation, applied by `ColorGrader` — rather than a new shader. Corrections are **clamped** to a gentle range. | Reuses the grader that already runs; a wrong match looks slightly off, never green or purple. |
| D12 | **Match follows slowly, continuously** (owner: "follow slowly, keep it always good"): the correction eases towards its new value over a few seconds and never jumps. When no skin is found (guest left the frame, looked away) or the reference camera is down, it **holds** the last correction. | Webcams change their own exposure and white balance mid-show; a locked match drifts. Easing avoids visible pumping on air. |
| D13 | **Auto balance** (no reference) aims for neutral white balance and a natural skin brightness. | The owner's case may have no good camera to match to. |
| D14 | **Per-stage GPU timing is new work.** Each stage (the new ones and the existing Colour, Matte, Background) is timed with D3D11 timestamp queries, read back a tick or more late, and that **GPU** time feeds its `StageGuard`. The per-stage times go out in the 1 Hz stats and show in the Inspector next to each effect. | Today the guard times the CPU call, which returns once GPU work is queued, so a GPU-bound effect would never trip its guard; and the only GPU timing is engine-wide `/engine/matteMs`. Without this, §3.6's per-stage bypass does not work for GPU effects. |
| D15 | **When ticks overrun and no single stage is over its share, the engine reports, it does not shed.** The status bar shows the overrun warning (as today) and names the most expensive stage per camera; the operator removes effects. | Silently dropping an effect on air changes the picture without the operator choosing it. Per-stage bypass (D14) still covers the single-culprit case. |
| D16 | **Match's easing correction lives in engine memory, not in the show's stage params.** Only the operator's settings (mode, Match to) are saved. | Writing stage params runs `reconfigure`, which resets every stage guard and autosaves on every easing step. |

## Constraints

- MSVC C++20 `/W4 /permissive-`; the tick never blocks; the main path never copies frames to the CPU
  (spec §3.1). Match's statistics come back as a few numbers via an async readback, never a frame.
- RTX code is compiled when its SDK is present and listed as "untested on hardware" in
  `docs/superpowers/follow-ups.md` until run on an RTX card (§7.2). A missing SDK never fails the build.
- A show file written before this change still loads; a camera without the new stages behaves exactly
  as before.
- UI copy in English, UK spelling.

## RTX Video SDK licence obligations

From the *NVIDIA RTX SDKs License* and its supplement, read 2026-10-18. BreadCamera's repo is public on
GitHub, which makes §4b and §4e the ones that bite.

- **No SDK file in the public repo.** No headers (`nvsdk_ngx*.h`), libs, DLLs or NVIDIA sample code are
  committed (§4b: no copying or distributing except as granted; §4e: the SDK must not become subject to an
  open-source licence). The developer downloads the SDK (login needed, so the fetch script cannot) and
  places it under `third_party/`, which is not tracked. `BC_WITH_RTXVIDEO` finds it or switches itself off
  (§7.2); CI builds without it.
- **Our own wrapper.** BreadCamera's RTX Video code is written from the headers and the programming guide,
  not copied from `samples/` (copying would also require the "This software contains source code provided
  by NVIDIA Corporation." notice, §2b).
- **The installer may ship the runtime DLLs** (§1c, §2a), under terms at least as protective as NVIDIA's
  (§2c): `installer/THIRD_PARTY_NOTICES.md` gains an "NVIDIA RTX Video SDK runtime" section saying those
  files are NVIDIA's, under NVIDIA's licence, with no reverse engineering and no redistribution apart from
  BreadCamera.
- **Attribution (supplement §7.1c):** credit the RTX Video SDK on a credits screen, or prominently in the
  end-user documentation when there is none (BreadCamera has none today: the About tab of Settings is
  the place). Using NVIDIA's marks (such as the "NVIDIA RTX" logo) needs NVIDIA's written approval, with a
  sample sent at least 2 weeks before use (§7.2c). Until approved, the credit is plain text, no logo.
  **Owner action before the first public release.**
- **Notify NVIDIA before commercial release** at developer.nvidia.com/sw-notification (supplement §4 names
  NGX; the RTX Video SDK is built on NGX, so notify to be safe). **Owner action.**
- **NVIDIA GPUs only, not life-critical use** (supplement §1, §4f): already true of the RTX path.

## Acceptance (approved by the owner 2026-10-18; **not tested yet** — no RTX machine has run it)

- **Load:** on an RTX 3060, at 1080p60 with no tick overruns over 30 minutes: **2 remote cameras** with
  Clean up + Upscale (720p → 1080p) + Colour Match + Matte + Background, plus **2 cameras** with Matte +
  Background. Latency stays within §1.1's 50 ms.
- **Upscale on AMD/WARP:** golden-image tests for the FSR 1 path; Add effect shows Clean up dimmed.
- **Match:** two webcams under different light, one matched to the other, look the same to the owner
  side by side; turning a lamp on mid-run is followed within a few seconds with no visible jump.
- **Bypass:** forcing Upscale to fail keeps the frame at the target size; no output re-opens.

## Risks

- **Budget on a 3060.** §1.1's 4× 1080p60 target assumes a 4070. Four cameras each running Clean up,
  Upscale and Matte at 1080p may not fit. Expected use is one or two remote cameras per show (planner's
  assumption; the owner did not give a number). Per-stage GPU timing (D14) bypasses an overrunning effect.
- **Matte cost rises after Upscale** (it now sees 1080p instead of 540p/720p).
- **Match on poor skin detection** (very warm room light, dark skin under bad light, no face in frame).
  Clamping (D11) and hold (D12) limit the damage.
- **Match feedback**: Colour runs before Matte, so Match uses the previous tick's alpha. One tick of lag is
  immaterial at the easing speed.
- **NVIDIA can end the SDK licence** with notice if it stops offering the SDK (licence §12). The fallback
  is Maxine VFX Super Resolution and Artifact Reduction, with a resample after Maxine's fixed scale factors.

## Build order

1. **Part A:** per-stage GPU timing (D14) → Upscale with the FSR 1 path and the stable-size bypass (D3a,
   testable here) → Add effect's dimmed rule (D7, D8) → Upscale and Clean up on the RTX Video SDK →
   the notices and About credit (licence obligations above).
2. **Part B:** skin statistics → Match to camera → Auto balance → the Colour effect's mode switch in the
   Inspector.

## Left to the builder

- How Clean up and Upscale slot into `Camera` (today Colour, Matte and Background are wired by hand in
  `src/engine/camera.cpp`, each with its own `StageGuard`; §5.4's registry is not generic yet). A small
  generalisation is fine; a node graph is not.
- Whether Clean up + Upscale fuse into one RTX Video SDK call when both are on.
- The skin-tone test, the statistic (e.g. mean and spread in a perceptual space), the easing time
  (a few seconds) and the clamp ranges, tuned on real webcam footage.
- How Match's skin statistics reach the CPU (a tiny async readback is expected).
- The Inspector layout for the Colour effect's two modes and the Match to picker.
