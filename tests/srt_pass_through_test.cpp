// DOCTEST_CONFIG_DOUBLE_STRINGIFY: without it doctest's ADL toString picks
// bc::toString(SignalState) (string_view) and string_view + const char* fails to compile.
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <srt/srt.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/state_store.h"
#include "engine/adapter_factory.h"
#include "engine/camera.h"
#include "engine/gpu_tools.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/ffmpeg_input.h"
#include "io/srt_params.h"
#include "support/requires.h"
#include "support/temp_dir.h"
#include "support/test_media.h"

using namespace std::chrono_literals;

namespace {
int64_t nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// A raw FFmpeg reader on the output's SRT URL: packets, not decoded pictures, so the test can
// compare sizes and timestamps with the source.
struct SrtReader {
  AVFormatContext* fmt = nullptr;
  std::atomic<int64_t> deadline{0};
  static int interrupt(void* self) {
    auto* r = static_cast<SrtReader*>(self);
    return nowMs() > r->deadline.load() ? 1 : 0;
  }
  bool open(const std::string& url, std::chrono::milliseconds limit) {
    deadline = nowMs() + limit.count();
    AVFormatContext* f = avformat_alloc_context();
    f->interrupt_callback = {&SrtReader::interrupt, this};
    AVDictionary* d = nullptr;
    av_dict_set(&d, "mode", "caller", 0);
    av_dict_set(&d, "latency", "200000", 0);
    const int rc = avformat_open_input(&f, url.c_str(), nullptr, &d);
    av_dict_free(&d);
    if (rc < 0) {
      char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
      av_strerror(rc, buf, sizeof(buf));
      std::fprintf(stderr, "srt open %s failed: %s (%s)\n", url.c_str(), buf, srt_getlasterror_str());
      return false;
    }
    fmt = f;
    return avformat_find_stream_info(fmt, nullptr) >= 0;
  }
  bool openRetry(const std::string& url, int attempts, std::chrono::milliseconds limit) {
    for (int i = 0; i < attempts; ++i)
      if (open(url, limit)) return true;
    return false;
  }
  int read(AVPacket* p, std::chrono::milliseconds limit) {
    deadline = nowMs() + limit.count();
    return av_read_frame(fmt, p);
  }
  void close() {
    if (fmt) avformat_close_input(&fmt);
  }
  ~SrtReader() { close(); }
};

// The source file's video packets, in file order: the sizes the output must reproduce.
struct SourcePackets {
  std::vector<int> sizes;
  std::vector<bool> key;
};
SourcePackets readSource(const std::filesystem::path& file) {
  SourcePackets out;
  AVFormatContext* f = nullptr;
  REQUIRE(avformat_open_input(&f, file.string().c_str(), nullptr, nullptr) == 0);
  REQUIRE(avformat_find_stream_info(f, nullptr) >= 0);
  const int vi = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  REQUIRE(vi >= 0);
  AVPacket* p = av_packet_alloc();
  REQUIRE(p != nullptr);
  while (av_read_frame(f, p) >= 0) {
    if (p->stream_index == vi) {
      out.sizes.push_back(p->size);
      out.key.push_back((p->flags & AV_PKT_FLAG_KEY) != 0);
    }
    av_packet_unref(p);
  }
  av_packet_free(&p);
  avformat_close_input(&f);
  return out;
}

struct Rig {
  bc::test::TempDir dir;
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};
  bc::ColorGrader grader{*gpu, pool};
  bc::StateStore state;
};
}  // namespace

namespace {
// Builds a camera with one SRT input and one SRT output, both listeners on the loopback.
struct CameraSpec {
  int inPort;
  int outPort;
  bc::CameraConfig cfg;
};
CameraSpec cameraSpec(int inPort, int outPort) {
  CameraSpec s;
  s.inPort = inPort;
  s.outPort = outPort;
  s.cfg.id = "a";
  s.cfg.name = "Cam a";
  s.cfg.input.kind = "srt";
  s.cfg.input.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", inPort}};
  bc::OutputConfig o;
  o.id = "o1";
  o.kind = "srt";
  o.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", outPort}};
  s.cfg.outputs = {o};
  return s;
}
// A factory that remembers the FfmpegInput the camera built, so the test can read its decode count.
class CapturingFactory final : public bc::AdapterFactory {
 public:
  CapturingFactory(bc::GpuDevice& gpu, bc::TexturePool& pool) : real_(gpu, pool) {}
  std::unique_ptr<bc::Input> makeInput(const bc::InputConfig& cfg) override {
    auto in = real_.makeInput(cfg);
    input_ = dynamic_cast<bc::FfmpegInput*>(in.get());
    return in;
  }
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig& cfg, const bc::CameraConfig& cam,
                                         bc::Rate rate) override {
    return real_.makeOutput(cfg, cam, rate);
  }
  bc::FfmpegInput* input() const { return input_; }

 private:
  bc::DefaultAdapterFactory real_;
  bc::FfmpegInput* input_ = nullptr;
};

struct FrameSink final : bc::InputSink {
  std::mutex m;
  std::condition_variable cv;
  int video = 0;
  int64_t audio = 0;
  uint32_t width = 0;
  void onVideo(bc::VideoFrameIn f) override {
    std::lock_guard l(m);
    ++video;
    width = f.tex->width;
    cv.notify_all();
  }
  void onAudio(bc::AudioBlock a) override {
    std::lock_guard l(m);
    audio += static_cast<int64_t>(a.samples.size()) / a.channels;
  }
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
  int count() {
    std::lock_guard l(m);
    return video;
  }
  bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds t) {
    std::unique_lock l(m);
    return cv.wait_for(l, t, pred);
  }
};
}  // namespace

TEST_CASE("SRT pass-through: packets reach the output unchanged" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  Rig rig;
  const auto file = rig.dir.path / "source.ts";
  bc::test::writeH264TestTs(file, 640, 360, 30, 6, 15);
  const auto source = readSource(file);
  REQUIRE(source.sizes.size() > 30);

  CameraSpec spec = cameraSpec(49154, 49155);
  CapturingFactory factory(*rig.gpu, rig.pool);
  bc::Camera cam(spec.cfg, factory, bc::GpuTools{rig.conv, rig.comp, rig.grader}, rig.state, bc::SyncPolicy{});
  cam.setPreviewQuery([] { return false; });
  cam.start();
  bc::test::TsPusher pusher(file, "srt://127.0.0.1:49154?mode=caller", false);

  SrtReader reader;
  REQUIRE(reader.openRetry("srt://127.0.0.1:49155?mode=caller", 3, 10s));
  const int vi = av_find_best_stream(reader.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  REQUIRE(vi >= 0);
  CHECK(reader.fmt->streams[vi]->codecpar->codec_id == AV_CODEC_ID_H264);
  CHECK(reader.fmt->streams[vi]->codecpar->width == 640);
  CHECK(reader.fmt->streams[vi]->codecpar->height == 360);

  std::vector<int> got;
  AVPacket* p = av_packet_alloc();
  REQUIRE(p != nullptr);
  const auto until = std::chrono::steady_clock::now() + 6s;
  bool firstKey = false;
  while (got.size() < 30 && std::chrono::steady_clock::now() < until) {
    if (reader.read(p, 2s) < 0) break;
    if (p->stream_index == vi) {
      if (got.empty()) firstKey = (p->flags & AV_PKT_FLAG_KEY) != 0;
      got.push_back(p->size);
    }
    av_packet_unref(p);
  }
  av_packet_free(&p);
  REQUIRE(got.size() >= 20);
  CHECK(firstKey);  // nothing was written before the first keyframe

  // One pass of the source: the output's video packet sizes are the source's, in order. The
  // camera's stream-info probe may consume the source's first packets, so the output can start
  // at a later keyframe; find where it starts, then require the whole run to match.
  const size_t n = source.sizes.size();
  size_t k = n;
  for (size_t i = 0; i < n && i + got.size() <= n; ++i) {
    if (!source.key[i]) continue;
    bool same = true;
    for (size_t j = 0; j < got.size() && same; ++j) same = source.sizes[i + j] == got[j];
    if (same) {
      k = i;
      break;
    }
  }
  REQUIRE(k < n);

  CHECK(factory.input() != nullptr);
  CHECK(factory.input()->videoFrames() == 0);  // nothing was decoded: the packets were remuxed
  reader.close();
  pusher.stop();
  cam.stop();
}

TEST_CASE("SRT pass-through: audio and video stay together" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  Rig rig;
  const auto file = rig.dir.path / "source.ts";
  bc::test::writeH264TestTs(file, 320, 180, 30, 6, 15);

  CameraSpec spec = cameraSpec(49156, 49157);
  CapturingFactory factory(*rig.gpu, rig.pool);
  bc::Camera cam(spec.cfg, factory, bc::GpuTools{rig.conv, rig.comp, rig.grader}, rig.state, bc::SyncPolicy{});
  cam.setPreviewQuery([] { return false; });
  cam.start();
  bc::test::TsPusher pusher(file, "srt://127.0.0.1:49156?mode=caller", false);

  SrtReader reader;
  REQUIRE(reader.openRetry("srt://127.0.0.1:49157?mode=caller", 3, 10s));
  const int vi = av_find_best_stream(reader.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  const int ai = av_find_best_stream(reader.fmt, AVMEDIA_TYPE_AUDIO, -1, vi, nullptr, 0);
  REQUIRE(vi >= 0);
  REQUIRE(ai >= 0);

  int video = 0, audio = 0;
  int64_t firstV = INT64_MIN, firstA = INT64_MIN, lastV = INT64_MIN, lastA = INT64_MIN;
  bool monoV = true, monoA = true;
  AVPacket* p = av_packet_alloc();
  REQUIRE(p != nullptr);
  const auto until = std::chrono::steady_clock::now() + 6s;
  while ((video < 20 || audio < 20) && std::chrono::steady_clock::now() < until) {
    if (reader.read(p, 2s) < 0) break;
    const int64_t us = av_rescale_q(p->pts, reader.fmt->streams[p->stream_index]->time_base, {1, 1'000'000});
    if (p->stream_index == vi) {
      if (firstV == INT64_MIN) firstV = us;
      if (lastV != INT64_MIN && us < lastV) monoV = false;
      lastV = us;
      ++video;
    } else if (p->stream_index == ai) {
      if (firstA == INT64_MIN) firstA = us;
      if (lastA != INT64_MIN && us < lastA) monoA = false;
      lastA = us;
      ++audio;
    }
    av_packet_unref(p);
  }
  av_packet_free(&p);
  REQUIRE(video >= 20);
  REQUIRE(audio >= 20);  // the input's audio packets were remuxed with the video
  CHECK(std::llabs(firstA - firstV) <= 100'000);
  CHECK(monoV);
  CHECK(monoA);
  reader.close();
  pusher.stop();
  cam.stop();
}

TEST_CASE("SRT pass-through: an effect turned on goes back to encoding" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  Rig rig;
  const auto file = rig.dir.path / "source.ts";
  bc::test::writeH264TestTs(file, 320, 180, 30, 4, 15);

  CameraSpec spec = cameraSpec(49158, 49159);
  CapturingFactory factory(*rig.gpu, rig.pool);
  bc::Camera cam(spec.cfg, factory, bc::GpuTools{rig.conv, rig.comp, rig.grader}, rig.state, bc::SyncPolicy{});
  cam.setPreviewQuery([] { return false; });
  cam.start();
  bc::test::TsPusher pusher(file, "srt://127.0.0.1:49158?mode=caller", true);

  // The reader decodes, so it proves the switched picture is decodable; it reconnects by itself
  // when the output reopens its muxer on the switch.
  bc::FfmpegInput reader(*rig.gpu, rig.pool, bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "127.0.0.1"}, {"port", 49159}}));
  FrameSink sink;
  reader.start(sink);

  std::atomic<bool> ticking{true};
  std::thread ticker([&] {
    auto slate = rig.conv.solid(640, 360, {0, 0, 0, 1});
    while (ticking) {
      const auto now = std::chrono::steady_clock::now();
      cam.beginTick(now, slate);
      cam.endTick(now, nullptr);
      std::this_thread::sleep_for(15ms);
    }
  });
  REQUIRE(sink.waitFor([&] { return sink.video >= 10; }, 10s));
  CHECK(factory.input()->videoFrames() == 0);  // still re-muxing before the switch

  CameraSpec next = spec;
  next.cfg.stages["color"] = {{"enabled", true}};
  const int before = sink.count();
  cam.reconfigure(next.cfg);
  CHECK(sink.waitFor([&] { return sink.video >= before + 10; }, 10s));  // decodable pictures resumed

  ticking = false;
  ticker.join();
  reader.stop();
  pusher.stop();
  cam.stop();
}
