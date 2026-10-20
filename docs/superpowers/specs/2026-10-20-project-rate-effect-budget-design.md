# BreadCamera — Project frame rate is an effect budget; inputs and outputs keep their own rate — Design

Date: 2026-10-20. Owner decision. Supersedes two points of
`2026-10-17-no-master-clock-design.md`: the rate setting **stays** (D9 below) and the effect
budgets come from the **Project rate**, not a measured period (D3 below).

## Intent

The Project frame rate is a **budget reference for effects**, not the show clock. It answers one
question: how long may a stage take on one frame? Changing it must not re-time an input and must
not re-time an output.

- Inputs run at their own rate. A 60 fps camera stays 60; a 29.97 camera stays 29.97.
- Each camera emits **one output frame per input frame**.
- Each output sends at the camera's rate. NDI/OMT/SRT timestamps and SDI hardware modes use that
  rate, not the Project rate.
- The Project rate only feeds the stage/effect budgets.

Allowed Project rates (exact rational values):

| Label | Stored rate |
|---|---|
| 15 | `15/1` |
| 23.976 | `24000/1001` |
| 24 | `24/1` |
| 25 | `25/1` |
| 29.97 | `30000/1001` |
| 30 | `30/1` |
| 50 | `50/1` |
| 59.94 | `60000/1001` |
| 60 | `60/1` |
| 119.88 | `120000/1001` |
| 120 | `120/1` |

Nothing else is accepted. HFR is limited to 119.88 and 120.

## Why now

Today the Project rate is the master clock, so it does exactly what the owner does not want:

- `engine.cpp:144-286` ticks at `settings_.rate`; every camera is picked once per tick.
- `frame_sync.h:25-26` repeats slower inputs and drops faster ones — inputs are resampled to the
  Project rate.
- `camera.cpp:588` submits one frame per output per tick — outputs run at the Project rate.
- The same period sets the stage budgets (`camera.cpp:204`, `camera.cpp:216`,
  `camera.cpp:538`, `engine.cpp:239`).

Setting 15 fps today makes a 60 fps input send 15 frames; setting 120 makes it repeat frames.
That is the behaviour this design removes.

## Goals

- Project rate allowlist above, exact rational values, in the engine and in Settings.
- Inputs are not re-timed. No repeat/drop against the Project rate while a camera is live.
- One composited output frame per input frame, at the input's rate.
- Outputs receive the camera's rate: NDI/OMT/SRT metadata and timestamps, SDI mode, virtual camera.
- Effect/stage budgets derive from `settings_.rate.period()`:
  - Background, Upscale, Clean up: `period() / 4` (as today).
  - Matte: `period() * 6 / 10` (as today).
  - Colour keeps its 4 ms CPU budget; GPU timing unchanged.
- Keep hold/slate, audio, half rate, matte batching, preview, calibration capture, Match, stats.
- Remove the Add-output SDI guard planned earlier: SDI follows the camera's input rate instead.

## Non-goals

- Per-camera rate configuration.
- Arbitrary rates outside the 11.
- Frame-rate conversion (no 24 → 60, no 29.97 → 30).
- Automatic SDI reconfiguration when a live source changes rate mid-show (see D10).
- The native Multiview.

## Anti-goals

- A system that still repeats/drops to the Project rate.
- An input or output rate silently rounded to the Project rate.
- Guessing 29.97 as 30.0 because the measured period is noisy.

## Decisions

| # | Decision | Why |
|---|---|---|
| D1 | **Arrival-driven, one GPU thread.** `Camera::onVideo` stores the newest frame and wakes the engine through a condition variable. The engine processes every camera with a new frame, then waits again. No periodic show tick. | Removes the Project rate from pacing without per-camera GPU threads. "Newest wins" coalesces a burst. |
| D2 | **Live = one output frame per input frame.** While a camera is live, no repeats and no drops against a clock. A frame that is replaced before processing counts as a drop. | Exactly the owner's rule: output follows input. |
| D3 | **Effect budgets use the Project rate period.** `StageGuard` budgets and the matte budget take `settings_.rate.period()` fractions. The Project rate does nothing else. | The setting is the budget knob the owner asked for. |
| D4 | **Rate allowlist.** `parseShowRate` accepts only the 11 rational values above. Settings offers exactly those. Show schema stays 1. | Explicit, testable, and matches the owner's list. |
| D5 | **Camera nominal rate.** `VideoFrameIn` gains an optional `Rate` filled by the input when it knows it (NDI frame-rate metadata, SDI mode, SRT/FFmpeg stream rate, UVC/DShow negotiated rate). When absent, the camera measures inter-arrival (EWMA) and derives a stable rational. `OutFrame.rate` is this nominal rate. **The rate is latched per input session** once a declared value arrives, or once the measured value is stable; only an explicit input renegotiation changes it. Consumers may assume a constant rate between renegotiations. | Outputs need a rate to stamp frames, and 29.97 must not become 30. Latching stops a noisy measurement from flickering NDI metadata or tripping the SDI rate-change error. |
| D6 | **Audio per emitted frame.** The camera keeps its own emitted-frame index and pulls `TickSchedule(nominalRate).audioFramesForTick(index, 48000)`. A held/slate repeat pulls the same count. | Keeps 48 kHz pacing exact without a global tick. |
| D7 | **Half rate is every 2nd emitted frame**, a per-output counter, carrying the odd frame's audio to the even frame. | Preserves the existing option without a tick index. |
| D8 | **Hold/slate still works.** While live, nothing is emitted without a new frame. After `stallAfter` (250 ms) the camera repeats the held frame at its nominal rate; after `stallAfter + holdMs` it repeats the slate at that rate. The engine waits until the earliest repeat deadline when no camera has a new frame. | The live path follows input exactly, and signal loss still holds the picture and then shows the slate. |
| D9 | **Keep the Project rate setting.** It is stored in the show file and shown in Settings. It is **not** used to pace I/O. | Owner decision, overriding D9 of the 2026-10-17 design. |
| D10 | **SDI follows the camera rate at open.** DeckLink/AJA pick their hardware mode from the camera's nominal rate. Supported 1080p modes are whatever the installed SDK exposes (23.976, 24, 25, 29.97, 30, 50, 59.94, 60, 120 on DeckLink 16; 119.88 only if the SDK has the mode). If the rate changes after the output opened, the output stops with a clear "SDI mode changed; re-add the output" error instead of silently converting. 1080p only, as today. | Output follows input, without pretending the card can switch modes per frame. Rate changes mid-show are rare and visible. |
| D11 | **Stats and UI.** `/engine/rate` remains the Project rate. Status bar shows it with the label "Project rate" and a hint that it sets effect budgets. Per-camera fps remains the emitted rate. `/engine/overruns` counts passes whose processing exceeds the Project period; the status bar calls them "budget overruns". | Keeps the operator's mental model honest. |
| D12 | **Matte batching stays.** Each engine pass batches the cameras that have a new frame; a camera without one keeps its last alpha. | Preserves spec §3.4 and one inference call per pass. |
| D13 | **A drop advances time.** A superseded pending frame increments the drop count and advances the camera's emitted-frame index by the dropped count, so `OutFrame.tick`, OMT timestamps, the audio pull, Match's every-6th cadence and capture's `every` all track wall clock under sustained coalescing. | Emitting 30 of a 60 fps stream must not make timestamps or 48 kHz audio run at half speed. |
| D14 | **SRT and the virtual camera follow the rate.** SRT rebuilds its H.264 encoder on a rate change (the same path it already uses for a size change). The virtual camera passes every emitted frame; the hard `frames_ % 2` decimation goes, because at a 30 fps camera it would halve the webcam to 15 fps. The media source still pulls at its own advertised rate. NDI/OMT carry per-frame metadata; fullscreen ignores it. | "Output follows input" means every output type, not only the convenient ones. |
| D15 | **Preview cadence is wall-clock capped.** The preview encodes at most once per camera per 33 ms (≈30 fps), independent of the Project rate. The frame tap carries the camera's emitted-frame index. | A 24 fps camera should not preview at 12 fps just because "every 2nd pass" became meaningful. |
| D16 | **No output before the first frame.** A camera that has never received a frame emits nothing: outputs stay connected but send no picture, the phase is `nosignal`, and the local preview may still show the slate. The first input frame latches the nominal rate and starts emission. | Owner decision: the Project rate must never pace an output, including startup. An operator sees "connecting", not a slate, until the input delivers. |

## Data and API changes

- `io/input.h`: `VideoFrameIn` gains `std::optional<Rate> rate;`. Each input fills it when the
  protocol reports one. Empty is valid.
- `Camera`:
  - `bool hasFreshFrame() const` — a new frame is pending.
  - `bool emit(EngineClock&, ...)` / reworked `beginTick`/`endTick` that process **one emitted
    frame**, not one tick. The exact split stays a builder choice.
  - `Rate nominalRate() const` — declared or measured, falling back to the Project rate.
  - `void setWake(std::function<void()>)` — called by the engine to signal the loop.
  - `holdDeadline()` — when the camera next needs a held/slate repeat.
- `Engine`:
  - `run()` waits on a condition variable (with the earliest hold deadline as a timeout) instead
    of a waitable timer.
  - `apply()` signals the loop.
  - `settings.rate` is passed to cameras as the **budget** period, not a schedule.
  - `TickSchedule` is no longer the loop's clock; it remains a helper for audio and period math.
- `AdapterFactory::makeOutput` gains the camera's nominal `Rate` (or a per-call `setShowRate` is
  removed in favour of a parameter). Test fakes update.
- DeckLink/AJA output constructors already take a `Rate`; the mode mapping extends to every
  supported 1080p mode.
- The virtual camera and fullscreen output keep their frame delivery; their timestamps follow
  `OutFrame.rate`.

## Migration

- `schemaVersion` stays 1.
- Shows with `50/1`, `60000/1001` or `60/1` load unchanged.
- A show with any other rate is rejected with the new allowlist message.
- No stored `resolution` behaviour changes.

## Tests

- `rate_test`: all 11 accepted; 14.985/1, 23/1, 48/1, 72/1, 90/1, 95.904, 96/1, 100/1, 121/1,
  144/1, `60`, `60/0`, `abc/1` and empty rejected.
- `show_test` / `command_router_test`: `30/1` and `120/1` valid; `100/1` invalid.
- Engine arrival tests with fake inputs:
  - Cameras at 30 and 60 emit 30 and 60 frames over the same wall time; a Project rate of 15
    does not change either count.
  - Startup: a camera with no frame emits no output frame and stays `nosignal`; the first frame
    starts emission (D16).
  - Project rate 15 vs 60 changes the stage budget handed to `StageGuard` (unit-level).
  - No repeat or drop while live; a superseded pending frame counts as a drop.
  - Hold: after 250 ms the last frame repeats at the nominal rate; after `holdMs` the slate does.
  - Audio: `audioFramesForTick` sums to 48 000 per camera over one second at 59.94.
  - Half rate: one frame per two emitted frames, with the carried audio.
  - Capture: `every` counts emitted frames.
  - Match: the every-6th-frame cadence follows emitted frames.
- SDI mode mapping: unit test per allowed rate for the modes the SDK exposes; unsupported rates
  produce the clear error.
- UI: Settings lists exactly 11 options with the right values; the hint says budgets only;
  status bar shows "Project rate"; Add-output has no Project-rate gate.
- Existing e2e tests that assume a tick count are updated to frame counts.

## Risks

- This is the largest change in the program: the engine loop, camera pacing, hold/slate, audio,
  outputs and tests all move.
- Coalescing can drop a frame under a GPU stall. It is reported as a drop, never hidden.
- A measured fallback rate can jitter. Prefer declared rates and snap the measured value to a
  stable rational (never to the Project rate).
- SDI hardware is untested on this build machine (AMD RX 6600, no SDI card). Mode mapping is
  compile-checked and unit-tested; the card path stays "untested on hardware" like today.
- 119.88 may not be a 1080p mode in the installed DeckLink SDK; the output must error clearly.

## Left to the builder

- The wake/coalescing mechanism, the housekeeping wake for status, and the exact `Camera` API split.
- How the fallback nominal rate is measured, smoothed, snapped and latched.
- How SDI outputs wait for the first nominal rate before opening.
- The precise overrun accounting per pass.
- How the local preview shows the slate while the output side stays silent (D16).

## Sizing

Seven tasks: (1) rate allowlist + Settings, (2) frame-rate metadata + estimator + camera nominal
rate, (3) real inputs declare their rate, (4) arrival-driven engine + no output before the first
frame, (5) hold/slate + audio + drops + half rate, (6) outputs follow the camera rate (factory,
SDI, SRT, vcam, preview), (7) stats/status, docs and the whole suite.
