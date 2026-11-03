# BreadCamera Lean Paths Phase 5c: SRT → SRT Remux — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A camera with no enabled stage, fed by an SRT input carrying H.264 or HEVC, re-muxes the input's packets into its SRT outputs with no decode and no encode. It decodes only while a preview is visible or another output needs a picture.

**Architecture:** Phase 5a's pass targets gain a third kind, packets. `FfmpegInput` (SRT or any URL it demuxes) hands each video and audio packet, with its stream's codec parameters and time base, to `Camera::onPacket`, which submits it to packet targets through a new `OutFrame::packet`. `SrtOutput` gets a remux mode: on the first packet it builds its mpegts streams from the input's codec parameters (`avcodec_parameters_copy`), writes the header, and from the first video keyframe writes packets with `av_packet_rescale_ts`. Audio packets are copied too (AAC, MP2 or Opus as the input has them).

**Tech Stack:** C++20, FFmpeg (libavformat, libavcodec), libsrt, doctest.

**Spec:** `BreadCamera/docs/superpowers/specs/2026-11-03-breadcamera-lean-paths-design.md` (§3.2, phase 5, SRT → SRT)

**Runs after:** lean phase 5b is merged.

## Global Constraints

- A packet target: an `SrtOutput`, not half-rate, on a camera with no enabled stage whose input is an `FfmpegInput` with video codec `AV_CODEC_ID_H264` or `AV_CODEC_ID_HEVC`, while Live. The output's own bitrate setting does not apply while it re-muxes; its status says so (`"re-muxing the source"` in the output's note or status).
- The first video packet written is a keyframe (`AV_PKT_FLAG_KEY`); packets before it are dropped. A new target starts the same way. A change of the input's codec parameters (a new stream, a size change) rebuilds the muxer streams at the next keyframe.
- `FfmpegInput` keeps decoding only while `wantsGpuVideo()`; it still demuxes and decodes audio for the tick path while non-target outputs exist.
- Never push. Do not touch `F:\Coding\BreadMedia\BreadReplay`.

## Review Focus

1. **Remuxed bytes:** the video payload a receiver demuxes equals the input's (compare packet sizes and the first keyframe's bytes). Test in Task 2.
2. **Timestamps:** a receiver's video and audio stay in sync (first audio and video pts within 100 ms) and pts increase monotonically across a muxer rebuild. Test in Task 2.
3. **An input that starts mid-GOP:** nothing is written before the first keyframe. Test in Task 1 (unit, on the remux writer).
4. **Switching an effect on:** the output goes back to encoding without a broken stream (a fresh header or a keyframe-led continuation). Test in Task 2.
5. **SRT listener/caller timing limits** on this PC: reuse the ports and modes the existing SRT tests use (WSL2 holds TCP 44620–48715; the SRT benchmark used UDP 40001–40004).

---

### Task 1: The remux writer

**Files:** Create `src/io/remux.h`, `src/io/remux.cpp` (add to `CMakeLists.txt`); test new `tests/remux_test.cpp`.

**Interfaces:**
- Produces:
  ```cpp
  // One coded packet as the input demuxed it, with what a muxer needs to take it as is.
  struct MediaPacket {
    std::shared_ptr<const AVCodecParameters> params;  // shared per stream; replaced when the stream changes
    AVRational timeBase{1, 90000};
    bool video = true;
    AVPacket* pkt = nullptr;  // owned (av_packet_free on destruction); a ref of the input's packet
    ~MediaPacket();
  };
  // Writes MediaPackets into an AVFormatContext it does not own: makes one stream per input stream on first sight,
  // writes the header once, drops video until a keyframe, rescales timestamps.
  class RemuxWriter {
   public:
    explicit RemuxWriter(AVFormatContext* fmt);
    void write(const MediaPacket& p);  // throws on a write error
    uint64_t dropped() const;          // packets dropped while waiting for a keyframe
  };
  ```
- [ ] **Step 1: Failing tests** (no network; an mpegts muxer writing to a memory `AVIOContext`): feed packets from a small H.264 stream (`tests/support/h264_black720.h` split into access units; mark every 3rd as a keyframe to simulate a GOP, or decode the clip's own flags), starting at a non-keyframe → `dropped()` equals the leading non-keyframes; the output, read back with `avformat_open_input` on the memory buffer, has one H.264 stream of 1280×720 and the same number of video packets as written after the first keyframe.
- [ ] **Step 2: Run** `--test-case=RemuxWriter*` → build error.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** → green.
- [ ] **Step 5: Commit** on `bcam-lean-pass-through-srt`: `feat(io): a remux writer for packets passed through`.

### Task 2: Packet targets, `FfmpegInput` hands packets, `SrtOutput` re-muxes

**Files:** Modify `src/io/output.h`, `src/io/input.h`, `src/engine/camera.h/.cpp`, `src/io/ffmpeg_input.h/.cpp`, `src/io/srt_output.h/.cpp`; tests `tests/ffmpeg_input_test.cpp` and a new e2e case beside the existing SRT ones.

**Interfaces:**
- Consumes: `MediaPacket`, `RemuxWriter` (Task 1); 5a's pass targets, audio handling and `wantsGpuVideo`.
- Produces:
  - `OutFrame::packet` (`std::shared_ptr<const MediaPacket>`). `Output::takesPackets(AVCodecID video) const { return false; }`; `SrtOutput` returns true for H.264 and HEVC.
  - `InputSink::wantsPackets() const`, `virtual void onPacket(std::shared_ptr<const MediaPacket>)` (input thread). `Camera` implements both with 5a's target rules; packets go to targets in order under `passMutex_`. Audio packets go with them (targets do not take the pass audio FIFO's samples).
  - `FfmpegInput`: while `wantsPackets()`, after `av_read_frame`, wrap a ref of the packet (`av_packet_ref`) with its stream's params (one shared `AVCodecParameters` copy per stream, renewed on change) and call `onPacket`; decode the video packet only while `wantsGpuVideo()` (resetting the decoder with `avcodec_flush_buffers` when decoding turns on, and dropping until a keyframe).
  - `SrtOutput::deliver` with `f.packet` → a `RemuxWriter` on its own context (opened as today); its encoder path stays for picture frames. Switching from packets to pictures closes and reopens the connection's muxer (a new header), which receivers handle as a stream restart.
- [ ] **Step 1: Failing tests** (`ffmpeg_input_test` style, SRT on localhost with the ports the existing tests use):
  - `"SRT pass-through: packets reach the output unchanged"`: an FFmpeg-made H.264 SRT source (the existing test feeder) → camera with one `SrtOutput`, no stages, no preview → an FFmpeg reader on the output's URL demuxes H.264 at the source's size; video packet sizes after the first keyframe equal the source's, in order; the input's decoded-frame count stays 0 (add a counter if there is none).
  - `"SRT pass-through: audio and video stay together"`: first audio and video pts within 100 ms; pts monotonic.
  - `"SRT pass-through: an effect turned on goes back to encoding"`: enable colour at 1 s → the reader keeps getting decodable video after the switch (it may reopen once).
- [ ] **Step 2: Run** → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** them, the 5a/5b pass-through tests, then the full verification command (keep the brief's SRT exclusions for the two known-flaky cases) → green.
- [ ] **Step 5: Commit, merge into `main`** (`feat(io): SRT inputs re-mux into SRT outputs`, `merge: lean phase 5c, SRT pass-through`).
