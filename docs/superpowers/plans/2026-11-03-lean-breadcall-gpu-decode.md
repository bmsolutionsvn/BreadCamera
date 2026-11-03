# BreadCamera Lean Paths Phase 4: BreadCall Guests Decoded on the GPU — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A BreadCall guest's H.264 is decoded with D3D11VA on the engine's device and its NV12 surface copied on the GPU, as `FfmpegInput` already does. With no hardware decoder, the decoded I420 is interleaved to NV12 on the CPU and uploaded with the leak-safe NV12 upload. No swscale to BGRA on either path for 4:2:0 video.

**Architecture:** The hardware-decode pieces of `FfmpegInput` (`pickFormat`, `makeHwDevice`, the `CopySubresourceRegion` of a D3D11 frame) move into one shared unit, `src/io/d3d11va.{h,cpp}`. A small `H264Decoder` there wraps "packet in → `VideoFrameIn` out" so the BreadCall input and a test can both drive it. `BreadCallInput::decodeVideo` uses it.

**Tech Stack:** C++20, FFmpeg (libavcodec, d3d11va hwcontext), D3D11, doctest.

**Spec:** `BreadCamera/docs/superpowers/specs/2026-11-03-breadcamera-lean-paths-design.md` (§3.1 item 4, phase 4)

**Runs after:** lean phase 3 (`2026-11-03-lean-uyvy-inputs.md`) is merged (it provides `uploadNv12Planes` in `src/gpu/upload.h` and `uploadBgra`).

## Global Constraints

- Hardware frames: `PixelLayout::NV12` (or `P010` for a 10-bit surface), `fullRange` from `AVFrame::color_range == AVCOL_RANGE_JPEG`, matrix by height as today.
- Software frames: `AV_PIX_FMT_YUV420P`/`YUVJ420P` → NV12 by interleaving U and V (one CPU pass over the chroma only) → `uploadNv12Planes`. Any other software format → swscale to BGRA (context cached) → `uploadBgra`, as today.
- Odd sizes are cropped to even (`& ~1`), as today.
- WebRTC changes guest resolution mid-call: every size works, without reopening the decoder by hand.
- FFmpeg's decode calls take the engine's context lock (`lock_ctx`), as `FfmpegInput` sets up.
- Never push. Do not touch `F:\Coding\BreadMedia\BreadReplay`.

## Review Focus

1. **A resolution change mid-stream** (640×360 → 1280×720): pictures of the new size, no stale surface. Test in Task 1.
2. **No hardware decoder** (forced): the software NV12 path gives the same picture within ±4. Test in Task 1.
3. **`FfmpegInput` unchanged in behaviour** after the move: its existing tests stay green.
4. **The decoder's surface pool:** a copied frame must not hold the decoder's surface (copy, then release the `AVFrame`). Test in Task 1 by decoding 300 frames with memory flat (< 64 MB growth).
5. **The live-call test** still passes where `BC_LIVECALL_*` is set (skips here).

---

### Task 1: Shared D3D11VA pieces and `H264Decoder`

**Files:**
- Create: `src/io/d3d11va.h`, `src/io/d3d11va.cpp` (add to `CMakeLists.txt`)
- Modify: `src/io/ffmpeg_input.h/.cpp` (use the shared pieces; behaviour unchanged)
- Test: new `tests/h264_decoder_test.cpp` (add to the test list), using `tests/support/h264_black720.h`

**Interfaces:**
- Produces, in `d3d11va.h`:
  - `AVBufferRef* makeD3d11vaDevice(GpuDevice& gpu);` — today's `FfmpegInput::makeHwDevice`.
  - `AVPixelFormat pickD3d11Format(AVCodecContext*, const AVPixelFormat*);` — today's `pickFormat`.
  - `bool copyDecodedFrame(GpuDevice& gpu, TexturePool& pool, const AVFrame* f, uint32_t w, uint32_t h, VideoFrameIn& out);` — a `AV_PIX_FMT_D3D11` frame copied with `CopySubresourceRegion` into a pooled texture; sets `tex`, `layout`; false for a software frame.
  - `bool uploadSoftwareFrame(GpuDevice& gpu, TexturePool& pool, const AVFrame* f, uint32_t w, uint32_t h, SwsContext*& sws, std::vector<uint8_t>& scratch, VideoFrameIn& out);` — the software rules from Global Constraints.
  - ```cpp
    class H264Decoder {
     public:
      H264Decoder(GpuDevice& gpu, TexturePool& pool, bool hardware = true);
      ~H264Decoder();
      // One Annex-B access unit in; zero or more pictures out through `emit`. Throws only when the decoder cannot open.
      void decode(std::span<const uint8_t> au, int64_t ptsUs, const std::function<void(VideoFrameIn)>& emit);
      bool hardware() const;  // true once a D3D11 frame came out
    };
    ```
    `hardware = false` forces the software path (for the test, and as the fallback when `makeD3d11vaDevice` throws).

- [ ] **Step 1: Failing tests** (`HardwareGpu`):
  - `"H264Decoder decodes black 720p on the GPU"`: split `kH264Black720` into access units (start codes; each IDR with its SPS/PPS) and feed 10 → at least 5 pictures, 1280×720, `layout == NV12`, `hardware()` true (skip with a `MESSAGE` if the GPU has no H.264 decoder); converted with `toLinear` → `toBgra8` → readback, the centre pixel is black within ±4.
  - `"H264Decoder's software path matches"`: the same with `hardware = false` → `layout == NV12`, centre black within ±4.
  - `"H264Decoder keeps memory flat"`: 300 access units (the clip looped), memory growth < 64 MB.
  - A size change: if a 640×360 Annex-B fixture is easy to make with the FFmpeg already linked (encode a few black frames in the test with `libx264` or `h264_mf` if available), feed 720p then 360p and check both sizes; otherwise log a `Ruling:` and rely on FFmpeg's get_format reinit.
- [ ] **Step 2: Run** `--test-case=H264Decoder*` → build error.
- [ ] **Step 3: Implement**, moving `FfmpegInput`'s pieces rather than copying them.
- [ ] **Step 4: Run** the new tests and `--test-case=FfmpegInput*` (with the brief's socket exclusions) → green.
- [ ] **Step 5: Commit** on `bcam-lean-breadcall-gpu`: `feat(io): shared D3D11VA decode, H264Decoder`.

### Task 2: `BreadCallInput` uses it

**Files:** Modify `src/io/breadcall_input.h/.cpp`; test `tests/breadcall_input_test.cpp`.

**Interfaces:**
- Consumes: `H264Decoder` (Task 1).
- Behaviour: `decodeVideo` hands each event's bytes to one `H264Decoder` (made in `openVideo`, hardware first), emitting through the existing live/state logic and `sink_->onVideo`. The decode-failure counting stays (a call that emits nothing counts as a failure). `emitVideo`'s swscale/BGRA path and `bgra_` go away. A rejoin flushes by recreating the decoder (as `avcodec_flush_buffers` did).
- The live test's `CHECK(sink.layout == BGRA8)` becomes `NV12`.

- [ ] **Step 1: Change the live test's expected layout** to `NV12` (it skips here without `BC_LIVECALL_*`; note that in the ledger).
- [ ] **Step 2: Implement.**
- [ ] **Step 3: Run** `--test-case=BreadCallInput*`, then the full verification command → green.
- [ ] **Step 4: Commit, merge into `main`** (`feat(io): BreadCall guests decode on the GPU`, `merge: lean phase 4, BreadCall GPU decode`).
