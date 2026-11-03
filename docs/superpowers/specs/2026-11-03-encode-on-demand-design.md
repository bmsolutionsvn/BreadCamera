# Encode only while someone is pulling (OMT / OMT-HX) — Design

Date: 2026-11-03. Status: approved by the owner in chat (2026-11-03); corrected against libomt while planning (§3.1, §6). Project 1 of the BreadMedia
data-path backlog (`docs/superpowers/2026-11-03-data-path-backlog.md`, item 2).

## 1. Why

An OMT output that no receiver is pulling still does all its work every frame today:
- OMT-HX's `Sender::Impl::sendPicture` encodes every HX frame. `omt_send_connections` is read only for stats.
- libomt's `OMTSend.SendVideo` VMX-encodes every frame it is handed.
- BreadCamera reads back (or, after the OMT copy-paths work, converts) every frame for an OMT output, whether or
  not anyone is pulling.

The owner sees this in shows: OMT outputs nobody pulls, and OMT-Bridge routes that move frames for nothing. NDI
avoids it: the sending app asks `NDIlib_send_get_no_connections` "to avoid even rendering when nothing is
connected", and NDI|HX tells the app when a keyframe is needed on a new connection. Reference notes are in the
backlog, item 2.

## 2. Goals, non-goals, anti-goals

Goals
- An OMT or OMT-HX output with no receiver costs almost nothing. No pixels are read back, converted, packed or
  encoded, and nothing is handed to libomt. It stays discoverable, keeps its name, and tally and metadata still
  work.
- A receiver that connects sees a picture within about two frames while the encoder is warm.
- Every BreadMedia app that sends through OMT-HX benefits by asking one question: `hasReceivers()`.

Non-goals
- Changing libomt. Plain OMT stays the standard wire format, and with nobody connected nothing is on the wire.
- Caching the last frame for a new receiver, as NDI does. Every BreadMedia sender sends every tick. Revisit for a
  source that sends rarely.
- OMT-Bridge pass-through (project 2).

Anti-goals
- A receiver that connects and sees nothing for seconds, or a broken first HX frame (a P-frame with no keyframe
  before it).
- Discovery, tally or metadata going quiet while idle.
- An encoder opened and closed on every short gap. That churns GPU encoder sessions, which consumer GPUs limit.

## 3. Design

### 3.1 OMT-HX (`src/link.cpp`, `include/omthx/omthx.h`, C API)

- `bool Sender::hasReceivers()` and `omthx_c_sender_has_receivers`: true while `omt_send_connections` is above
  zero. The count is cached and refreshed at most every 100 ms, so asking every frame costs nothing. Safe from any
  thread.
  - **Any connection counts** (checked in libomt, 2026-11-03). A receiver opens one connection for video and
    metadata and a second for audio. `omt_send_previewconnections` counts a preview receiver's video connection,
    but its audio connection looks like any other, so a full receiver plus two probes (4 connections, 2 preview)
    cannot be told from two preview receivers with audio (also 4 and 2). Counting every connection is the safe
    side: a probe or a preview receiver also wakes the full stream, as today. Telling them apart needs a
    video-connection count in libomt (the owner's libomtnet, a few lines), later.
- Idle gate: every video send (`sendVideo` ×3 overloads, `sendVideoNv12`, `sendVideoUyvy`, `sendVideoD3D11`) and
  `sendCoded` return at once, after their argument checks, while nobody is connected. `sendAudio` too. A new
  `SenderStats::framesIdle` counts skipped video frames. It is not added to the C counters: that struct is filled
  by the DLL, and a caller built against the old header would be overrun.
- On idle → active: the next HX frame is a keyframe. It bypasses the 200 ms keyframe limit for this one case. For
  `sendCoded`, `keyframeRequested()` becomes true, so a relay asks its own source for one (existing mechanism).
  Plain OMT frames are all keyframes.
- Encoder warm time: the HX encoder (and the preview encoder) stays open for **10 s** after going idle, then
  closes. A receiver that comes back within 10 s waits about one frame. After that, the encoder re-opens on the
  next active frame (about 100–300 ms on AMF).
- Tally, metadata and discovery are untouched.

### 3.2 BreadCamera (`src/io/omt_output.*`)

- `OmthOutput::needsCpu()` and `wantsGpuRgba8()` (from the OMT copy-paths work) also return false while
  `send_->hasReceivers()` is false. The camera then skips the readback and the RGBA texture for this output.
- `deliver` still runs each tick (audio drain, tally update) and lets OMT-HX drop the video.
- First picture after a connect: the camera's readback is one tick behind (async), so expect about 2–3 ticks. That
  is acceptable.
- BreadReplay builds `OmthOutput` through BreadCamera's factory and gets this for free.

### 3.3 OMT-Bridge

- The sink (`src/engine/omt_sink.cpp`) makes no copy: it hands the source's UYVY to `sendVideoUyvy`, which the gate
  now returns from at once. So the bridge only moves its OMT-HX submodule forward. Its source keeps decoding for
  other outputs and previews; stopping an unused source is project 2.

## 4. Verification

- OMT-HX tests:
  - With no receiver, 120 `sendVideo` calls open no encoder (`stats().encoder` empty, or a test hook) and count
    `framesIdle == 120`.
  - A receiver connects: a decodable picture arrives within 5 frames, and the first HX frame received is a keyframe.
  - The receiver leaves: after 10 s the encoder is closed; before 10 s, it is still open. Tested on the gate's
    logic with a fake clock, not by waiting 10 s.
  - A probe of an idle HX sender still reads the source.
  - Plain OMT: the same idle and connect behaviour.
  - Tally needs a receiver, which wakes the sender, so it has no idle case to test.
- BreadCamera: the audit's bench P1 (4 HX outputs) with no receivers is within noise of P0 (no outputs). With one
  receiver per output, it is within noise of P1 as measured after the OMT copy-paths work.
- A vMix receiver pulls a BreadCamera plain-OMT output after it has been idle: the picture appears at once.

## 5. Order and dependencies

After the OMT copy-paths run merges (it changes `OmthOutput` and OMT-HX's `sendVideoD3D11`). One plan, three
repos: OMT-HX first, then BreadCamera, then OMT-Bridge.

## 6. Calls made (owner to check)

- A 10 s warm encoder after the last receiver leaves.
- The connection count is refreshed every 100 ms at most.
- No last-frame cache. Revisit for senders that send rarely.
- Any connection counts as a receiver, so a probe or a preview-only receiver also runs the full stream, as
  today. Exact preview-only needs a video-connection count in libomt (later, the owner's call).
- OMT-Bridge needs no code change: its sink makes no copy, and OMT-HX's gate drops the UYVY pack and the encode.
  Moving its OMT-HX submodule forward is enough.
