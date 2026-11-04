# BreadReplay Clean-up: Replay Colour, Smooth GOP Boundaries — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two follow-ups from the record-as-received and B-frame runs (2026-11-05).

**Why:**
1. Compressed recordings carry no colour space. When `GpuDecoder` leaves `VideoFrameIn::bt709` false, the converter picks the matrix by height. A sub-720p OMT-HX recording therefore plays with BT.601, although OMT-HX is BT.709 at every size (BreadCamera `src/io/omt_input.cpp`: "OMT-HX decodes full-range BT.709 at every size, not by height"). An SRT source that signals its colour space is ignored the same way.
2. `FrameSource::getCompressed` builds its display-order slot table only for the requested GOP. Pictures fed past `gopEnd` (the reorder tail into the next GOP) are dropped, so the next `get()` in that GOP resets the decoder and re-feeds from its keyframe. That is one extra short decode at every GOP boundary in forward play (reviewer, 2026-11-05).

**Tech Stack:** C++20, BreadCamera libs, FFmpeg, doctest. Repo: BreadReplay only.

## Global Constraints

- Colour: store a per-segment colour hint: unknown, BT.601 or BT.709. The segment rolls on any change.
  - Coded OMT-HX frames (`onCodedVideo`, H.264/HEVC): BT.709.
  - SRT packets (`onPacket`): from `params->color_space`. `AVCOL_SPC_BT709` → BT.709; `BT470BG`/`SMPTE170M` → BT.601; anything else → unknown.
  - VMX and picture recordings: unknown (today's behaviour).
- `FrameSource` sets the picture's `bt709` from the hint when it is known, and leaves the decoder's answer (the height rule) when it is unknown. The range stays the decoder's answer.
- Segments without the field (v1–v3 written before this change) read as unknown. Bump the header version if the layout needs it, and keep reading every older version.
- GOP boundaries: when `feed` crosses `gopEnd`, build the next GOP's table and keep caching its pictures under their display slots, rather than dropping them. Display order stays exactly as the B-frame plan made it (its tests must stay green, unchanged).
- Never push. Do not change `ui/`.

## Review Focus

1. A 640×360 HEVC/H.264 recording tagged BT.709 decodes to `bt709 == true`; an untagged one keeps today's flag.
2. A v3 segment from before this change still reads, with an unknown colour.
3. Forward play across a GOP boundary on the AMF B-frame recording does **not** reset the decoder: decodes from slot 50 to 70 ≤ 25 for a GOP of 60, and no `reset()` at the boundary. Expose a reset counter for the test if needed.
4. Every existing FrameSource, Segment, InputStore and Recorder test stays green, unchanged.

---

### Task 1: Recordings keep their colour space

**Files:** `src/store/segment.h/.cpp`, `src/store/input_store.h/.cpp`, `src/record/recorder.h/.cpp`, `src/engine/engine.cpp` (the two taps), `src/play/frame_source.cpp`. Tests beside the existing ones.

- [ ] **Step 1: Failing tests** for Review Focus 1–2 and both taps' mapping.
- [ ] **Step 2: Run** `./build/release/br_tests.exe -tc="Segment*,InputStore*,Recorder*,FrameSource*"` → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** Full suite → green.
- [ ] **Step 5: Commit** `fix(play): recordings keep their colour space`.

### Task 2: Forward play crosses a GOP boundary without a re-seek

**Files:** `src/play/frame_source.h/.cpp`, `tests/frame_source_test.cpp`.

- [ ] **Step 1: Failing test** for Review Focus 3.
- [ ] **Step 2: Run** `-tc="FrameSource*"` → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4:** Full suite → green.
- [ ] **Step 5: Commit on `br-cleanup`, merge into `main`** (`perf(play): forward play crosses a GOP without a re-seek`, `merge: replay colour and GOP boundaries`).
