# Product

<!-- impeccable:product-schema 1 -->

BreadCamera is part of the BM Solutions ecosystem. The shared product record is
`../PRODUCT.md` and the shared UX rules and glossary are
`../bm-design/docs/UX-CONTRACT.md`; this file adds only what is specific to
BreadCamera. Where the two disagree, this file wins for BreadCamera. The full
design is `docs/superpowers/specs/2026-10-08-breadcamera-design.md`.

## Platform

web

The UI is a React app hosted in WebView2 inside the Windows desktop shell
`BreadCamera.exe`. Desktop only: no mobile layout and no browser deployment.

## Users

- **Event production crews** on the Windows production PC next to OBS/vMix. The
  operator takes several cameras in, cleans them up (background removal, blur or
  replacement, colour calibration) and sends each one on to the switcher or a display.
- **How they use it (confirmed 2026-10-08):** heavy use during setup before the
  show (inputs, matte, colour calibration, outputs). During the show it sits on a side
  monitor or in the tray, and a problem must be obvious at a glance.

## Product Purpose

Multi-camera GPU processing for live production: AI background removal without a
green screen, background blur or replacement, one-time colour calibration of
each camera against a colour chart, and AI auto framing that keeps the speaker's
face in the shot with no camera operator, with more effects added later as add-ins.

Success means a camera's output never goes black because of BreadCamera, and one
RTX 4070-class PC runs 4 cameras at 1080p60 with every effect on, adding at most
50 ms of latency.

## Positioning

- **Like NVIDIA Broadcast, but for a whole camera rig.** Several professional
  inputs (SRT/BreadCall, NDI and NDI|HX, OMT and OMT HX, SDI capture cards, USB) processed together on one
  GPU, and sent on to production outputs (NDI, OMT, SRT, SDI key+fill, virtual
  camera, fullscreen).
- **Keyed output.** Outputs can carry fill plus alpha (NDI, OMT, SDI key+fill),
  so the switcher composites the person over its own background.
- **Chart calibration.** Each camera is calibrated once against a printed
  ColorChecker or a tablet showing the chart, then locked, so every camera matches.
- **The broadcast does not fail.** The engine runs headless and keeps every
  output going when the UI is closed. An input drop holds the last frame, then
  shows the slate. A failing stage is bypassed for that camera only. A failing
  output affects only itself. An engine crash restarts into the same show in
  under 3 s.

## Operating Context

- Runs on the production PC beside OBS/vMix, often with a BreadCall room feeding
  remote cameras in over SRT.
- **Object model (confirmed 2026-10-08):** no level 1 (standalone, no login).
  Level 2 is the **Show**, one show file per event. Level 3 is where a signal goes
  in or out: **Camera** (input) and **Output**.
- Closing the window asks to confirm, then quits and stops the engine; the window
  never hides to the tray. Stopping the outputs alone is a separate action behind
  a confirm dialog whose primary button reads "Stop all outputs".
- **UI (confirmed 2026-10-08):** a native Windows pro tool, English only. Title
  bar with menus, toolbar, Multiview with a docked Inspector for the selected
  camera, status bar; Settings is a dialog. Colour calibration runs inside the
  camera's Colour effect, not on its own page. Reference: `docs/design/mockups/`.

## Capabilities and Constraints

- Windows 11 required (the virtual camera uses `MFCreateVirtualCamera`).
- Project frame rate: 15, 23.976, 24, 25, 29.97, 30, 50, 59.94, 60,
  119.88 or 120. It sets the effect time budgets only; inputs and outputs keep
  their own rates.
- NVIDIA first (Maxine AI Green Screen, NVENC); an ONNX Runtime/DirectML matte
  works on other GPUs.
- No Kinde login and no billing in v1.
- AJA input and output are experimental, and the UI labels them "not verified on
  hardware".
- Previews: about 480×270 at 30 fps per Multiview tile, streamed only while the
  tile is visible; 720p for the selected camera while calibrating.
- **English only (confirmed 2026-10-08).** Names the operator types (cameras,
  outputs, shows) can be in any language and are Unicode end to end.
- Show format: each camera emits one frame per input frame and every output
  follows that rate; the Project frame rate sets the effect budgets. There is
  no show-wide resolution. An output may send half the frame rate.

## Brand Commitments

- Name: **BreadCamera**, in that camel-case spelling, as part of BM Solutions.
- The four signal states in English (Ready · Connecting · Live · Offline), the
  error/confirm/form rules in the UX contract apply unchanged. BreadCamera is
  English only, unlike the rest of the ecosystem (owner decision 2026-10-08).

## Evidence on Hand

- The design spec and the milestone plans in `docs/superpowers/`.
- **None yet:** no UI code, screenshots, benchmark results, hardware test
  results or customers. Performance figures in the spec are targets, not
  measurements, until the M5 soak run and the RTX validation report them.

## Product Principles

1. **Outputs outlive everything.** A failure in the UI, a stage or one output
   never blacks out another output, and the UI says plainly what failed.
2. **Problems are readable from across the room.** During the show the UI is
   glanced at, not studied; a camera's state and any error must be clear at
   Multiview tile size.
3. **Setup is where the work happens.** Configuration screens can be detailed;
   the live view stays calm until something is wrong.
4. **One ecosystem, one way of working.** Same shell, states, terms and dialogs
   as BreadStream and BreadCall.
5. **Say what was tested.** Hardware paths that were not tested on real hardware
   are labeled as such.
