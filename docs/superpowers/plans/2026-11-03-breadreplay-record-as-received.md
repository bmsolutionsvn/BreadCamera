# BreadReplay: Record Compressed Sources as Received — Implementation Plan (phase 1 of 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** BreadReplay stores OMT-HX and H.264/HEVC SRT inputs exactly as they arrive (no decode, no VMX encode), and plays them back frame-accurately by decoding on the GPU with seek and a cache. Picture sources keep VMX.

**Architecture:** Store format v2 tags each segment with its codec and each frame with a keyframe flag. The recorder writes coded access units straight into the store. BreadCamera's `OmtInput` hands HX coded frames to a sink that asks for them; SRT uses lean phase 5c's packet hand-off. `FrameSource` decodes non-VMX frames with BreadCamera's `GpuDecoder` on BreadReplay's own D3D11 device: from the keyframe at or before the target, forward, caching every picture it passes (GPU textures, LRU, VRAM budget). `Renderer` takes NV12 straight into `toLinear`, and UYVY through `uploadUyvy` + `PixelLayout::UYVY` instead of the CPU `uyvyToBgra`.

**Tech Stack:** C++20, BreadCamera libs (built from `../BreadCamera`), FFmpeg d3d11va, libvmx, doctest.

**Spec:** `BreadCamera/docs/superpowers/specs/2026-11-03-breadreplay-recording-formats-design.md` (§4.1–4.4, phase 1). Phase 2 (per-input overrides, the pre-record check, the tile's scrub label) is UI work; plan it with the BreadReplay UI session after this merges.

**Runs after:** lean phase 5c (packets), lean phase 3 (`uploadUyvy`), lean phase 4 (`GpuDecoder`), the BreadReplay native-preview plan, and the BreadReplay UI session's go-ahead.

## Global Constraints

- Auto rule: OMT-HX → as received; SRT with H.264/HEVC → as received; plain OMT → VMX as received (today); SDI, NDI, other SRT codecs → VMX encode (today).
- Store v2: `SegmentHeader::version = 2` and `codec` (`Vmx`, `H264`, `Hevc`); v1 segments read as VMX. `FrameRecord::reserved` becomes `flags`, bit 0 = keyframe; VMX frames always set it. `previewLen` is 0 for H.264/HEVC.
- Access units are stored Annex-B. A keyframe without parameter sets gets them prepended (from the stream's extradata for SRT; OMT-HX keyframes already carry them).
- A segment starts on a keyframe: frames before the first keyframe of a segment are dropped (counted in `RecorderStats::dropped`). A codec or size change closes the segment.
- OMT-HX alpha is not recorded (the fill's access unit only: `alphaBytes` trimmed). Replays do not key.
- OMT-HX sources are asked for a keyframe every 500 ms while recorded as received (spec §7, owner to check).
- Audio stays float32 interleaved: OMT audio as it is; SRT audio decoded once by `FfmpegInput` (`onAudio`) as today.
- Decoded-frame cache: at most 64 frames per input and 1 GB of NV12 in total (w × h × 1.5 per frame), least recently used out.
- Never push. Coordinate with the BreadReplay UI session: do not change `ui/`.

## Review Focus

1. **Frame-accurate cue on a long GOP** (2 s, SRT H.264 with B-frames): the picture shown at a cue equals a reference decode of that frame (compare decoded Y planes, ±2). Test in Task 4.
2. **Reverse and 0.25× slow motion** at 1080p60: after the first seek, each step back hits the cache (decode count stays flat within a GOP). Test in Task 4.
3. **A recording that starts mid-GOP:** nothing before the first keyframe is stored; playback never shows garbage. Test in Task 2.
4. **Crash recovery:** a v2 segment cut mid-frame recovers as v1 did. Test in Task 1.
5. **v1 shows still play** after the upgrade. Test in Task 1.

---

### Task 1: Store v2

**Files:** Modify `BreadReplay/src/store/segment.h/.cpp`, `src/store/input_store.h/.cpp`; tests `tests/segment_test.cpp`, `tests/input_store_test.cpp`.

**Interfaces:**
- Produces:
  - `enum class StoredCodec : uint8_t { Vmx, H264, Hevc };` `SegmentHeader::codec` (default `Vmx`), `version = 2`. Header serialisation writes it; a v1 file reads as `Vmx`.
  - `FrameRecord::flags` (bit 0 `kKeyframe`).
  - `SegmentWriter::append(int64_t tsUs, std::span<const uint8_t> video, uint32_t previewLen, std::span<const float> audio, bool keyframe = true);` `InputStore::append(…, bool keyframe = true)`.
  - `InputStore::changeVideoFormat(uint32_t w, uint32_t h, bc::Rate rate, StoredCodec codec = StoredCodec::Vmx);`
  - `std::optional<FrameRef> InputStore::keyframeAtOrBefore(const FrameRef&) const;` `StoredCodec InputStore::codecOf(const FrameRef&) const;` `bool InputStore::isKeyframe(const FrameRef&) const;`
  - `InputStore` drops a non-key frame appended to a segment that has no frames yet (returns without writing; a counter `droppedBeforeKeyframe()`).
- [ ] **Step 1: Failing tests:** a v2 HEVC segment round-trips codec, keyframe flags and bytes; a v1 segment (write one with the old header layout, kept as a byte fixture in the test) reads as `Vmx` with every frame a keyframe; a segment starting with two P-frames stores from the first keyframe; `keyframeAtOrBefore` finds the right frame across a segment boundary; truncating the essence file mid-frame recovers the earlier frames (as the existing recovery test does).
- [ ] **Step 2: Run** (`scripts/test.ps1 -Filter …` in BreadReplay, or its build + `br_tests.exe`; use the repo's own commands) → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the store tests, then the full BreadReplay suite → green.
- [ ] **Step 5: Commit** on `br-record-as-received`: `feat(store): v2 segments with a codec and keyframe flags`.

### Task 2: Coded frames reach the recorder

**Files:** Modify `BreadCamera/src/io/input.h`, `BreadCamera/src/io/omt_input.cpp` (BreadCamera branch `bcam-coded-hx-tap`, merged into BreadCamera `main` first); `BreadReplay/src/record/recorder.h/.cpp`, `src/engine/engine.h/.cpp` (`Engine::Sink`); tests `BreadCamera/tests/omt_input_test.cpp`, `BreadReplay/tests/recorder_test.cpp`.

**Interfaces:**
- BreadCamera:
  - `CodedVideoFrame` gains `bool keyframe = true;`; `codec` is `"vmx"`, `"hevc"` or `"h264"`.
  - `InputSink::wantsCodedHx() const { return false; }`.
  - `OmtInput`: with `wantsCodedVmx() && wantsCodedHx()`, an HX source stays on the `Coded` receiver and its frames go to `onCodedVideo` (fill only, keyframe from `CodedFrame::keyframe`), with `requestKeyframe()` every 500 ms. Without `wantsCodedHx()`, today's behaviour (reopen as pictures).
- BreadReplay:
  - `Recorder::pushCodedAu(int64_t tsUs, StoredCodec codec, uint32_t w, uint32_t h, bool keyframe, std::vector<uint8_t> au);` — written in arrival order with no encode, through the same writer ordering as `pushCoded`; a codec or size change calls `changeVideoFormat`.
  - `Engine::Sink`: `wantsCodedHx()` true; `wantsPackets()` true (lean 5c's interface); `onCodedVideo` with `"hevc"`/`"h264"` → `pushCodedAu`; `onPacket` with an H.264/HEVC video packet → `pushCodedAu` (size from the packet's codec parameters, parameter sets prepended to a keyframe that lacks them, from `extradata`); other packets ignored. `FfmpegInput` must still call `onAudio` with decoded audio for a sink that takes packets: check lean 5c's behaviour and add it if missing (BreadCamera, same branch).
- [ ] **Step 1: Failing tests:** BreadCamera — an HX source to a sink wanting both → `onCodedVideo` gets `"hevc"` frames, the first one a keyframe, and keyframes at least every 600 ms (hardware HEVC). BreadReplay — `pushCodedAu` with a GOP starting at a P-frame stores from the keyframe on, with flags right and bytes equal; the encoder is never called (stats `encodeMsAvg == 0`).
- [ ] **Step 2: Run** → fail.
- [ ] **Step 3: Implement** (BreadCamera part first; merge it into BreadCamera `main` with `merge: coded HX tap for recorders` before building BreadReplay against it).
- [ ] **Step 4: Run** both suites → green.
- [ ] **Step 5: Commit** (BreadReplay) `feat(record): compressed inputs are stored as received`.

### Task 3: GPU decode on BreadReplay's device

**Files:** Modify `BreadReplay/src/play/frame_source.h/.cpp`, `src/engine/engine.cpp` (`rebuildFrameSource` passes the device and pool); test `tests/frame_source_test.cpp`.

**Interfaces:**
- Consumes: `bc::GpuDecoder` (lean phase 4), store v2 (Task 1).
- Produces:
  - `FrameSource(std::map<std::string, const InputStore*> stores, bc::GpuDevice* gpu, bc::TexturePool* pool, int threads = 4, size_t cacheFrames = 32);` (null device: H.264/HEVC frames return null).
  - `DecodedFrame` gains `bc::VideoFrameIn picture;` — set (NV12/P010 texture) for H.264/HEVC frames; `uyvy` stays for VMX.
  - `get(input, ts)` for a non-VMX frame: if this input's decoder last emitted the frame just before `ref` in the same segment, feed the next access units until `ref`'s picture comes out; otherwise `reset()`, feed from `keyframeAtOrBefore(ref)` forward to `ref`. Every picture emitted on the way is cached under its own timestamp. Pictures map to frames by timestamp (decode order may differ from display order).
  - `preview(input, ts)` for a non-VMX frame: decode the keyframe at or before `ts` with a second decoder per input (playback state untouched), `conv.fit` to the VMX preview size, read back to BGRA; reuse the last result while the keyframe is the same.
  - `uint64_t FrameSource::decodes() const` (pictures decoded), for tests.
- [ ] **Step 1: Failing tests** (hardware GPU; skip without a decoder): record a short H.264 stream with a 2 s GOP and B-frames, made in the test with FFmpeg's encoder (`libx264` or `h264_mf`, whichever is linked; log which) at 640×360 30 fps into an `InputStore` through `pushCodedAu`; then:
  - `get` at frame 47 equals a reference software decode's frame 47 (Y plane ±2);
  - stepping backward from frame 59 to 31 costs at most one seek's decodes (`decodes()` grows by ≤ 60), the rest from cache;
  - `preview` of frame 47 returns the keyframe's picture at the VMX preview size.
- [ ] **Step 2: Run** → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → green; full suite → green.
- [ ] **Step 5: Commit** `feat(play): recordings in H.264/HEVC decode on the GPU with seek and cache`.

### Task 4: Renderer takes NV12 and UYVY on the GPU; end to end

**Files:** Modify `BreadReplay/src/play/renderer.cpp`; tests `tests/renderer_test.cpp`, `tests/engine_test.cpp`.

**Interfaces:**
- `Renderer::render`: `f.picture.tex` set → `conv_.toLinear(*tex, layout, fullRange, bt709)`; else UYVY → `bc::uploadUyvy` + `toLinear(UYVY, false)` (no `uyvyToBgra`). Zoom and fit as today.
- [ ] **Step 1: Failing tests:** `renderer_test` — a UYVY frame renders the same colours as before (centre within ±6); an NV12 `DecodedFrame` renders. `engine_test` (hardware HEVC) — an OMT-HX source recorded for 3 s, then a cue at 1.5 s and 30 frames of reverse play: every rendered frame non-black, the recorder's encode count 0, and `FrameSource::decodes()` within one GOP's worth per seek.
- [ ] **Step 2: Run** → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → green; full suite → green.
- [ ] **Step 5: Commit, merge into BreadReplay `main`** (`feat(play): render NV12 and UYVY on the GPU`, `merge: record compressed sources as received`).
