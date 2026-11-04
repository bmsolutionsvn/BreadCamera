# BreadReplay: B-Frame Recordings Play in Display Order — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A recording of an H.264/HEVC source with B-frames plays its pictures in the source's display order.

**Why:** The record-as-received merge (BreadReplay 3a8a09a, 2026-11-05) stores each access unit (AU) on the arrival clock, in decode order, and `FrameSource` caches each decoded picture under the arrival timestamp of the AU that carried it. Slot N therefore shows the picture of the N-th AU in decode order: a source coded `I P B B` plays `I P B B` instead of `I B B P`. Every GOP visibly jumps forward and back. The existing long-GOP test enshrines this: slot 47 is compared with `ref.at(enc.pts[47])`. Most SRT encoders (OBS x264/NVENC defaults, hardware encoders) send B-frames. Found by the reviewer, 2026-11-05. v2 segments are hours old, so the store format can still change freely.

**Architecture:** Keep the arrival clock as the timeline (the store's monotonic time model stays). Also store each AU's **source pts**. Within one GOP (a keyframe up to the next keyframe), the picture with the r-th smallest source pts is shown at the r-th slot (the r-th AU's arrival `tsUs`). The pictures take the GOP's slots in display order; the slots themselves do not move.

**Tech Stack:** C++20, BreadCamera libs, FFmpeg, doctest.

**Spec:** none; this plan carries the rule. The recording-formats spec (`docs/superpowers/specs/2026-11-03-breadreplay-recording-formats-design.md`) still holds otherwise.

## Global Constraints

- The store's timeline stays the arrival clock: `find`, `keyframeAtOrBefore`, gaps and segment rollover are unchanged.
- Source pts per stored frame, in µs. From SRT packets: `av_rescale_q(pkt->pts, timeBase, {1, 1000000})`. From coded OMT frames: `CodedVideoFrame::ptsUs`. When a source gives none (`AV_NOPTS_VALUE`, or 0 on every frame), or the frame is VMX/picture-recorded, store the arrival `tsUs`. That keeps today's behaviour for sources without reordering.
- Segment format: bump to version 3 with the source pts per frame. The reader still reads v1 and v2 and treats their source pts as `tsUs`.
- Ties in source pts (a broken source) keep decode order.
- A picture the decoder never outputs (for example the leading pictures of an open GOP after a seek) leaves its slot empty. `get` then returns what it returns today for a missing picture: no throw, no stall.
- Never push. Do not change `ui/`.

## Review Focus

1. **Display order:** for the 90-frame AMF B-frame recording, every slot `i` shows the reference picture with display pts `i` (luma within ±2), in forward playback, after a mid-GOP seek, and when stepping backwards. Test in Task 2.
2. **No reordering source:** a recording whose source pts equal its arrival times (VMX, HX, or no pts) plays exactly as before. The existing FrameSource tests stay green.
3. **Format:** a v2 segment written before this change still reads, with source pts = `tsUs`. Test in Task 1.
4. **Taps:** `Engine::Sink::onPacket` stores the packet's rescaled pts; `onCodedVideo` stores `f.ptsUs`; a packet with `AV_NOPTS_VALUE` stores the arrival time. Test in Task 1.

---

### Task 1: The store keeps each frame's source pts

**Files:** Modify `BreadReplay/src/store/segment.h/.cpp`, `src/store/input_store.h/.cpp`, `src/record/recorder.h/.cpp`, `src/engine/engine.cpp`. Tests: `tests/segment_test.cpp`, `tests/input_store_test.cpp`, and an engine/recorder test beside the existing `pushCodedAu` tests.

**Interfaces:**
- `FrameRecord::srcPtsUs` (int64). Written by `SegmentWriter::append` (new parameter, defaulting to `tsUs`).
- `Recorder::pushCodedAu(int64_t tsUs, int64_t srcPtsUs, StoredCodec, uint32_t w, uint32_t h, bool keyframe, std::vector<uint8_t> au)`.
- `InputStore` exposes the source pts of a stored frame (for example on `FrameRef`, or `srcPtsOf(ref)`), and the index of the next keyframe after a frame in the same segment (or the segment's end).

- [ ] **Step 1: Failing tests**
  - Round trip: a v3 segment written with source pts that differ from `tsUs` reads them back.
  - A v2 segment (write one with the v2 layout, or keep a small v2 fixture) reads with `srcPtsUs == tsUs`.
  - `onPacket` with a packet whose pts is 3 frames in a 1/90000 time base stores 100000 µs; with `AV_NOPTS_VALUE` it stores the arrival time.
- [ ] **Step 2: Run** `./build/release/br_tests.exe -tc="Segment*,InputStore*,Recorder*"` (after `scripts/build.ps1`) → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** them, then the full suite → green.
- [ ] **Step 5: Commit** `feat(store): frames keep the source's own pts`.

### Task 2: `FrameSource` shows each picture at its display slot

**Files:** Modify `BreadReplay/src/play/frame_source.h/.cpp`. Test: `tests/frame_source_test.cpp`.

**Interfaces:**
- Feed each AU to `GpuDecoder::decode` with its **source pts**, so a picture's `ptsUs` is the source pts.
- Per GOP (a keyframe index up to the next keyframe or the segment's end), a table from source pts to slot `tsUs`: sort the GOP's AUs by (source pts, decode index); the r-th one in that order maps to the arrival `tsUs` of the GOP's r-th AU in decode order. `cachePicture` keys each picture by that slot.
- The "feed on until the wanted slot is cached" bound (today `ref.index + 16`) still applies: a slot's picture comes out at most the reorder depth after its AU.

- [ ] **Step 1: Failing tests** (rework the long-GOP case; keep its hardware gate and AMF selection)
  - Store AU `i` with arrival `kBase + i*33333` and source pts `kBase + enc.pts[i]*33333`.
  - Every slot `i` (0..89), read in order, shows `ref.at(i)` (luma ±2). It should fail today wherever `enc.pts[i] != i`.
  - A cold `get` at slot 47 shows `ref.at(47)`; stepping back 58→31 stays in cache within the existing decode bound.
  - Assert the encoder really reordered (`B-frames: true`). If AMF is missing, keep today's logged fallback.
  - A recording with source pts == arrival pts (no reordering) still shows `ref.at(enc.pts[i])` at slot `i`. It must not regress for sources that send no pts.
- [ ] **Step 2: Run** `./build/release/br_tests.exe -tc="FrameSource*"` → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** them, then the full suite → green.
- [ ] **Step 5: Commit on `br-bframe-display-order`, merge into `main`** (`fix(play): B-frame recordings play in display order`, `merge: B-frame recordings play in display order`).
