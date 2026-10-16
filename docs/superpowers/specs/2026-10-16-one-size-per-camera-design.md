# BreadCamera — one size per camera — Design

Date: 2026-10-16. Project 1 of 3 approved by the owner (2: remove the show-wide size/rate; 3: native
zero-copy Multiview). This design covers only project 1.

## Intent

Stop the engine producing — and reading back — more than one size per camera per tick. Every output,
and the preview, consumes the one composited frame. This removes the preview's private render and
readback (the source of the blur and a chunk of the tick overruns) and ends the per-output resize.

## What happens today

- `OutputConfig` carries an optional `width`/`height` (spec §4.3: "each output may send a smaller
  size"). `Camera::endTick` fans out **per distinct size**: one `fit` downscale (GPU) plus one
  async readback per size, shared by the CPU outputs of that size (`src/engine/camera.cpp:242-306`).
- The preview ignores that and does its **own** `toBgra8` + **blocking** `readback` at 512×288
  (or 1280×720), then H.264-encodes on the tick (`src/engine/preview_service.cpp:49`).
- The readback is the known tick hotspot. The preview adds a second one and is the only caller still
  blocking.

## Goals

- One frame size per camera per tick — the composited frame's size. No per-output resize, no `fit`.
- Exactly **one readback per camera per tick**, shared by every CPU output.
- The preview consumes that same image (no preview render, no preview-size readback) and encodes it
  at that size.
- The preview H.264 encode moves **off the tick**.

## Non-goals

- Removing the show-wide size/rate so the frame is the input's native size (project 2).
- A native zero-copy Multiview (project 3 — it supersedes the whole WebView preview path).
- Removing the preview encode (project 3).

## Decisions (and why)

| # | Decision | Why |
|---|---|---|
| D1 | **Drop `OutputConfig::width`/`height`.** Every output uses the frame size. | The per-size fan-out exists only to honour that option; without it there is one size, one `fit` (none), one readback. |
| D2 | **One readback slot per camera** (not per size). `endTick` hands the same `CpuImage` to every CPU output. | Outputs of a camera already share one composited frame; they should share one readback too. |
| D3 | **The frame tap carries the CPU image alongside the texture**, so the preview reuses it. | The preview needs pixels; the outputs already produce them. Copy, don't recompute. |
| D4 | **The preview encodes the shared image at the frame size, on a worker thread.** | Removes the preview's blocking readback and its tick-thread encode. |
| D5 | **If no output needs CPU, a visible preview forces the one readback.** `Engine` gains a `previewWanted(cameraId)` callback (wired from the `PreviewHub`) consulted before half 2, so `Camera::endTick` knows a preview wants the image; the readback runs whenever any CPU consumer — an output or a visible preview — needs it that tick. | The hub lives outside `Engine` and the tap fires after the tick, so the wanted-set must be pushed in before the readback decision; this closes the circular dependency. Keeps "one render + one readback per camera per tick" true for a camera that only has GPU outputs (e.g. Fullscreen). Changes the existing "no CPU output ⇒ zero readbacks" contract, which is tested. |
| D6 | **Half-rate stays per output for now.** | Rate is project 2's concern; only size is removed here. |
| D7 | **Keep the wire `size` (`small`/`large`) but the engine ignores it** — one encoder per camera at the frame size. | Minimal churn to the protocol and UI while project 3 replaces the WebView preview entirely; no viewer-visible feature is removed in the meantime. |
| D8 | **The preview accepts the shared image's ≤1-tick lag.** | The output readback is already async (one tick behind at most) and the preview encodes every 2nd tick (30 fps), so ~16 ms is immaterial. |
| D9 | **A minimal fallback: cap concurrent preview encoders; past the cap, skip that tile and set a state warning.** | A 1080p preview per visible tile can exhaust encoder sessions; today the first open failure disables *all* previews (`broken_`), too blunt for a many-camera show. |

## Constraints

- MSVC C++20 `/W4 /permissive-`; Windows only.
- `start()`/`stop()` never block for long; the tick is never blocked (spec §4.1).
- One composited render per camera per tick is unchanged (spec §3.2).
- The `readbackSlotCount()` test contract changes (D5); update it.
- Outputs a user already configured with a smaller size must keep working (the size is ignored, with
  a visible note or a migration in the show file — builder's call).

## Risks / open follow-ups

- **CPU-output bandwidth goes up.** An SRT/NDI output that used to send 720p now sends the full show
  size; the encoder and the network carry more. Product decision (owner's), stated here.
- **Preview at 1080p** is more encoder work per visible tile than 512×288; consumer NVENC session
  limits may bite with many cameras. Handled by D9 (cap + warn), not left open.
- **The preview-sharing work (D3/D4) is throwaway once project 3 lands** — it buys sharpness and
  fewer overruns in the meantime.

## Left to the builder

- The exact tap signature and where the shared `CpuImage` is produced/owned.
- The show-file migration for an output carrying `width`/`height`.
- The worker-thread shape for the preview encoder and its queue.
