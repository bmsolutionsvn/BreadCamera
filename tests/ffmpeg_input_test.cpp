// DOCTEST_CONFIG_DOUBLE_STRINGIFY: without it doctest's ADL toString picks
// bc::toString(SignalState) (string_view) and string_view + const char* fails to compile.
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include "gpu/device.h"
#include "io/ffmpeg_input.h"
#include "io/srt_params.h"
#include "support/requires.h"
#include "support/temp_dir.h"
#include "support/test_media.h"

using namespace std::chrono_literals;

namespace {
struct RecordingSink : bc::InputSink {
  std::mutex m;
  std::condition_variable cv;
  std::vector<bc::SignalState> states;
  std::vector<std::optional<bc::ErrorInfo>> errors;
  int video = 0;
  int64_t audioFrames = 0;
  int audioRate = 0;
  std::optional<bc::PixelLayout> layout;
  uint32_t width = 0;

  void onVideo(bc::VideoFrameIn f) override {
    std::lock_guard l(m);
    ++video;
    layout = f.layout;
    width = f.tex->width;
    cv.notify_all();
  }
  void onAudio(bc::AudioBlock a) override {
    std::lock_guard l(m);
    audioFrames += static_cast<int64_t>(a.samples.size()) / a.channels;
    audioRate = a.sampleRate;
  }
  void onState(bc::SignalState s, std::optional<bc::ErrorInfo> e) override {
    std::lock_guard l(m);
    states.push_back(s);
    errors.push_back(std::move(e));
    cv.notify_all();
  }
  bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds t) {
    std::unique_lock l(m);
    return cv.wait_for(l, t, pred);
  }
  int count(bc::SignalState s) {
    return static_cast<int>(std::count(states.begin(), states.end(), s));
  }
};

struct Fixture {
  bc::test::TempDir dir;
  std::filesystem::path ts = dir.path / "clip.ts";
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  Fixture() { bc::test::writeTestTs(ts, 320, 180, 30, 2); }
};

// Remuxes src into dst with the video stream re-labelled H.264 while the payload stays
// MPEG-2. The H.264 decoder opens cleanly but can never produce a frame from it, which is
// the real "decode accepts packets, emits nothing" path this test pins.
void remuxVideoAsH264(const std::filesystem::path& src, const std::filesystem::path& dst) {
  AVFormatContext* in = nullptr;
  REQUIRE(avformat_open_input(&in, src.string().c_str(), nullptr, nullptr) == 0);
  std::unique_ptr<AVFormatContext*, void (*)(AVFormatContext**)> inGuard(&in, &avformat_close_input);
  REQUIRE(avformat_find_stream_info(in, nullptr) >= 0);
  AVFormatContext* out = nullptr;
  REQUIRE(avformat_alloc_output_context2(&out, nullptr, "mpegts", dst.string().c_str()) == 0);
  std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> outGuard(out, &avformat_free_context);
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    AVStream* os = avformat_new_stream(out, nullptr);
    REQUIRE(os != nullptr);
    REQUIRE(avcodec_parameters_copy(os->codecpar, in->streams[i]->codecpar) == 0);
    os->codecpar->codec_tag = 0;
    if (os->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) os->codecpar->codec_id = AV_CODEC_ID_H264;
    os->time_base = in->streams[i]->time_base;
  }
  REQUIRE(avio_open(&out->pb, dst.string().c_str(), AVIO_FLAG_WRITE) == 0);
  REQUIRE(avformat_write_header(out, nullptr) == 0);
  AVPacket* pkt = av_packet_alloc();
  REQUIRE(pkt != nullptr);
  while (av_read_frame(in, pkt) >= 0) {
    av_packet_rescale_ts(pkt, in->streams[pkt->stream_index]->time_base,
                         out->streams[pkt->stream_index]->time_base);
    pkt->pos = -1;
    const int rc = av_interleaved_write_frame(out, pkt);
    av_packet_unref(pkt);
    REQUIRE(rc == 0);
  }
  REQUIRE(av_write_trailer(out) == 0);
  avio_closep(&out->pb);
  av_packet_free(&pkt);
}
}  // namespace

TEST_CASE("FfmpegInput: decodes a file to frames and 48 kHz stereo audio") {
  Fixture f;
  bc::FfmpegInputOptions o;
  o.url = f.ts.string();
  o.reconnect = false;
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] { return sink.count(bc::SignalState::Offline) > 0; }, 15s));
  in.stop();
  CHECK(sink.states.front() == bc::SignalState::Connecting);
  CHECK(sink.count(bc::SignalState::Live) == 1);
  CHECK(sink.video >= 55);
  CHECK(sink.audioFrames >= 80000);
  CHECK(sink.audioRate == 48000);
  CHECK(sink.width == 320);
}

TEST_CASE("FfmpegInput: the software path delivers BGRA8") {
  Fixture f;
  bc::FfmpegInputOptions o;
  o.url = f.ts.string();
  o.reconnect = false;
  o.allowHwDecode = false;
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] { return sink.video > 5; }, 10s));
  in.stop();
  CHECK(sink.layout == bc::PixelLayout::BGRA8);
}

TEST_CASE("FfmpegInput: the hardware path delivers NV12" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Fixture f;
  bc::FfmpegInputOptions o;
  o.url = f.ts.string();
  o.reconnect = false;
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] { return sink.video > 5; }, 10s));
  in.stop();
  CHECK(sink.layout == bc::PixelLayout::NV12);
}

TEST_CASE("FfmpegInput: an SRT listener receives a caller") {
  Fixture f;
  auto o = bc::srtOptionsFromParams({{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47901}});
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  bc::test::TsPusher pusher(f.ts, "srt://127.0.0.1:47901?mode=caller", true);
  CHECK(sink.waitFor([&] { return sink.video >= 30; }, 15s));
  auto t0 = std::chrono::steady_clock::now();
  in.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
  pusher.stop();
  CHECK_FALSE(pusher.failed());
}

TEST_CASE("FfmpegInput: stop() returns promptly while a listener waits for a caller") {
  Fixture f;
  auto o = bc::srtOptionsFromParams({{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47902}});
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  std::this_thread::sleep_for(300ms);
  auto t0 = std::chrono::steady_clock::now();
  in.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
  CHECK(sink.states == std::vector{bc::SignalState::Connecting, bc::SignalState::Offline});
}

TEST_CASE("FfmpegInput: a dead caller target reports Offline with an error, then retries") {
  Fixture f;
  auto o = bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "127.0.0.1"}, {"port", 47903}});
  o.avOptions["connect_timeout"] = "1000";
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  CHECK(sink.waitFor([&] { return sink.count(bc::SignalState::Connecting) >= 2; }, 10s));
  auto t0 = std::chrono::steady_clock::now();
  in.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
  REQUIRE(sink.states.size() >= 3);
  CHECK(sink.states[1] == bc::SignalState::Offline);
  REQUIRE(sink.errors[1].has_value());
  CHECK(sink.errors[1]->code == "input-failed");
  CHECK(sink.errors[1]->message.find("srt://127.0.0.1:47903") != std::string::npos);
}

TEST_CASE("FfmpegInput: a video stream that never decodes reports Offline with an error and retries") {
  Fixture f;
  const auto bad = f.dir.path / "bad-codec.ts";
  remuxVideoAsH264(f.ts, bad);
  bc::FfmpegInputOptions o;
  o.url = bad.string();
  o.allowHwDecode = false;  // software decode keeps the trigger independent of the GPU driver
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] {
    return sink.count(bc::SignalState::Offline) > 0 && sink.count(bc::SignalState::Connecting) >= 2;
  }, 20s));
  in.stop();
  REQUIRE(sink.errors.size() >= 2);
  REQUIRE(sink.errors[1].has_value());
  CHECK(sink.errors[1]->code == "input-failed");
  CHECK(sink.errors[1]->message.find("video decode failed") != std::string::npos);
  CHECK(sink.errors[1]->message.find(bad.filename().string()) != std::string::npos);
  CHECK(sink.count(bc::SignalState::Live) == 0);  // no frame was ever produced
}
