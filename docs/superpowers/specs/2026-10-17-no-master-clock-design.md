# BreadCamera — no master clock (project 2b) — Design

> Superseded 2026-10-20 on D3 and D9: the Project frame rate stays, and it is the source of the
> effect budgets; each camera still emits at its input's rate. See
> `2026-10-20-project-rate-effect-budget-design.md`.

Date: 2026-10-17. Project 2b of the approved three (3: native Multiview). Project 2a removed the
show-wide resolution; 2b removes the show-wide **rate**. This is the engine-core rewrite.

## Intent

Each camera runs at its **input's** rate, not a configured show rate. The show's Frame-rate setting
and the master clock (spec §3.3) go away. A camera emits one frame per input frame.

## What the master clock does today

One thread waits on a timer at `settings_.rate` (`engine.cpp:138-272`) and, every tick: every camera
`beginTick` (pick → convert → sync to the tick rate by repeat/drop), the matte batch over all
cameras, every camera `endTick` (stages → readback → fan-out), publish, then the frame tap
(preview). Tick budgets (`StageGuard`, the matte's 60 %-tick), audio pacing
(`TickSchedule::audioFramesForTick`), half-rate outputs, overrun skipping, and capture all key off
that one clock.

## Goals

- No configured show rate; each camera emits at its input's rate.
- One composited frame per input frame (no repeat/drop).
- Everything the clock paced still works: matte/stage budgets, hold+slate timing, audio, half-rate
  outputs, overrun reporting, calibration capture, the preview.
- A camera's health, latency and drops still reported.

## Non-goals

- Per-camera rate *configuration* (each camera uses its input's rate).
- The native Multiview (project 3).
- Changing the outputs beyond half-rate (every 2nd frame).

## Decisions (and why)

| # | Decision | Why |
|---|---|---|
| D1 | **Arrival-driven, one GPU thread.** An input pushes a frame (as today, on the input thread); the engine thread wakes, and for each camera with a new frame runs stages + fan-out once. No timer, no fixed rate. | Removes the master clock without per-camera threads (which would need a locking redesign — the D3D11 context is single-threaded). "Newest frame wins" coalesces a burst, so a camera faster than the GPU does not queue. |
| D2 | **One output frame per input frame.** `FrameSync`'s repeat/drop against the tick rate goes; the phase (Live/Holding/Slate) is time-based on frame arrival (`holdMs` unchanged). | There is no tick rate to repeat/drop against; an input at 30 fps now sends 30. |
| D3 | **A camera's frame period is measured** (an EWMA of inter-frame arrival), used for the stage budgets and the matte budget in place of `TickSchedule::period()`. | The budgets were a fraction of the tick; the same fraction of the measured period keeps their meaning. |
| D4 | **Audio is pulled per emitted frame** from the input's audio (the input already paces it). | `audioFramesForTick` is meaningless without a tick. |
| D5 | **The matte processes the cameras that have a new frame in this pass**, in one batched call; a camera without a new frame keeps its last alpha. | Keeps the batch (spec §3.4) without a global tick. |
| D6 | **Half-rate is every 2nd emitted frame** (a per-output counter, not `index % 2`). | Preserves the option without a tick index. |
| D7 | **Overrun/latency/drops are per camera** (time the pass; overrun when it exceeds the measured period). | The engine-wide `/engine/overruns` stops meaning one number; move it per camera. |
| D8 | **Calibration poll is unchanged** (it asks the engine for a camera's frame and blocks); `Engine::capture` reads the camera's latest composited frame. | It already works off `tickRaw`/the frame, not the tick rate. |
| D9 | **The show-file `rate` is ignored; `SettingsDialog` drops the Frame-rate field**; `TickSchedule` and the show-rate plumbing are removed. | Same migration rule as 2a's resolution. |

## Constraints

- MSVC C++20 `/W4 /permissive-`; the tick (now the pass) never blocks; `start()`/`stop()` prompt.
- One composited render per camera per emitted frame.
- A legacy show file with `settings.rate` still loads (ignored); `kSchemaVersion` stays 1.
- UK English copy; conventional commits.

## Risks / open follow-ups

- **This is the largest change in the program** — it rewrites `engine.cpp`'s loop, `frame_sync`,
  the stage-guard budget source, and per-camera stats, and touches the matte service, capture and
  the preview. It wants its own plan with a task per area.
- **A camera faster than the GPU** now coalesces to the newest frame (dropped, not queued) — the
  same policy as today's input frame sync, moved to the camera.
- **Per-camera overruns** change the status-bar meaning; the UI needs the new shape.
- Removing `FrameSync`'s tick rate touches the phase machine (Live/Holding/Slate) — the hold+slate
  tests are the ones to protect.

## Left to the builder

- The wake mechanism (a condition variable signalled by an input push is the obvious one) and how
  the engine coalesces a camera's pending frames.
- Where the per-camera measured period lives and how it seeds before the second frame.
- The exact `StageGuard` API change (a period passed in, rather than a tick).

## Sizing

Five-to-six tasks: (1) the arrival-driven pass + coalescing, (2) drop the tick rate from
`FrameSync`/phase, (3) measured-period budgets + per-camera overruns, (4) audio per frame, (5)
half-rate counter + the UI/status/show changes, (6) docs + whole suite.