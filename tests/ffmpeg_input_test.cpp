// DOCTEST_CONFIG_DOUBLE_STRINGIFY: without it doctest's ADL toString picks
// bc::toString(SignalState) (string_view) and string_view + const char* fails to compile.
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

#include <condition_variable>
#include <functional>
#include <mutex>

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
  in.stop();
  REQUIRE(sink.states.size() >= 3);
  CHECK(sink.states[1] == bc::SignalState::Offline);
  REQUIRE(sink.errors[1].has_value());
  CHECK(sink.errors[1]->code == "input-failed");
  CHECK(sink.errors[1]->message.find("srt://127.0.0.1:47903") != std::string::npos);
}
