# BreadCamera M4a — SRT out, NDI in, OMT out, USB capture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The I/O adapters the build PC can test end to end: SRT output (hardware H.264 + AAC in MPEG-TS), NDI input, OMT output (alpha), USB/UVC capture input through Media Foundation, plus the router validation, discovery commands and UI forms for every new kind.

**Architecture:** Each adapter is an `Input` or a `QueuedOutput` (M1 Task 7) created by `DefaultAdapterFactory` from show params validated by `validateInput`/`validateOutput` (M1 Task 11). The H.264 encoder opening logic moves from `PreviewEncoder` into a shared `openH264Encoder` so the SRT output and previews use one encoder order (spec §7.2). Discovery (NDI sources, capture devices) is exposed as read-only commands for the UI pickers.

**Tech Stack:** as before, plus libomt (OMT) v1.0.0.19 prebuilt binaries (MIT), Media Foundation source reader.

**Spec:** `docs/superpowers/specs/2026-10-08-breadcamera-design.md` (§4.1–§4.3, §7.1, §7.2).

**Branch:** `bcam-m4a`, cut from `bcam-m3`.

## Global Constraints

- All earlier Global Constraints apply (SDK options + `BC_REQUIRES`, never black, LGPL FFmpeg, 127.0.0.1 + token, DPAPI for every `passphrase`, Vietnamese-first UI copy).
- Outputs never block the tick: every new output is a `QueuedOutput` (queue of 2, drop-oldest, own thread, retry with backoff).
- SRT output encodes with the first encoder that opens in the order `h264_nvenc`, `h264_amf`, `h264_qsv`, `h264_mf`; audio is FFmpeg's native `aac` (LGPL) at 48 kHz stereo 160 kb/s; container MPEG-TS; no alpha (spec §4.3).
- OMT output carries alpha when the frame has it (`OMTVideoFlags_Alpha`); NDI input accepts any source and delivers BGRA.
- USB capture audio is out of scope for v1 (video only) — say so in the UI ("Không có âm thanh" note) and the follow-ups.
- New third-party binaries are fetched by `scripts/fetch-sdks.ps1` at a pinned version with a SHA-256 check, into `third_party/` (git-ignored).

## Review Focus

1. **An SRT output in listener mode with nobody connected, then a caller that connects late and leaves** — the tick never stalls, the output shows `connecting`, goes `live` when the caller arrives, and returns to `connecting` when it leaves — owned by Task 1.
2. **An NDI source that disappears and comes back** — the camera holds, then slates, then goes live again without operator action — owned by Task 2.
3. **A USB device unplugged mid-show** — the input reports offline with the device name and reconnects when it is plugged back — owned by Task 4 (automated part: device not found at start).
4. **Bad output/input params from the UI** (port 0, bitrate 999999, empty NDI source, unknown device) — the router refuses with `invalid` and a readable message; nothing is created — owned by Task 5.
5. **Resolution changes upstream while SRT out is encoding** — the output keeps its configured size (scaled), no encoder restart storm — owned by Task 1.

## File Structure

```
src/io/h264_encoder.h/.cpp     openH264Encoder + encoder order (moved from preview_encoder)
src/io/srt_output.h/.cpp       SRT out (H.264 + AAC, MPEG-TS)
src/io/ndi_input.h/.cpp        NDI receive (BC_WITH_NDI)
src/io/ndi_discovery.h/.cpp    listNdiSources (BC_WITH_NDI)
src/io/omt_output.h/.cpp       OMT send (BC_WITH_OMT)
src/io/mf_capture_input.h/.cpp USB/UVC capture + listCaptureDevices
scripts/fetch-sdks.ps1         pinned downloads (libomt)
cmake/FindOMT.cmake
```

## Execution notes for the controller

- 6 tasks → batched reviews (1–3, 4–6).
- Task 3 needs network access once (GitHub release download) — the night run has it; if the download fails, ledger it and continue (OMT turns itself off).

---

### Task 1: Shared H.264 encoder and the SRT output

**Files:**
- Create: `src/io/h264_encoder.h`, `src/io/h264_encoder.cpp`, `src/io/srt_output.h`, `src/io/srt_output.cpp`
- Modify: `src/engine/preview_encoder.cpp` (use `openH264Encoder`), `src/io/queued_output.h` (protected `stopRequested()`), `src/io/srt_params.h/.cpp` (`srtOutputOptionsFromParams`), `CMakeLists.txt`
- Test: `tests/srt_output_test.cpp`

**Interfaces:**
- Consumes: `QueuedOutput`, `OutFrame` (M1 Task 7); `FfmpegInput`, `srtOptionsFromParams` (M1 Task 6); `PreviewEncoder` (M1 Task 10).
- Produces:
  - `bc::OpenedEncoder { AVCodecContext* ctx; std::string name; }`, `bc::OpenedEncoder bc::openH264Encoder(int width, int height, int fpsNum, int fpsDen, int64_t bitRate, int gop, bool globalHeader)` (throws `std::runtime_error` listing what was tried; caller frees with `avcodec_free_context`), `const std::vector<std::string>& bc::h264EncoderOrder()`. `PreviewEncoder::encoderOrder()` now returns `h264EncoderOrder()`.
  - `bool QueuedOutput::stopRequested() const` (protected) — subclasses use it in FFmpeg interrupt callbacks.
  - `bc::SrtOutputOptions { std::string url; std::map<std::string,std::string> avOptions; int width = 1920, height = 1080; int bitrateKbps = 8000; }`, `bc::SrtOutputOptions bc::srtOutputOptionsFromParams(const nlohmann::json&)` — same keys and rules as the SRT input (`url` | `mode`+`host`+`port`, `latencyMs`, `passphrase`, `streamId`) plus `bitrateKbps` 1000..50000 (default 8000), `width`/`height` one of 1280×720, 1920×1080 (default 1920×1080).
  - `bc::SrtOutput(SrtOutputOptions)` — `needsCpu() == true`, error code `"srt-out-failed"`.

Behaviour: `open()` opens the SRT connection (caller: connect; listener: wait for a caller, interruptible by `stop()`), then the muxer header; the encoder opens on the first frame. Every frame is scaled to the configured size (BGRA → NV12 via swscale), so an upstream size change never restarts the encoder. Audio from `OutFrame::audio` goes through a FIFO into 1024-sample AAC frames. A write error throws → the base retries with backoff (`connecting` again in listener mode).

- [ ] **Step 1: Write the failing test**

`tests/srt_output_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <condition_variable>
#include <thread>

#include "gpu/device.h"
#include "io/ffmpeg_input.h"
#include "io/srt_output.h"
#include "io/srt_params.h"
#include "support/requires.h"

using namespace std::chrono_literals;

namespace {
struct Sink : bc::InputSink {
  std::mutex m;
  int video = 0;
  int64_t audio = 0;
  uint32_t width = 0;
  void onVideo(bc::VideoFrameIn f) override {
    std::lock_guard l(m);
    ++video;
    width = f.tex->width;
  }
  void onAudio(bc::AudioBlock a) override {
    std::lock_guard l(m);
    audio += static_cast<int64_t>(a.samples.size()) / 2;
  }
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
};

void feed(bc::SrtOutput& out, std::atomic<bool>& run, uint32_t w, uint32_t h) {
  auto img = std::make_shared<bc::CpuImage>();
  img->width = w;
  img->height = h;
  img->bgra.assign(static_cast<size_t>(w) * h * 4, 128);
  auto audio = std::make_shared<std::vector<float>>(800 * 2, 0.1f);
  uint64_t tick = 0;
  while (run) {
    bc::OutFrame f;
    f.tick = tick++;
    f.rate = {60, 1};
    f.bgra = img;
    f.audio = audio;
    out.submit(std::move(f));
    std::this_thread::sleep_for(16ms);
  }
}
}  // namespace

TEST_CASE("srtOutputOptionsFromParams: validation") {
  auto o = bc::srtOutputOptionsFromParams({{"mode", "listener"}, {"port", 9100}, {"bitrateKbps", 6000}});
  CHECK(o.url == "srt://0.0.0.0:9100");
  CHECK(o.bitrateKbps == 6000);
  CHECK(o.width == 1920);
  CHECK_THROWS_AS(bc::srtOutputOptionsFromParams({{"mode", "listener"}, {"port", 9100}, {"bitrateKbps", 999999}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOutputOptionsFromParams({{"mode", "listener"}, {"port", 9100}, {"width", 1000}, {"height", 500}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOutputOptionsFromParams({{"mode", "caller"}, {"port", 0}, {"host", "x"}}), std::invalid_argument);
}

TEST_CASE("SrtOutput: a listener output is read by an SRT caller input, picture and sound" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  auto opts = bc::srtOutputOptionsFromParams({{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47951}, {"width", 1280}, {"height", 720}});
  bc::SrtOutput out(opts);
  out.start();
  std::this_thread::sleep_for(300ms);
  CHECK(out.status().state == bc::SignalState::Connecting);  // nobody connected yet
  std::atomic<bool> run{true};
  std::thread feeder([&] { feed(out, run, 1920, 1080); });

  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::FfmpegInput in(*gpu, pool, bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "127.0.0.1"}, {"port", 47951}}));
  Sink sink;
  in.start(sink);
  for (int i = 0; i < 100; ++i) {
    {
      std::lock_guard l(sink.m);
      if (sink.video > 60) break;
    }
    std::this_thread::sleep_for(100ms);
  }
  in.stop();
  {
    std::lock_guard l(sink.m);
    CHECK(sink.video > 60);
    CHECK(sink.width == 1280);  // scaled to the configured size
    CHECK(sink.audio > 24000);
  }
  CHECK(out.status().state == bc::SignalState::Live);
  run = false;
  feeder.join();
  auto t0 = std::chrono::steady_clock::now();
  out.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 2000ms);
}

TEST_CASE("SrtOutput: stop() returns while a listener waits for a caller") {
  bc::SrtOutput out(bc::srtOutputOptionsFromParams({{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47952}}));
  out.start();
  std::this_thread::sleep_for(300ms);
  auto t0 = std::chrono::steady_clock::now();
  out.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
}
```

Add it to `bc_tests`, and `src/io/h264_encoder.cpp src/io/srt_output.cpp` to `bc_io`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "SrtOutput*,srtOutputOptions*"`
Expected: build FAILS.

- [ ] **Step 3: Move encoder opening into `bc_io`**

`src/io/h264_encoder.h`:
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;

namespace bc {
struct OpenedEncoder {
  AVCodecContext* ctx = nullptr;  // free with avcodec_free_context
  std::string name;
};
// spec §7.2: NVIDIA first, then AMD, Intel, Media Foundation.
const std::vector<std::string>& h264EncoderOrder();
// Low-latency, no B-frames, NV12 input. Throws std::runtime_error listing every attempt.
OpenedEncoder openH264Encoder(int width, int height, int fpsNum, int fpsDen, int64_t bitRate, int gop, bool globalHeader);
}  // namespace bc
```

`src/io/h264_encoder.cpp` — move the body of the `PreviewEncoder` constructor's encoder loop (M1 Task 10, from `std::string tried;` through `if (!ctx_) throw ...`) into this function, with these changes: `c->time_base = {fpsDen, fpsNum}; c->framerate = {fpsNum, fpsDen};`, `c->bit_rate = bitRate; c->gop_size = gop;`, set `AV_CODEC_FLAG_GLOBAL_HEADER` only when `globalHeader`, and return `{c, name}` instead of assigning members:
```cpp
#include "io/h264_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <stdexcept>

namespace bc {
namespace {
std::string averr(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(rc, buf, sizeof(buf));
  return buf;
}
}  // namespace

const std::vector<std::string>& h264EncoderOrder() {
  static const std::vector<std::string> order{"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf"};
  return order;
}

OpenedEncoder openH264Encoder(int w, int h, int fpsNum, int fpsDen, int64_t bitRate, int gop, bool globalHeader) {
  std::string tried;
  for (const auto& name : h264EncoderOrder()) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) {
      tried += name + " (not built); ";
      continue;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = w;
    c->height = h;
    c->time_base = {fpsDen, fpsNum};
    c->framerate = {fpsNum, fpsDen};
    c->pix_fmt = AV_PIX_FMT_NV12;
    c->gop_size = gop;
    c->max_b_frames = 0;
    c->bit_rate = bitRate;
    c->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (globalHeader) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* o = nullptr;
    if (name == "h264_nvenc") {
      av_dict_set(&o, "preset", "p1", 0);
      av_dict_set(&o, "tune", "ull", 0);
      av_dict_set(&o, "zerolatency", "1", 0);
      av_dict_set(&o, "forced-idr", "1", 0);
    } else if (name == "h264_amf") {
      av_dict_set(&o, "usage", "ultralowlatency", 0);
      av_dict_set(&o, "forced_idr", "1", 0);
    } else if (name == "h264_qsv") {
      av_dict_set(&o, "preset", "veryfast", 0);
      av_dict_set(&o, "async_depth", "1", 0);
    } else if (name == "h264_mf") {
      av_dict_set(&o, "rate_control", "cbr", 0);
      av_dict_set(&o, "scenario", "display_remoting", 0);
    }
    const int rc = avcodec_open2(c, codec, &o);
    av_dict_free(&o);
    if (rc < 0) {
      tried += name + " (" + averr(rc) + "); ";
      avcodec_free_context(&c);
      continue;
    }
    return {c, name};
  }
  throw std::runtime_error("no H.264 encoder could be opened: " + tried);
}
}  // namespace bc
```
Then in `src/engine/preview_encoder.cpp` replace that loop with:
```cpp
  auto opened = openH264Encoder(w, h, fps, 1, w >= 1280 ? 4'000'000 : 1'000'000, fps, true);
  ctx_ = opened.ctx;
  name_ = opened.name;
```
and make `PreviewEncoder::encoderOrder()` return `h264EncoderOrder()`. Run `-Filter "*Preview*"` — the M1 preview tests must still pass.

In `src/io/queued_output.h` add to the protected section: `bool stopRequested() const { return stopping_; }`.

- [ ] **Step 4: SRT output params**

`src/io/srt_params.h` — add:
```cpp
struct SrtOutputOptions {
  std::string url;
  std::map<std::string, std::string> avOptions;
  int width = 1920, height = 1080;
  int bitrateKbps = 8000;
};
// Same connection keys as the input (spec §4.2) plus bitrateKbps, width, height. Throws std::invalid_argument.
SrtOutputOptions srtOutputOptionsFromParams(const nlohmann::json& params);
```
`src/io/srt_params.cpp` — add:
```cpp
SrtOutputOptions srtOutputOptionsFromParams(const json& p) {
  if (!p.is_object()) throw std::invalid_argument("SRT settings must be an object");
  json conn = p;
  for (auto k : {"bitrateKbps", "width", "height"}) conn.erase(k);
  auto in = srtOptionsFromParams(conn);
  SrtOutputOptions o;
  o.url = in.url;
  o.avOptions = in.avOptions;
  o.bitrateKbps = intIn(p, "bitrateKbps", 8000, 1000, 50000);
  o.width = intIn(p, "width", 1920, 1280, 1920);
  o.height = intIn(p, "height", 1080, 720, 1080);
  if (!((o.width == 1920 && o.height == 1080) || (o.width == 1280 && o.height == 720))) {
    throw std::invalid_argument("size must be 1920x1080 or 1280x720");
  }
  return o;
}
```

- [ ] **Step 5: Implement the output**

`src/io/srt_output.h`:
```cpp
#pragma once
#include <cstdint>
#include <vector>

#include "io/queued_output.h"
#include "io/srt_params.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct SwsContext;

namespace bc {
// H.264 (hardware) + AAC in MPEG-TS over SRT (spec §4.3). No alpha.
class SrtOutput final : public QueuedOutput {
 public:
  explicit SrtOutput(SrtOutputOptions opts) : opts_(std::move(opts)) {}
  ~SrtOutput() override { stop(); }
  bool needsCpu() const override { return true; }

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "srt-out-failed"; }

 private:
  static int interrupt(void* self);
  void openEncoders(const OutFrame& first);
  void writePacket(AVCodecContext* c, AVStream* st);
  SrtOutputOptions opts_;
  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* venc_ = nullptr;
  AVCodecContext* aenc_ = nullptr;
  AVStream* vst_ = nullptr;
  AVStream* ast_ = nullptr;
  AVFrame* vframe_ = nullptr;
  AVFrame* aframe_ = nullptr;
  SwsContext* sws_ = nullptr;
  uint32_t swsW_ = 0, swsH_ = 0;
  std::vector<float> audioFifo_;  // interleaved stereo
  int64_t vpts_ = 0, apts_ = 0;
  bool headerWritten_ = false;
};
}  // namespace bc
```

`src/io/srt_output.cpp`:
```cpp
#include "io/srt_output.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
}

#include <stdexcept>

#include "io/h264_encoder.h"

namespace bc {
namespace {
void check(int rc, const char* what) {
  if (rc < 0) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(rc, buf, sizeof(buf));
    throw std::runtime_error(std::string(what) + ": " + buf);
  }
}
}  // namespace

int SrtOutput::interrupt(void* self) { return static_cast<SrtOutput*>(self)->stopRequested() ? 1 : 0; }

void SrtOutput::open() {
  check(avformat_alloc_output_context2(&fmt_, nullptr, "mpegts", opts_.url.c_str()), "mpegts muxer");
  fmt_->interrupt_callback = {&SrtOutput::interrupt, this};
  AVDictionary* d = nullptr;
  for (auto& [k, v] : opts_.avOptions) av_dict_set(&d, k.c_str(), v.c_str(), 0);
  const int rc = avio_open2(&fmt_->pb, opts_.url.c_str(), AVIO_FLAG_WRITE, &fmt_->interrupt_callback, &d);
  av_dict_free(&d);
  check(rc, ("cannot open " + opts_.url).c_str());
}

void SrtOutput::openEncoders(const OutFrame& f) {
  auto v = openH264Encoder(opts_.width, opts_.height, f.rate.num, f.rate.den, int64_t{opts_.bitrateKbps} * 1000,
                           f.rate.num / f.rate.den, false);
  venc_ = v.ctx;
  vst_ = avformat_new_stream(fmt_, nullptr);
  avcodec_parameters_from_context(vst_->codecpar, venc_);
  vst_->time_base = venc_->time_base;

  const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
  aenc_ = avcodec_alloc_context3(ac);
  aenc_->sample_rate = 48000;
  aenc_->sample_fmt = AV_SAMPLE_FMT_FLTP;
  av_channel_layout_default(&aenc_->ch_layout, 2);
  aenc_->bit_rate = 160000;
  aenc_->time_base = {1, 48000};
  check(avcodec_open2(aenc_, ac, nullptr), "aac encoder");
  ast_ = avformat_new_stream(fmt_, nullptr);
  avcodec_parameters_from_context(ast_->codecpar, aenc_);
  ast_->time_base = aenc_->time_base;

  vframe_ = av_frame_alloc();
  vframe_->format = AV_PIX_FMT_NV12;
  vframe_->width = opts_.width;
  vframe_->height = opts_.height;
  check(av_frame_get_buffer(vframe_, 0), "video frame");
  aframe_ = av_frame_alloc();
  aframe_->format = AV_SAMPLE_FMT_FLTP;
  av_channel_layout_copy(&aframe_->ch_layout, &aenc_->ch_layout);
  aframe_->sample_rate = 48000;
  aframe_->nb_samples = aenc_->frame_size;  // 1024
  check(av_frame_get_buffer(aframe_, 0), "audio frame");
  check(avformat_write_header(fmt_, nullptr), "mpegts header");
  headerWritten_ = true;
}

void SrtOutput::writePacket(AVCodecContext* c, AVStream* st) {
  AVPacket* pkt = av_packet_alloc();
  while (avcodec_receive_packet(c, pkt) >= 0) {
    av_packet_rescale_ts(pkt, c->time_base, st->time_base);
    pkt->stream_index = st->index;
    const int rc = av_interleaved_write_frame(fmt_, pkt);
    if (rc < 0) {
      av_packet_free(&pkt);
      check(rc, "srt write");  // throws → close, offline, retry
    }
  }
  av_packet_free(&pkt);
}

void SrtOutput::deliver(const OutFrame& f) {
  if (!f.bgra || f.bgra->bgra.empty()) return;
  if (!venc_) openEncoders(f);
  if (!sws_ || swsW_ != f.bgra->width || swsH_ != f.bgra->height) {
    sws_ = sws_getCachedContext(sws_, static_cast<int>(f.bgra->width), static_cast<int>(f.bgra->height), AV_PIX_FMT_BGRA,
                                opts_.width, opts_.height, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
    swsW_ = f.bgra->width;
    swsH_ = f.bgra->height;
  }
  check(av_frame_make_writable(vframe_), "video frame writable");
  const uint8_t* src[1] = {f.bgra->bgra.data()};
  const int stride[1] = {static_cast<int>(f.bgra->width * 4)};
  sws_scale(sws_, src, stride, 0, static_cast<int>(f.bgra->height), vframe_->data, vframe_->linesize);
  vframe_->pts = vpts_++;
  check(avcodec_send_frame(venc_, vframe_), "encode video");
  writePacket(venc_, vst_);

  if (f.audio) audioFifo_.insert(audioFifo_.end(), f.audio->begin(), f.audio->end());
  const int n = aframe_->nb_samples;
  while (static_cast<int>(audioFifo_.size()) >= n * 2) {
    check(av_frame_make_writable(aframe_), "audio frame writable");
    auto* l = reinterpret_cast<float*>(aframe_->data[0]);
    auto* r = reinterpret_cast<float*>(aframe_->data[1]);
    for (int i = 0; i < n; ++i) {
      l[i] = audioFifo_[static_cast<size_t>(i) * 2];
      r[i] = audioFifo_[static_cast<size_t>(i) * 2 + 1];
    }
    audioFifo_.erase(audioFifo_.begin(), audioFifo_.begin() + n * 2);
    aframe_->pts = apts_;
    apts_ += n;
    check(avcodec_send_frame(aenc_, aframe_), "encode audio");
    writePacket(aenc_, ast_);
  }
}

void SrtOutput::close() noexcept {
  if (fmt_ && headerWritten_) av_write_trailer(fmt_);
  if (fmt_ && fmt_->pb) avio_closep(&fmt_->pb);
  if (fmt_) avformat_free_context(fmt_);
  fmt_ = nullptr;
  avcodec_free_context(&venc_);
  avcodec_free_context(&aenc_);
  av_frame_free(&vframe_);
  av_frame_free(&aframe_);
  sws_freeContext(sws_);
  sws_ = nullptr;
  swsW_ = swsH_ = 0;
  audioFifo_.clear();
  vpts_ = apts_ = 0;
  headerWritten_ = false;
  vst_ = ast_ = nullptr;
}
}  // namespace bc
```

Note: the base marks the output `live` after the first delivered frame; in listener mode `open()` only returns once a caller connected, so `connecting` means "waiting for the receiver".

- [ ] **Step 6: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "SrtOutput*,srtOutputOptions*,*Preview*"`
Expected: 3 SRT cases + the M1 preview cases pass (the loopback one uses `h264_amf` on the RX 6600).

- [ ] **Step 7: Commit**

```bash
git add src/io src/engine/preview_encoder.cpp tests CMakeLists.txt
git commit -m "feat(io): SRT output (hardware H.264 + AAC in MPEG-TS) with a shared encoder opener"
```

### Task 2: NDI input and NDI source discovery

**Files:**
- Create: `src/io/ndi_input.h`, `src/io/ndi_input.cpp`, `src/io/ndi_discovery.h`, `src/io/ndi_discovery.cpp`
- Modify: `CMakeLists.txt` (both `.cpp` inside the `if(BC_WITH_NDI)` block)
- Test: `tests/ndi_input_test.cpp`

**Interfaces:**
- Consumes: `Input`, `InputSink`, `VideoFrameIn`, `AudioBlock` (M1 Task 6); `TexturePool`, `GpuDevice` (M1 Task 5); `NdiOutput` (M1 Task 8, tests only).
- Produces: `bc::NdiInput(GpuDevice&, TexturePool&, std::string source)` implementing `Input` (BC_WITH_NDI only); `std::vector<std::string> bc::listNdiSources(std::chrono::milliseconds wait = 1500ms)` (BC_WITH_NDI only; full NDI names like `"MACHINE (Name)"`).

Behaviour: `source` matches an NDI name exactly, or as a substring when no exact match exists. The input thread finds the source (waiting up to 2 s per try, state `connecting`), creates a receiver with `NDIlib_recv_color_format_BGRX_BGRA`, and captures with a 200 ms timeout. Video → BGRA8 texture (`fullRange = true`); audio (planar float) → interleaved stereo at 48 kHz (first two channels; mono is duplicated; other sample rates are dropped and reported once as `{code: "ndi-audio-rate"}` while video keeps flowing — ponytail: no resampling). No video for 2 s → `offline` with `{code: "ndi-no-video"}`; frames again → `live`. `stop()` returns within ~250 ms.

- [ ] **Step 1: Write the failing test**

`tests/ndi_input_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_NDI
#include <atomic>
#include <thread>

#include "gpu/device.h"
#include "io/ndi_discovery.h"
#include "io/ndi_input.h"
#include "io/ndi_output.h"

using namespace std::chrono_literals;

namespace {
struct Sink : bc::InputSink {
  std::mutex m;
  int video = 0;
  int64_t audio = 0;
  std::vector<bc::SignalState> states;
  void onVideo(bc::VideoFrameIn) override {
    std::lock_guard l(m);
    ++video;
  }
  void onAudio(bc::AudioBlock a) override {
    std::lock_guard l(m);
    audio += static_cast<int64_t>(a.samples.size()) / 2;
  }
  void onState(bc::SignalState s, std::optional<bc::ErrorInfo>) override {
    std::lock_guard l(m);
    states.push_back(s);
  }
};
}  // namespace

TEST_CASE("NdiInput: receives our own NDI output, loses it, and gets it back" BC_REQUIRES(bc::test::Req::Ndi)) {
  const std::string name = "BreadCamera NdiIn " + std::to_string(GetCurrentProcessId());
  auto sender = std::make_unique<bc::NdiOutput>(name);
  sender->start();
  std::atomic<bool> run{true};
  auto feed = [&](bc::NdiOutput* out) {
    auto img = std::make_shared<bc::CpuImage>();
    img->width = 64;
    img->height = 36;
    img->bgra.assign(64 * 36 * 4, 200);
    auto audio = std::make_shared<std::vector<float>>(1600 * 2, 0.2f);
    for (uint64_t t = 0; run; ++t) {
      bc::OutFrame f;
      f.tick = t;
      f.rate = {30, 1};
      f.bgra = img;
      f.audio = audio;
      out->submit(std::move(f));
      std::this_thread::sleep_for(33ms);
    }
  };
  std::thread feeder(feed, sender.get());

  bool listed = false;
  for (int i = 0; i < 6 && !listed; ++i) {
    for (auto& s : bc::listNdiSources(1000ms)) listed = listed || s.find(name) != std::string::npos;
  }
  CHECK(listed);

  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::NdiInput in(*gpu, pool, name);
  Sink sink;
  in.start(sink);
  std::this_thread::sleep_for(4s);
  {
    std::lock_guard l(sink.m);
    CHECK(sink.video > 30);
    CHECK(sink.audio > 48000);
    CHECK(std::find(sink.states.begin(), sink.states.end(), bc::SignalState::Live) != sink.states.end());
  }
  run = false;
  feeder.join();
  sender->stop();
  std::this_thread::sleep_for(3s);
  {
    std::lock_guard l(sink.m);
    CHECK(sink.states.back() == bc::SignalState::Offline);
  }
  sender = std::make_unique<bc::NdiOutput>(name);
  sender->start();
  run = true;
  std::thread again(feed, sender.get());
  std::this_thread::sleep_for(5s);
  {
    std::lock_guard l(sink.m);
    CHECK(sink.states.back() == bc::SignalState::Live);
  }
  auto t0 = std::chrono::steady_clock::now();
  in.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
  run = false;
  again.join();
  sender->stop();
}
#else
TEST_CASE("NdiInput: receives our own NDI output, loses it, and gets it back" BC_REQUIRES(bc::test::Req::Ndi)) {}
#endif
```

Add it to `bc_tests`. Add the two sources to the `if(BC_WITH_NDI)` block next to `ndi_output.cpp`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "NdiInput*"`
Expected with the NDI SDK: build FAILS (`io/ndi_input.h` missing). Without it: 1 skipped — ledger that and still write the code (it compiles once the SDK is present).

- [ ] **Step 3: Implement discovery**

`src/io/ndi_discovery.h`:
```cpp
#pragma once
#include <chrono>
#include <string>
#include <vector>

namespace bc {
// NDI sources visible on the network, e.g. "STUDIO-PC (vMix - Output 1)".
std::vector<std::string> listNdiSources(std::chrono::milliseconds wait = std::chrono::milliseconds(1500));
}  // namespace bc
```

`src/io/ndi_discovery.cpp`:
```cpp
#include "io/ndi_discovery.h"

#include <Processing.NDI.Lib.h>

#include <stdexcept>

namespace bc {
std::vector<std::string> listNdiSources(std::chrono::milliseconds wait) {
  static const bool ok = NDIlib_initialize();
  if (!ok) throw std::runtime_error("the NDI runtime could not start");
  NDIlib_find_create_t fc{};
  fc.show_local_sources = true;
  NDIlib_find_instance_t find = NDIlib_find_create_v2(&fc);
  if (!find) throw std::runtime_error("cannot start NDI discovery");
  NDIlib_find_wait_for_sources(find, static_cast<uint32_t>(wait.count()));
  uint32_t n = 0;
  const NDIlib_source_t* srcs = NDIlib_find_get_current_sources(find, &n);
  std::vector<std::string> out;
  for (uint32_t i = 0; i < n; ++i) out.emplace_back(srcs[i].p_ndi_name);
  NDIlib_find_destroy(find);
  return out;
}
}  // namespace bc
```

- [ ] **Step 4: Implement the input**

`src/io/ndi_input.h`:
```cpp
#pragma once
#include <atomic>
#include <string>
#include <thread>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

namespace bc {
// NDI receive → BGRA8 frames + 48 kHz stereo audio (spec §4.2).
class NdiInput final : public Input {
 public:
  NdiInput(GpuDevice& gpu, TexturePool& pool, std::string source) : gpu_(gpu), pool_(pool), source_(std::move(source)) {}
  ~NdiInput() override { stop(); }
  void start(InputSink& sink) override;
  void stop() override;

 private:
  void run();
  GpuDevice& gpu_;
  TexturePool& pool_;
  std::string source_;
  InputSink* sink_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};
}  // namespace bc
```

`src/io/ndi_input.cpp`:
```cpp
#include "io/ndi_input.h"

#include <Processing.NDI.Lib.h>

#include <chrono>
#include <cstring>

namespace bc {
using Clock = std::chrono::steady_clock;

void NdiInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
  thread_ = std::thread([this] { run(); });
}

void NdiInput::stop() {
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
}

void NdiInput::run() {
  static const bool ok = NDIlib_initialize();
  if (!ok) {
    sink_->onState(SignalState::Offline, ErrorInfo{"ndi-failed", "the NDI runtime could not start"});
    return;
  }
  sink_->onState(SignalState::Connecting, std::nullopt);
  NDIlib_find_create_t fc{};
  fc.show_local_sources = true;
  NDIlib_find_instance_t find = NDIlib_find_create_v2(&fc);
  NDIlib_recv_instance_t recv = nullptr;
  bool live = false, rateReported = false;
  auto lastVideo = Clock::now();
  std::vector<uint8_t> packed;
  while (!stopping_) {
    if (!recv) {
      NDIlib_find_wait_for_sources(find, 200);
      uint32_t n = 0;
      const NDIlib_source_t* srcs = NDIlib_find_get_current_sources(find, &n);
      const NDIlib_source_t* pick = nullptr;
      for (uint32_t i = 0; i < n; ++i) if (source_ == srcs[i].p_ndi_name) pick = &srcs[i];
      for (uint32_t i = 0; i < n && !pick; ++i) if (std::strstr(srcs[i].p_ndi_name, source_.c_str())) pick = &srcs[i];
      if (!pick) continue;
      NDIlib_recv_create_v3_t rc{};
      rc.source_to_connect_to = *pick;
      rc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
      rc.bandwidth = NDIlib_recv_bandwidth_highest;
      rc.p_ndi_recv_name = "BreadCamera";
      recv = NDIlib_recv_create_v3(&rc);
      lastVideo = Clock::now();
      continue;
    }
    NDIlib_video_frame_v2_t v{};
    NDIlib_audio_frame_v3_t a{};
    switch (NDIlib_recv_capture_v3(recv, &v, &a, nullptr, 200)) {
      case NDIlib_frame_type_video: {
        const uint32_t w = static_cast<uint32_t>(v.xres), h = static_cast<uint32_t>(v.yres);
        VideoFrameIn f;
        f.layout = PixelLayout::BGRA8;
        f.fullRange = true;
        f.ptsUs = v.timestamp == NDIlib_recv_timestamp_undefined ? 0 : v.timestamp / 10;
        f.tex = pool_.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        {
          auto lock = gpu_.lock();
          gpu_.context()->UpdateSubresource(f.tex->tex.Get(), 0, nullptr, v.p_data, static_cast<UINT>(v.line_stride_in_bytes), 0);
        }
        NDIlib_recv_free_video_v2(recv, &v);
        lastVideo = Clock::now();
        if (!live) {
          live = true;
          sink_->onState(SignalState::Live, std::nullopt);
        }
        sink_->onVideo(std::move(f));
        break;
      }
      case NDIlib_frame_type_audio: {
        if (a.sample_rate == 48000 && a.no_channels >= 1) {
          AudioBlock blk;
          blk.samples.resize(static_cast<size_t>(a.no_samples) * 2);
          const auto* base = reinterpret_cast<const uint8_t*>(a.p_data);
          const auto* left = reinterpret_cast<const float*>(base);
          const auto* right = a.no_channels > 1 ? reinterpret_cast<const float*>(base + a.channel_stride_in_bytes) : left;
          for (int i = 0; i < a.no_samples; ++i) {
            blk.samples[static_cast<size_t>(i) * 2] = left[i];
            blk.samples[static_cast<size_t>(i) * 2 + 1] = right[i];
          }
          sink_->onAudio(std::move(blk));
        } else if (!rateReported) {
          rateReported = true;
          sink_->onState(live ? SignalState::Live : SignalState::Connecting,
                         ErrorInfo{"ndi-audio-rate", "NDI audio at " + std::to_string(a.sample_rate) + " Hz is ignored; only 48 kHz is used"});
        }
        NDIlib_recv_free_audio_v3(recv, &a);
        break;
      }
      default:
        break;
    }
    if (live && Clock::now() - lastVideo > std::chrono::seconds(2)) {
      live = false;
      sink_->onState(SignalState::Offline, ErrorInfo{"ndi-no-video", "no video from NDI source \"" + source_ + "\" for 2 s"});
    }
  }
  if (recv) NDIlib_recv_destroy(recv);
  NDIlib_find_destroy(find);
  sink_->onState(SignalState::Offline, std::nullopt);
}
}  // namespace bc
```

Note: `NDIlib_audio_frame_v3_t` FLTP data uses `channel_stride_in_bytes` (NDI SDK v5+ naming). If the installed SDK names it differently, follow the header and ledger it. `p_ndi_recv_name` is `const char*` in the SDK; keep the string literal.

- [ ] **Step 5: Run the test**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "NdiInput*"`
Expected with the SDK: PASS (~15 s). Without: skipped, reason printed.

- [ ] **Step 6: Commit**

```bash
git add src/io tests CMakeLists.txt
git commit -m "feat(io): NDI input with reconnect and NDI source discovery"
```

---

### Task 3: OMT output (libomt) and the SDK fetch script

**Files:**
- Create: `scripts/fetch-sdks.ps1`, `cmake/FindOMT.cmake`, `src/io/omt_output.h`, `src/io/omt_output.cpp`
- Modify: `CMakeLists.txt` (`BC_WITH_OMT`, `bc_copy_runtime` copies `libomt.dll` + `libvmx.dll`), `.gitignore` (`/third_party/`), `tests/support/requires.h/.cpp` (`Req::Omt`)
- Test: `tests/omt_output_test.cpp`

**Interfaces:**
- Consumes: `QueuedOutput`, `OutFrame` (M1 Task 7).
- Produces: `bc::OmtOutput(std::string name)` (BC_WITH_OMT only; `needsCpu() == true`; error code `"omt-failed"`); `Req::Omt`; `scripts/fetch-sdks.ps1` (idempotent; later milestones add entries).

libomt facts (repo `openmediatransport/libomt` @ `bb4b67b`, binaries from `openmediatransport/libomtnet` release `v1.0.0.19`, asset `OpenMediaTransport.Binaries.Release.v1.0.0.19.zip`, folder `Libraries/Winx64/` with `libomt.h`, `libomt.lib`, `libomt.dll`, `libvmx.dll`; MIT): `omt_send_t* omt_send_create(const char* name, OMTQuality)`, `int omt_send(omt_send_t*, OMTMediaFrame*)`, `void omt_send_destroy(omt_send_t*)`; receive side `char** omt_discovery_getaddresses(int*)`, `omt_receive_t* omt_receive_create(const char* address, OMTFrameType, OMTPreferredVideoFormat, OMTReceiveFlags)`, `OMTMediaFrame* omt_receive(omt_receive_t*, OMTFrameType, int timeoutMs)`, `omt_receive_destroy`. `OMTMediaFrame` fields: `Type, Timestamp (100 ns units), Codec, Width, Height, Stride, Flags, FrameRateN, FrameRateD, AspectRatio, ColorSpace, SampleRate, Channels, SamplesPerChannel, Data, DataLength`. Video send codecs include `OMTCodec_BGRA` (alpha honoured with `OMTVideoFlags_Alpha`); audio only `OMTCodec_FPA1` (planar float).

- [ ] **Step 1: Fetch script**

`scripts/fetch-sdks.ps1`:
```powershell
# Downloads pinned third-party binaries into third_party/ (git-ignored). Safe to re-run.
$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot '..\third_party'
New-Item -ItemType Directory -Force $root | Out-Null

function Get-Pinned($name, $url, $sha256, $extract) {
  $dest = Join-Path $root $name
  if (Test-Path (Join-Path $dest '.ok')) { Write-Host "$name already present"; return }
  $zip = Join-Path $env:TEMP "$name.zip"
  Invoke-WebRequest -Uri $url -OutFile $zip
  $actual = (Get-FileHash $zip -Algorithm SHA256).Hash
  if ($sha256 -and $actual -ne $sha256) { throw "$name: SHA-256 mismatch ($actual)" }
  if (-not $sha256) { Write-Warning "$name: no pinned hash yet; actual SHA-256 is $actual - pin it in this script" }
  $tmp = Join-Path $env:TEMP "$name-x"
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
  Expand-Archive $zip $tmp
  New-Item -ItemType Directory -Force $dest | Out-Null
  Copy-Item -Recurse -Force (Join-Path $tmp $extract '*') $dest
  New-Item (Join-Path $dest '.ok') -ItemType File | Out-Null
}

Get-Pinned 'omt' 'https://github.com/openmediatransport/libomtnet/releases/download/v1.0.0.19/OpenMediaTransport.Binaries.Release.v1.0.0.19.zip' '' 'Libraries\Winx64'
```
Run it once (`powershell -NoProfile -ExecutionPolicy Bypass -File scripts/fetch-sdks.ps1`), copy the printed SHA-256 into the `''` argument, run again (expect "already present"; delete `third_party/omt/.ok` and re-run once to confirm the hash check passes). Add `/third_party/` to `.gitignore`.

- [ ] **Step 2: CMake**

`cmake/FindOMT.cmake`:
```cmake
# libomt prebuilt binaries fetched by scripts/fetch-sdks.ps1 (spec §7.2: optional).
set(_omt "${CMAKE_SOURCE_DIR}/third_party/omt")
find_path(OMT_INCLUDE_DIR libomt.h PATHS "${_omt}" NO_DEFAULT_PATH)
find_library(OMT_LIBRARY libomt PATHS "${_omt}" NO_DEFAULT_PATH)
find_file(OMT_DLL libomt.dll PATHS "${_omt}" NO_DEFAULT_PATH)
find_file(OMT_VMX_DLL libvmx.dll PATHS "${_omt}" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OMT DEFAULT_MSG OMT_INCLUDE_DIR OMT_LIBRARY OMT_DLL OMT_VMX_DLL)
if(OMT_FOUND AND NOT TARGET OMT::OMT)
  add_library(OMT::OMT UNKNOWN IMPORTED)
  set_target_properties(OMT::OMT PROPERTIES IMPORTED_LOCATION "${OMT_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${OMT_INCLUDE_DIR}")
endif()
```
In `CMakeLists.txt`, next to the NDI option:
```cmake
find_package(OMT)
option(BC_WITH_OMT "Build the OMT output" ${OMT_FOUND})
if(BC_WITH_OMT AND NOT OMT_FOUND)
  message(WARNING "BC_WITH_OMT=ON but libomt was not found (run scripts/fetch-sdks.ps1); turning it off")
  set(BC_WITH_OMT OFF)
endif()
message(STATUS "BC_WITH_OMT=${BC_WITH_OMT}")
```
after the NDI block of `bc_io`:
```cmake
if(BC_WITH_OMT)
  target_sources(bc_io PRIVATE src/io/omt_output.cpp)
  target_link_libraries(bc_io PUBLIC OMT::OMT)
  target_compile_definitions(bc_io PUBLIC BC_WITH_OMT=1)
else()
  target_compile_definitions(bc_io PUBLIC BC_WITH_OMT=0)
endif()
```
and inside `bc_copy_runtime` add:
```cmake
  if(BC_WITH_OMT)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OMT_DLL}" "${OMT_VMX_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
```
`requires.h/.cpp`: add `Omt` → missing when `BC_WITH_OMT == 0`, reason "libomt not built (run scripts/fetch-sdks.ps1)".

- [ ] **Step 3: Write the failing test**

`tests/omt_output_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_OMT
#include <libomt.h>

#include <atomic>
#include <string>
#include <thread>

#include "io/omt_output.h"

using namespace std::chrono_literals;

TEST_CASE("OmtOutput: an OMT receiver gets our picture with alpha" BC_REQUIRES(bc::test::Req::Omt)) {
  const std::string name = "BreadCamera OMT " + std::to_string(GetCurrentProcessId());
  bc::OmtOutput out(name);
  out.start();
  auto img = std::make_shared<bc::CpuImage>();
  img->width = 64;
  img->height = 36;
  img->bgra.resize(64 * 36 * 4);
  for (size_t i = 0; i < img->bgra.size(); i += 4) {
    img->bgra[i] = 0;
    img->bgra[i + 1] = 0;
    img->bgra[i + 2] = 200;
    img->bgra[i + 3] = 128;
  }
  std::atomic<bool> run{true};
  std::thread feeder([&] {
    for (uint64_t t = 0; run; ++t) {
      bc::OutFrame f;
      f.tick = t;
      f.rate = {30, 1};
      f.bgra = img;
      f.hasAlpha = true;
      out.submit(std::move(f));
      std::this_thread::sleep_for(33ms);
    }
  });
  std::string address;
  for (int i = 0; i < 20 && address.empty(); ++i) {
    int n = 0;
    char** list = omt_discovery_getaddresses(&n);
    for (int k = 0; k < n; ++k) if (std::string(list[k]).find(name) != std::string::npos) address = list[k];
    std::this_thread::sleep_for(250ms);
  }
  REQUIRE_FALSE(address.empty());
  omt_receive_t* rx = omt_receive_create(address.c_str(), OMTFrameType_Video, OMTPreferredVideoFormat_UYVYorBGRA, OMTReceiveFlags_None);
  REQUIRE(rx);
  bool got = false;
  int red = -1, alpha = -1;
  for (int i = 0; i < 50 && !got; ++i) {
    OMTMediaFrame* f = omt_receive(rx, OMTFrameType_Video, 200);
    if (f && f->Codec == OMTCodec_BGRA) {
      auto* p = static_cast<uint8_t*>(f->Data);
      red = p[2];
      alpha = p[3];
      got = true;
    }
  }
  omt_receive_destroy(rx);
  run = false;
  feeder.join();
  out.stop();
  REQUIRE(got);
  CHECK(std::abs(red - 200) <= 10);
  CHECK(std::abs(alpha - 128) <= 10);
}
#else
TEST_CASE("OmtOutput: an OMT receiver gets our picture with alpha" BC_REQUIRES(bc::test::Req::Omt)) {}
#endif
```
Add it to `bc_tests` (and `bc_copy_runtime(bc_tests)` already copies the DLLs).

Run: `scripts/test.ps1 -Filter "OmtOutput*"` — Expected: build FAILS (`io/omt_output.h` missing).

- [ ] **Step 4: Implement the output**

`src/io/omt_output.h`:
```cpp
#pragma once
#include <string>
#include <vector>

#include "io/queued_output.h"

namespace bc {
// Open Media Transport sender (spec §4.3), alpha when the frame has it.
class OmtOutput final : public QueuedOutput {
 public:
  explicit OmtOutput(std::string name) : name_(std::move(name)) {}
  ~OmtOutput() override { stop(); }
  bool needsCpu() const override { return true; }

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "omt-failed"; }

 private:
  std::string name_;
  long long* send_ = nullptr;  // omt_send_t*
  std::vector<float> planar_;
};
}  // namespace bc
```

`src/io/omt_output.cpp`:
```cpp
#include "io/omt_output.h"

#include <libomt.h>

#include <stdexcept>

namespace bc {
void OmtOutput::open() {
  send_ = omt_send_create(name_.c_str(), OMTQuality_Default);
  if (!send_) throw std::runtime_error("cannot create OMT source '" + name_ + "'");
}

void OmtOutput::deliver(const OutFrame& f) {
  const int64_t ts = static_cast<int64_t>(f.tick) * 10'000'000LL * f.rate.den / f.rate.num;
  if (f.bgra && !f.bgra->bgra.empty()) {
    OMTMediaFrame v{};
    v.Type = OMTFrameType_Video;
    v.Timestamp = ts;
    v.Codec = OMTCodec_BGRA;
    v.Width = static_cast<int>(f.bgra->width);
    v.Height = static_cast<int>(f.bgra->height);
    v.Stride = v.Width * 4;
    v.Flags = f.hasAlpha ? OMTVideoFlags_Alpha : OMTVideoFlags_None;
    v.FrameRateN = f.rate.num;
    v.FrameRateD = f.rate.den;
    v.AspectRatio = static_cast<float>(v.Width) / static_cast<float>(v.Height);
    v.ColorSpace = OMTColorSpace_BT709;
    v.Data = const_cast<uint8_t*>(f.bgra->bgra.data());
    v.DataLength = static_cast<int>(f.bgra->bgra.size());
    omt_send(send_, &v);
  }
  if (f.audio && !f.audio->empty()) {
    const int ch = f.channels, n = static_cast<int>(f.audio->size()) / ch;
    planar_.resize(f.audio->size());
    for (int c = 0; c < ch; ++c)
      for (int i = 0; i < n; ++i) planar_[static_cast<size_t>(c) * n + i] = (*f.audio)[static_cast<size_t>(i) * ch + c];
    OMTMediaFrame a{};
    a.Type = OMTFrameType_Audio;
    a.Timestamp = ts;
    a.Codec = OMTCodec_FPA1;
    a.SampleRate = f.sampleRate;
    a.Channels = ch;
    a.SamplesPerChannel = n;
    a.Data = planar_.data();
    a.DataLength = static_cast<int>(planar_.size() * sizeof(float));
    omt_send(send_, &a);
  }
}

void OmtOutput::close() noexcept {
  if (send_) {
    omt_send_destroy(send_);
    send_ = nullptr;
  }
}
}  // namespace bc
```
(`omt_send_t` is `long long` in `libomt.h`, so the member type matches without including the header in ours.)

- [ ] **Step 5: Run the test**

Run: `scripts/test.ps1 -Filter "OmtOutput*"`
Expected: PASS on the build PC (OMT is software). If discovery finds nothing within 5 s on this network setup, ledger it, retry once with `OMTDiscoveryServer.exe` from the release zip running, and report.

- [ ] **Step 6: Commit**

```bash
git add scripts/fetch-sdks.ps1 cmake/FindOMT.cmake src/io tests CMakeLists.txt .gitignore
git commit -m "feat(io): OMT output with alpha via pinned libomt binaries"
```

### Task 4: USB/UVC capture input (Media Foundation)

**Files:**
- Create: `src/io/mf_capture_input.h`, `src/io/mf_capture_input.cpp`
- Modify: `CMakeLists.txt` (`bc_io` links `mf mfplat mfreadwrite mfuuid ole32`), `tests/support/requires.h/.cpp` (`Req::CaptureDevice`)
- Test: `tests/mf_capture_test.cpp`

**Interfaces:**
- Consumes: `Input`, `InputSink`, `VideoFrameIn` (M1 Task 6); `TexturePool`, `GpuDevice` (M1).
- Produces: `bc::CaptureDevice { std::string name; std::string id; }`, `std::vector<CaptureDevice> bc::listCaptureDevices()` (id = MF symbolic link), `bc::MfCaptureInput(GpuDevice&, TexturePool&, std::string device, int width = 0, int height = 0, int fps = 0)` (device = id, or a friendly-name substring; 0 = the device's largest mode at ≥ 25 fps); `Req::CaptureDevice` (missing when `listCaptureDevices()` is empty).

Behaviour: one thread per input; MF is started on it (`MFStartup` once per process, `CoInitializeEx(MTA)` per thread). It opens the device, picks a native mode, asks the source reader for `MFVideoFormat_RGB32` with `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING`, and reads synchronously. Every frame → BGRA8 texture. A device that is missing or fails → `offline` with `{code: "capture-failed", message: "<device name>: …"}`, retry with backoff (unplug/replug recovers). Video only (spec §4.2 lists UVC; capture audio is a follow-up). ponytail: synchronous `ReadSample` — `stop()` waits at most one frame interval, or the device's error, before returning.

- [ ] **Step 1: Write the failing test**

`tests/mf_capture_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <thread>

#include "gpu/device.h"
#include "io/mf_capture_input.h"
#include "support/requires.h"

using namespace std::chrono_literals;

namespace {
struct Sink : bc::InputSink {
  std::mutex m;
  int video = 0;
  std::vector<bc::SignalState> states;
  std::vector<std::optional<bc::ErrorInfo>> errors;
  void onVideo(bc::VideoFrameIn) override {
    std::lock_guard l(m);
    ++video;
  }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState s, std::optional<bc::ErrorInfo> e) override {
    std::lock_guard l(m);
    states.push_back(s);
    errors.push_back(e);
  }
};
}  // namespace

TEST_CASE("listCaptureDevices: runs and returns names with ids") {
  auto list = bc::listCaptureDevices();
  for (auto& d : list) {
    CHECK_FALSE(d.name.empty());
    CHECK_FALSE(d.id.empty());
  }
  MESSAGE(list.size() << " capture device(s)");
}

TEST_CASE("MfCaptureInput: an unknown device is reported and retried, never crashes") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::MfCaptureInput in(*gpu, pool, "No Such Camera 1234");
  Sink sink;
  in.start(sink);
  std::this_thread::sleep_for(1500ms);
  in.stop();
  std::lock_guard l(sink.m);
  REQUIRE(sink.states.size() >= 3);
  CHECK(sink.states[1] == bc::SignalState::Offline);
  REQUIRE(sink.errors[1]);
  CHECK(sink.errors[1]->code == "capture-failed");
  CHECK(sink.errors[1]->message.find("No Such Camera 1234") != std::string::npos);
}

TEST_CASE("MfCaptureInput: the first capture device delivers frames" BC_REQUIRES(bc::test::Req::CaptureDevice)) {
  auto dev = bc::listCaptureDevices().front();
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::MfCaptureInput in(*gpu, pool, dev.id);
  Sink sink;
  in.start(sink);
  std::this_thread::sleep_for(3s);
  in.stop();
  std::lock_guard l(sink.m);
  CHECK(sink.video > 20);
}
```

`requires.cpp`: `CaptureDevice` → `reason = "no USB/UVC capture device on this PC"; return bc::listCaptureDevices().empty();` (include `io/mf_capture_input.h`).

Add the test and the source; link the MF libraries.

- [ ] **Step 2: Run to verify failure**

Run: `scripts/test.ps1 -Filter "listCaptureDevices*,MfCaptureInput*"` — Expected: build FAILS.

- [ ] **Step 3: Implement**

`src/io/mf_capture_input.h`:
```cpp
#pragma once
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

namespace bc {
struct CaptureDevice {
  std::string name;
  std::string id;  // MF symbolic link
};
std::vector<CaptureDevice> listCaptureDevices();

// USB/UVC (and most HDMI-to-USB) capture through Media Foundation (spec §4.2). Video only.
class MfCaptureInput final : public Input {
 public:
  MfCaptureInput(GpuDevice& gpu, TexturePool& pool, std::string device, int width = 0, int height = 0, int fps = 0)
      : gpu_(gpu), pool_(pool), device_(std::move(device)), width_(width), height_(height), fps_(fps) {}
  ~MfCaptureInput() override { stop(); }
  void start(InputSink& sink) override;
  void stop() override;

 private:
  void run();
  void runOnce();
  GpuDevice& gpu_;
  TexturePool& pool_;
  std::string device_;
  int width_, height_, fps_;
  InputSink* sink_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::vector<uint8_t> flip_;
};
}  // namespace bc
```

`src/io/mf_capture_input.cpp`:
```cpp
#include "io/mf_capture_input.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <condition_variable>
#include <mutex>
#include <stdexcept>

#include "core/backoff.h"
#include "core/utf8.h"

using Microsoft::WRL::ComPtr;

namespace bc {
namespace {
void ensureMf() {
  static std::once_flag once;
  std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); });
}

struct ComInit {
  ComInit() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
  ~ComInit() { CoUninitialize(); }
};

std::vector<ComPtr<IMFActivate>> enumerate() {
  ComPtr<IMFAttributes> attrs;
  MFCreateAttributes(&attrs, 1);
  attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate** raw = nullptr;
  UINT32 n = 0;
  std::vector<ComPtr<IMFActivate>> out;
  if (FAILED(MFEnumDeviceSources(attrs.Get(), &raw, &n))) return out;
  for (UINT32 i = 0; i < n; ++i) {
    out.emplace_back(raw[i]);
    raw[i]->Release();
  }
  CoTaskMemFree(raw);
  return out;
}

std::string str(IMFActivate* a, const GUID& key) {
  WCHAR* s = nullptr;
  UINT32 len = 0;
  if (FAILED(a->GetAllocatedString(key, &s, &len))) return {};
  std::string out = toUtf8(s);
  CoTaskMemFree(s);
  return out;
}
}  // namespace

std::vector<CaptureDevice> listCaptureDevices() {
  ensureMf();
  ComInit com;
  std::vector<CaptureDevice> out;
  for (auto& a : enumerate()) {
    out.push_back({str(a.Get(), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME), str(a.Get(), MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK)});
  }
  return out;
}

void MfCaptureInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
  thread_ = std::thread([this] { run(); });
}

void MfCaptureInput::stop() {
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
}

void MfCaptureInput::run() {
  ensureMf();
  ComInit com;
  Backoff backoff;
  std::mutex m;
  std::condition_variable cv;
  while (!stopping_) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    try {
      runOnce();
      if (stopping_) break;
      throw std::runtime_error("the device stopped sending frames");
    } catch (const std::exception& e) {
      if (stopping_) break;
      sink_->onState(SignalState::Offline, ErrorInfo{"capture-failed", device_ + ": " + e.what()});
    }
    std::unique_lock l(m);
    cv.wait_for(l, backoff.next(), [&] { return stopping_.load(); });
  }
  sink_->onState(SignalState::Offline, std::nullopt);
}

void MfCaptureInput::runOnce() {
  ComPtr<IMFActivate> pick;
  for (auto& a : enumerate()) {
    const auto id = str(a.Get(), MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
    const auto name = str(a.Get(), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
    if (id == device_ || (!pick && name.find(device_) != std::string::npos)) pick = a;
  }
  if (!pick) throw std::runtime_error("device not found");
  ComPtr<IMFMediaSource> source;
  if (FAILED(pick->ActivateObject(IID_PPV_ARGS(&source)))) throw std::runtime_error("cannot open the device (in use by another app?)");
  struct Shutdown {
    IMFMediaSource* s;
    ~Shutdown() { s->Shutdown(); }
  } shutdown{source.Get()};
  ComPtr<IMFAttributes> ra;
  MFCreateAttributes(&ra, 1);
  ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  ComPtr<IMFSourceReader> reader;
  if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), ra.Get(), &reader))) throw std::runtime_error("cannot create a source reader");

  // Pick a native mode: the requested size/fps, else the largest at >= 25 fps.
  constexpr DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  ComPtr<IMFMediaType> best;
  UINT64 bestScore = 0;
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> t;
    if (FAILED(reader->GetNativeMediaType(kVideo, i, &t))) break;
    UINT32 w = 0, h = 0, num = 0, den = 1;
    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
    const double fps = den ? static_cast<double>(num) / den : 0;
    UINT64 score = static_cast<UINT64>(w) * h * (fps >= 25 ? 2 : 1);
    if (width_ && height_) score = (static_cast<int>(w) == width_ && static_cast<int>(h) == height_) ? 1'000'000'000ULL + static_cast<UINT64>(fps) : 0;
    if (fps_ && std::abs(fps - fps_) > 1) score /= 4;
    if (score > bestScore) {
      bestScore = score;
      best = t;
    }
  }
  if (!best) throw std::runtime_error("the device offers no video mode");
  reader->SetCurrentMediaType(kVideo, nullptr, best.Get());
  ComPtr<IMFMediaType> rgb;
  MFCreateMediaType(&rgb);
  rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  if (FAILED(reader->SetCurrentMediaType(kVideo, nullptr, rgb.Get()))) throw std::runtime_error("cannot convert the device format to RGB");
  ComPtr<IMFMediaType> actual;
  reader->GetCurrentMediaType(kVideo, &actual);
  UINT32 w = 0, h = 0;
  MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h);
  LONG stride = static_cast<LONG>(w * 4);
  UINT32 s = 0;
  if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) stride = static_cast<LONG>(static_cast<INT32>(s));

  bool live = false;
  while (!stopping_) {
    DWORD flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> sample;
    if (FAILED(reader->ReadSample(kVideo, 0, nullptr, &flags, &ts, &sample))) throw std::runtime_error("read failed");
    if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) throw std::runtime_error("the device was disconnected");
    if (!sample) continue;
    ComPtr<IMFMediaBuffer> buf;
    sample->ConvertToContiguousBuffer(&buf);
    BYTE* data = nullptr;
    DWORD len = 0;
    if (FAILED(buf->Lock(&data, nullptr, &len))) continue;
    VideoFrameIn f;
    f.layout = PixelLayout::BGRA8;
    f.fullRange = true;
    f.ptsUs = ts / 10;
    f.tex = pool_.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    const UINT pitch = static_cast<UINT>(std::abs(stride));
    const BYTE* src = data;
    if (stride < 0) {  // bottom-up RGB32: row y of the picture is at data + (h-1-y)*pitch
      flip_.resize(static_cast<size_t>(pitch) * h);
      for (UINT32 y = 0; y < h; ++y) std::memcpy(&flip_[static_cast<size_t>(y) * pitch], data + static_cast<size_t>(h - 1 - y) * pitch, pitch);
      src = flip_.data();
    }
    {
      auto lock = gpu_.lock();
      gpu_.context()->UpdateSubresource(f.tex->tex.Get(), 0, nullptr, src, pitch, 0);
    }
    buf->Unlock();
    if (!live) {
      live = true;
      sink_->onState(SignalState::Live, std::nullopt);
    }
    sink_->onVideo(std::move(f));
  }
}
}  // namespace bc
```
(`#include <cstring>` for `std::memcpy`.)

- [ ] **Step 4: Run the tests**

Run: `scripts/test.ps1 -Filter "listCaptureDevices*,MfCaptureInput*"`
Expected: 2 pass; the device case passes if a webcam/capture card is present, else is skipped with its reason.

- [ ] **Step 5: Commit**

```bash
git add src/io tests CMakeLists.txt
git commit -m "feat(io): USB/UVC capture input via Media Foundation, with device listing"
```

---

### Task 5: Factory, router validation, discovery commands and UI forms for the new kinds

**Files:**
- Modify: `src/engine/adapter_factory.cpp`, `src/engine/command_router.h/.cpp` (`validateInput`, `validateOutput`, `Effects::listNdiSources`, `Effects::listCaptureDevices`, commands), `src/engine/engine_main.cpp`, `tests/command_router_test.cpp` (`FakeFx` additions + cases), `ui/src/screens/AddCameraDialog.tsx`, `ui/src/screens/AddOutputDialog.tsx`, `ui/src/screens/Multiview.tsx` + `CameraDetail.tsx` (kind labels), `ui/src/i18n.ts`
- Test: `tests/command_router_test.cpp`, `ui/src/screens/AddCameraDialog.test.tsx`, `ui/src/screens/AddOutputDialog.test.tsx`

**Interfaces:**
- Consumes: `SrtOutput`, `srtOutputOptionsFromParams` (Task 1), `NdiInput`, `listNdiSources` (Task 2), `OmtOutput` (Task 3), `MfCaptureInput`, `listCaptureDevices` (Task 4).
- Produces: input kinds `srt` | `ndi` `{source: string 1..256}` | `uvc` `{device: string 1..512, width?: 0|640..3840, height?: 0|360..2160, fps?: 0|15..60}`; output kinds `ndi` | `fullscreen` | `srt` (params per Task 1) | `omt` `{name?: string 1..200}`; commands `listNdiSources` → `["MACHINE (Name)", ...]`, `listCaptureDevices` → `[{name, id}]`; `Effects` gains `virtual nlohmann::json listNdiSources() = 0; virtual nlohmann::json listCaptureDevices() = 0;`. A kind whose SDK is not built is still **valid in the show** (so a show file moves between PCs) but the factory throws "… support is not installed in this build" — the camera/output shows that error (M1 behaviour).

UI copy (vi / en): input kind "Loại nguồn" / "Source type" with "SRT", "NDI", "Thiết bị USB / capture" / "USB / capture device"; "Nguồn NDI" / "NDI source"; "Tìm lại" / "Refresh"; "Thiết bị" / "Device"; note "Đầu vào USB chỉ có hình, không có âm thanh." / "USB input is video only, no audio."; output kinds add "OMT" and "SRT"; "Tên nguồn OMT" / "OMT source name"; "Bitrate (kb/s)"; "Kích thước" / "Size"; errors "Chọn một nguồn NDI" / "Choose an NDI source", "Chọn một thiết bị" / "Choose a device".

- [ ] **Step 1: Write the failing router tests**

In `tests/command_router_test.cpp`, add to `FakeFx`:
```cpp
  json listNdiSources() override { return json::array({"STUDIO (vMix - Output 1)"}); }
  json listCaptureDevices() override { return json::array({{{"name", "Cam Link 4K"}, {"id", "\\\\?\\usb#vid_0fd9"}}}); }
```
and append:
```cpp
TEST_CASE("CommandRouter: new input and output kinds are validated") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto ok = [&](json input) { return call(r, {{"cmd", "addCamera"}, {"args", {{"name", "C"}, {"input", input}}}})["ok"] == true; };
  CHECK(ok({{"kind", "ndi"}, {"params", {{"source", "STUDIO (vMix - Output 1)"}}}}));
  CHECK_FALSE(ok({{"kind", "ndi"}, {"params", {{"source", ""}}}}));
  CHECK(ok({{"kind", "uvc"}, {"params", {{"device", "Cam Link 4K"}}}}));
  CHECK_FALSE(ok({{"kind", "uvc"}, {"params", {{"device", "x"}, {"fps", 240}}}}));
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "C"}, {"input", kSrt}}}})["result"]["id"];
  auto addOut = [&](json o) { return call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", o}}}}); };
  CHECK(addOut({{"kind", "srt"}, {"params", {{"mode", "listener"}, {"port", 9200}}}})["ok"] == true);
  CHECK(addOut({{"kind", "srt"}, {"params", {{"mode", "listener"}, {"port", 0}}}})["error"]["code"] == "invalid");
  CHECK(addOut({{"kind", "srt"}, {"params", {{"mode", "listener"}, {"port", 9200}, {"bitrateKbps", 999999}}}})["error"]["code"] == "invalid");
  CHECK(addOut({{"kind", "omt"}, {"params", {{"name", "Cam C"}}}})["ok"] == true);
  CHECK(addOut({{"kind", "omt"}, {"params", {{"name", ""}}}})["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "listNdiSources"}})["result"][0] == "STUDIO (vMix - Output 1)");
  CHECK(call(r, {{"cmd", "listCaptureDevices"}})["result"][0]["name"] == "Cam Link 4K");
}
```

- [ ] **Step 2: Run to verify failure**

Run: `scripts/test.ps1 -Filter "CommandRouter*"` — Expected: build FAILS (abstract `FakeFx` / unknown kinds).

- [ ] **Step 3: Router, factory, engine wiring**

`command_router.h`: add the two pure virtuals to `Effects`.

`command_router.cpp` — extend the validators:
```cpp
namespace {
std::string strParam(const json& p, const char* key, size_t minLen, size_t maxLen, const char* what) {
  if (!p.contains(key) || !p[key].is_string()) throw std::invalid_argument(std::string(what) + " is required");
  auto s = p[key].get<std::string>();
  if (s.size() < minLen || s.size() > maxLen) throw std::invalid_argument(std::string(what) + " must be " + std::to_string(minLen) + " to " + std::to_string(maxLen) + " characters");
  return s;
}
void optInt(const json& p, const char* key, int lo, int hi, const char* what) {
  if (!p.contains(key)) return;
  if (!p[key].is_number_integer()) throw std::invalid_argument(std::string(what) + " must be a whole number");
  const int v = p[key].get<int>();
  if (v != 0 && (v < lo || v > hi)) throw std::invalid_argument(std::string(what) + " must be 0 (auto) or " + std::to_string(lo) + " to " + std::to_string(hi));
}
}  // namespace

void validateInput(const InputConfig& in) {
  if (in.kind == "srt") {
    srtOptionsFromParams(in.params);
  } else if (in.kind == "ndi") {
    strParam(in.params, "source", 1, 256, "NDI source");
  } else if (in.kind == "uvc") {
    strParam(in.params, "device", 1, 512, "device");
    optInt(in.params, "width", 640, 3840, "width");
    optInt(in.params, "height", 360, 2160, "height");
    optInt(in.params, "fps", 15, 60, "fps");
  } else {
    throw std::invalid_argument("unknown input kind '" + in.kind + "'");
  }
}
```
and in `validateOutput`, before the final `throw`:
```cpp
  if (o.kind == "srt") {
    srtOutputOptionsFromParams(o.params);
    return;
  }
  if (o.kind == "omt") {
    if (o.params.contains("name")) strParam(o.params, "name", 1, 200, "OMT name");
    return;
  }
```
(include `io/srt_params.h` — already included). Add commands next to `listMonitors`:
```cpp
  if (cmd == "listNdiSources") return fx_.listNdiSources();
  if (cmd == "listCaptureDevices") return fx_.listCaptureDevices();
```

`adapter_factory.cpp`:
```cpp
  if (cfg.kind == "ndi") {
#if BC_WITH_NDI
    return std::make_unique<NdiInput>(gpu_, pool_, cfg.params.at("source").get<std::string>());
#else
    throw std::invalid_argument("NDI support is not installed in this build");
#endif
  }
  if (cfg.kind == "uvc") {
    return std::make_unique<MfCaptureInput>(gpu_, pool_, cfg.params.at("device").get<std::string>(), cfg.params.value("width", 0),
                                            cfg.params.value("height", 0), cfg.params.value("fps", 0));
  }
```
(in `makeInput`, before the `throw`) and in `makeOutput`:
```cpp
  if (cfg.kind == "srt") return std::make_unique<SrtOutput>(srtOutputOptionsFromParams(cfg.params));
  if (cfg.kind == "omt") {
#if BC_WITH_OMT
    return std::make_unique<OmtOutput>(cfg.params.value("name", std::string("BreadCamera - ") + cam.name));
#else
    throw std::invalid_argument("OMT support is not installed in this build (run scripts/fetch-sdks.ps1)");
#endif
  }
```
with the matching `#include`s (`io/ndi_input.h` under `#if BC_WITH_NDI`, `io/mf_capture_input.h`, `io/srt_output.h`, `io/omt_output.h` under `#if BC_WITH_OMT`).

`engine_main.cpp` `Fx`:
```cpp
    json listNdiSources() override {
#if BC_WITH_NDI
      return bc::listNdiSources();
#else
      return json::array();
#endif
    }
    json listCaptureDevices() override {
      json out = json::array();
      for (auto& d : bc::listCaptureDevices()) out.push_back({{"name", d.name}, {"id", d.id}});
      return out;
    }
```

Run `scripts/test.ps1 -Filter "CommandRouter*"` — all pass.

- [ ] **Step 4: UI forms**

`ui/src/i18n.ts`: add the keys `'field.inputKind'`, `'input.srt'`, `'input.ndi'`, `'input.uvc'`, `'field.ndiSource'`, `'action.refresh'`, `'field.device'`, `'note.uvcNoAudio'`, `'output.omt'`, `'output.srt'`, `'field.omtName'`, `'field.bitrate'`, `'field.size'`, `'error.ndiSource'`, `'error.device'` with the copy above.

`AddCameraDialog.tsx` — add at the top of the form a kind select (`srt` default), and render per kind:
```tsx
  const [kind, setKind] = useState<'srt' | 'ndi' | 'uvc'>('srt');
  const [ndiSources, setNdiSources] = useState<string[]>([]);
  const [ndiSource, setNdiSource] = useState('');
  const [devices, setDevices] = useState<{ name: string; id: string }[]>([]);
  const [device, setDevice] = useState('');
  const refreshNdi = () => client.request<string[]>('listNdiSources').then(setNdiSources).catch(() => setNdiSources([]));
  const refreshDevices = () => client.request<{ name: string; id: string }[]>('listCaptureDevices').then(setDevices).catch(() => setDevices([]));
  useEffect(() => {
    if (!open) return;
    if (kind === 'ndi') refreshNdi();
    if (kind === 'uvc') refreshDevices();
  }, [open, kind]);
```
Validation: for `ndi` the error is `t('error.ndiSource')` when `ndiSource` is empty; for `uvc` `t('error.device')` when `device` is empty; the SRT checks apply only when `kind === 'srt'`. `submit()` sends `input: { kind: 'ndi', params: { source: ndiSource } }` or `{ kind: 'uvc', params: { device } }` (device = the chosen id) or the existing SRT params. The NDI field is a `TextField select` of `ndiSources` plus a "Tìm lại" button calling `refreshNdi`; the device field is a select of `devices` (label = name, value = id) with the `note.uvcNoAudio` caption under it.

`AddOutputDialog.tsx` — kinds `ndi | omt | srt | fullscreen`; `omt` shows a name field (default `BreadCamera - <camera name>`); `srt` shows mode (caller/listener), host (caller), port, latency, passphrase, bitrate (default 8000) and size (`1920x1080` | `1280x720`), and sends `{mode, host?, port, latencyMs, passphrase?, bitrateKbps, width, height}`.

Kind labels in `Multiview.tsx`/`CameraDetail.tsx`: replace the inline ternaries with a helper `outputLabel(kind)` in `i18n.ts` that maps `ndi|omt|srt|fullscreen` to their keys and falls back to the raw kind.

- [ ] **Step 5: UI tests**

`ui/src/screens/AddCameraDialog.test.tsx`:
```tsx
import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';

import { EngineClient } from '../api/client';
import { FakeWs } from '../test/fakeWs';
import { AddCameraDialog } from './AddCameraDialog';

test('NDI cameras pick a discovered source', async () => {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  render(<AddCameraDialog client={c} open onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Tên'), { target: { value: 'Khách' } });
  fireEvent.mouseDown(screen.getByLabelText('Loại nguồn'));
  fireEvent.click(within(screen.getByRole('listbox')).getByText('NDI'));
  const list = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'listNdiSources');
  FakeWs.last.receive({ id: list.id, ok: true, result: ['STUDIO (vMix - Output 1)'] });
  await waitFor(() => fireEvent.mouseDown(screen.getByLabelText('Nguồn NDI')));
  fireEvent.click(within(screen.getByRole('listbox')).getByText('STUDIO (vMix - Output 1)'));
  fireEvent.click(screen.getByRole('button', { name: 'Thêm camera' }));
  const add = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'addCamera');
  expect(add.args).toEqual({ name: 'Khách', input: { kind: 'ndi', params: { source: 'STUDIO (vMix - Output 1)' } } });
});
```

`ui/src/screens/AddOutputDialog.test.tsx`:
```tsx
import { fireEvent, render, screen, within } from '@testing-library/react';

import { EngineClient } from '../api/client';
import { FakeWs } from '../test/fakeWs';
import { AddOutputDialog } from './AddOutputDialog';

test('an SRT output sends its connection, bitrate and size', () => {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  render(<AddOutputDialog client={c} camera={{ id: 'a', name: 'A' }} onClose={() => {}} />);
  fireEvent.mouseDown(screen.getByLabelText('Loại đầu ra'));
  fireEvent.click(within(screen.getByRole('listbox')).getByText('SRT'));
  fireEvent.mouseDown(screen.getByLabelText('Chế độ'));
  fireEvent.click(within(screen.getByRole('listbox')).getByText(/Listener/));
  fireEvent.change(screen.getByLabelText('Cổng'), { target: { value: '9300' } });
  fireEvent.click(screen.getByRole('button', { name: 'Thêm đầu ra' }));
  const add = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'addOutput');
  expect(add.args.output.kind).toBe('srt');
  expect(add.args.output.params).toMatchObject({ mode: 'listener', port: 9300, bitrateKbps: 8000, width: 1920, height: 1080 });
});
```

Run in `ui/`: `npm test` (all pass), `npm run build`.

- [ ] **Step 6: Commit**

```bash
git add src tests ui/src
git commit -m "feat: NDI/USB inputs and SRT/OMT outputs in the factory, router, discovery and UI"
```

---

### Task 6: M4a acceptance and follow-ups

**Files:**
- Create: `tests/e2e_m4a_test.cpp`
- Modify: `docs/superpowers/follow-ups.md`, `CMakeLists.txt`

- [ ] **Step 1: Write the acceptance test**

`tests/e2e_m4a_test.cpp` — one camera from an SRT test stream (as M1 acceptance), with two real outputs: SRT out (listener, 127.0.0.1:47961) read back by an `FfmpegInput` caller, and OMT out read back by `omt_receive` when `BC_WITH_OMT`:
```cpp
#include <doctest/doctest.h>

#include <thread>

#include "core/state_store.h"
#include "engine/adapter_factory.h"
#include "engine/engine.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "io/ffmpeg_input.h"
#include "io/srt_params.h"
#include "support/requires.h"
#include "support/temp_dir.h"
#include "support/test_media.h"

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
struct Count : bc::InputSink {
  std::atomic<int> video{0};
  void onVideo(bc::VideoFrameIn) override { ++video; }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
};
}  // namespace

TEST_CASE("M4a acceptance: SRT in → engine → SRT out, read back" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  bc::test::TempDir dir;
  bc::test::writeTestTs(dir.path / "in.ts", 1280, 720, 60, 6);
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::Compositor comp(*gpu, pool);
  bc::ColorGrader grader(*gpu, pool);
  bc::StateStore state;
  bc::DefaultAdapterFactory factory(*gpu, pool);
  bc::Engine engine(bc::GpuTools{conv, comp, grader}, state, factory);
  bc::Show show;
  bc::CameraConfig c;
  c.id = "cam";
  c.name = "Cam";
  c.input.kind = "srt";
  c.input.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47960}};
  c.outputs.push_back({"srt-out", "srt", false, true, {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47961}, {"width", 1280}, {"height", 720}}});
  show.cameras.push_back(c);
  engine.apply(show);
  engine.start();
  bc::test::TsPusher push(dir.path / "in.ts", "srt://127.0.0.1:47960?mode=caller", true);
  bc::FfmpegInput reader(*gpu, pool, bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "127.0.0.1"}, {"port", 47961}}));
  Count count;
  reader.start(count);
  std::this_thread::sleep_for(10s);
  reader.stop();
  push.stop();
  engine.stop();
  MESSAGE("frames read back from the SRT output: " << count.video.load());
  CHECK(count.video > 300);
  CHECK(state.snapshot()["cameras"]["cam"]["outputs"]["srt-out"]["state"] == "live");
}
```

Add it to `bc_tests`. Run `scripts/test.ps1 -Filter "M4a acceptance*"` — Expected: PASS (> 300 frames in ~8 s of streaming).

- [ ] **Step 2: Follow-ups**

Append to `docs/superpowers/follow-ups.md`:
```markdown
## M4a
- USB/UVC input is video only; capture audio is not wired (`src/io/mf_capture_input.cpp`).
- USB capture uses synchronous `ReadSample`; `stop()` can wait one frame interval (`src/io/mf_capture_input.cpp`).
- NDI input drops audio that is not 48 kHz, with a visible message (`src/io/ndi_input.cpp`).
- NDI input/discovery are tested only when the NDI SDK is installed on the build PC (`tests/ndi_input_test.cpp`).
- libomt is a prebuilt binary pinned to libomtnet v1.0.0.19 (`scripts/fetch-sdks.ps1`).
```

- [ ] **Step 3: Full verification**

`scripts/test.ps1` (all; record passed/skipped and reasons), `ui/` `npm test` + `npm run build`, and a `BC_FORCE_WARP=1` run.

- [ ] **Step 4: Commit**

```bash
git add tests docs/superpowers/follow-ups.md CMakeLists.txt
git commit -m "test: M4a acceptance (SRT round trip through the engine) and follow-ups"
```
