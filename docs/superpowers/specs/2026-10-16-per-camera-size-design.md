# BreadCamera — per-camera size — Design

Date: 2026-10-16. Project 2a of the approved three (2b: no master clock; 3: native Multiview). This
design removes the **show-wide resolution** only. The show **rate** stays until 2b.

## Intent

Each camera's frame is its **input's** size. There is no show-wide resolution; the Settings
"Resolution" field, `ShowSettings::width/height` and everything sized from them go away. Combined
with project 1 (one size per camera, no per-output size), every camera then produces exactly its
input's picture and every output sends it.

## What depends on the show size today

- `Camera::setFormat(w,h)` stores the size; `beginTick` scales the converted input to it
  (`fit`) even when the input already matches.
- One shared `Engine::slate_` texture is built at the show size and handed to every camera when it
  has no picture (`engine.cpp:40-53,179`).
- `command_router` validates `resolution`; `SettingsDialog` offers it; `/engine/resolution` reports
  it; calibration and the Multiview assume a uniform 16:9 frame.

## Goals

- The composited frame is the camera input's size (no scale to a show size).
- No show-wide resolution anywhere: no setting, no state key, no validation, no UI field.
- The slate still appears on a camera with no picture, at that camera's size.
- A show file with `settings.resolution` still loads (ignored).

## Non-goals

- The show-wide **rate** (project 2b).
- Mixed-size tiles in the Multiview beyond letterboxing into the existing 16:9 cell (my call, per
  the approved riders).
- Removing half-rate or the master tick.

## Decisions (and why)

| # | Decision | Why |
|---|---|---|
| D1 | **`ShowSettings` loses `width`/`height`**; `setFormat` and the `fit`-to-show-size in `beginTick` go. The frame is `toLinear(input)` at its own size. | The input size *is* the frame size; scaling to a global size was the only reason for it. |
| D2 | **The slate is built per camera, lazily, at the camera's current frame size.** Keep the loaded slate image (or the solid colour) once; `beginTick` fits it to the camera's size only while the camera has no picture. Before any frame is seen the camera uses a fallback size (last known, else 1280×720). | One shared slate texture cannot be the right size for cameras of different sizes. The fit only runs on the no-picture path, so it costs nothing when live. |
| D3 | **`command_router`: a legacy `resolution` is ignored, not validated.** `SettingsDialog` drops the field; `/engine/resolution` is removed. | Same migration rule as project 1's per-output size. |
| D4 | **The Multiview letterboxes a non-16:9 tile into its cell** (CSS `object-fit`/letterbox on the canvas). | Least UI churn; keeps the uniform grid. |
| D5 | **Calibration keeps its own fixed capture size (960×540); it does not capture at the camera's native size.** | `Engine::capture` does a blocking readback under the tick lock; at a 4K camera's native size that is 15 blocking 4K readbacks during a calibration, and an input under 960×540 would upscale first. The chart capture size is independent of the frame size. |
| D6 | **An output that bakes its size at open re-opens when the frame size changes.** `SrtOutput` opens its encoders once from the first frame; with per-camera sizes a mid-show renegotiation now moves the frame size, so it must re-init. | NDI/OMT set the size per frame, so they are fine; SRT would otherwise encode the old size forever — a latent bug 2a is the first to expose. |
| D7 | **`showToJson` stops emitting `settings.resolution`; `Show::kSchemaVersion` stays 1.** | An older build opening a newer show falls back to 1920×1080, which is harmless; bumping the version would make older builds refuse new shows. |
| D8 | **The preview still shares the one readback but caps its encode size at 1280×720** (downscaled on the encoder worker, off the tick). | Project 1 made the preview encode at the frame size, which today *is* the show size; per-camera sizes would make a 4K camera a 4K encode per visible tile. The cap bounds a 4K camera's preview cost. |
| D9 | **The status bar drops the resolution** (`/engine/resolution` is removed); the Multiview tiles show each camera's own picture. | There is no longer one show size to display. |

## Constraints

- MSVC C++20 `/W4 /permissive-`; the tick never blocks.
- One composited render per camera per tick unchanged.
- A show file written before this change still loads (legacy `resolution` ignored).
- UK English copy; conventional commits.

## Risks / open follow-ups

- **A camera input that changes size mid-show** (an SRT source that renegotiates) changes the
  camera's frame size with it. Outputs take the frame size per frame except SRT, which D6 makes
  re-open; the readback reset on a size change (project 1) covers the rest — verify.
- **The slate fit on the no-picture path** adds one GPU op per camera while a camera is down;
  immaterial, but note it.
- The Multiview's letterbox is a stop-gap; project 3's native Multiview owns the real layout.

## Left to the builder

- Where the fallback slate size lives and how the slate image is retained for per-camera fitting.
- The exact `beginTick` change and the readback reset on a size change.
- The UI letterbox rule.