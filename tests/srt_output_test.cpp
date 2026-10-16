// DOCTEST_CONFIG_DOUBLE_STRINGIFY: without it doctest's ADL toString picks
// bc::toString(SignalState) (string_view) and string_view + const char* fails to compile.
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
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
  CHECK_THROWS_AS(bc::srtOutputOptionsFromParams({{"mode", "listener"}, {"port", 9100}, {"bitrateKbps", 999999}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOutputOptionsFromParams({{"mode", "caller"}, {"port", 0}, {"host", "x"}}), std::invalid_argument);
}

TEST_CASE("SrtOutput: a listener output is read by an SRT caller input, picture and sound" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  auto opts = bc::srtOutputOptionsFromParams({{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 49151}});
  bc::SrtOutput out(opts);
  out.start();
  std::this_thread::sleep_for(300ms);
  CHECK(out.status().state == bc::SignalState::Connecting);  // nobody connected yet
  std::atomic<bool> run{true};
  std::thread feeder([&] { feed(out, run, 1280, 720); });  // the engine sends the output's format size

  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::FfmpegInput in(*gpu, pool, bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "127.0.0.1"}, {"port", 49151}}));
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
    CHECK(sink.width == 1280);  // encoded at the size it was given
    CHECK(sink.audio > 24000);
  }
  CHECK(out.status().state == bc::SignalState::Live);
  run = false;
  feeder.join();
  auto t0 = std::chrono::steady_clock::now();
  out.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 2000ms);
}

TEST_CASE("SrtOutput: re-opens when the frame size changes" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  auto opts = bc::srtOutputOptionsFromParams({{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 49153}});
  bc::SrtOutput out(opts);
  out.start();
  std::this_thread::sleep_for(300ms);

  std::atomic<bool> run{true};
  std::atomic<bool> big{false};  // flip the fed frame size mid-show (a source renegotiation)
  std::thread feeder([&] {
    uint64_t tick = 0;
    while (run) {
      const uint32_t w = big ? 1280 : 640;
      const uint32_t h = big ? 720 : 360;
      auto img = std::make_shared<bc::CpuImage>();
      img->width = w;
      img->height = h;
      img->bgra.assign(static_cast<size_t>(w) * h * 4, 128);
      auto audio = std::make_shared<std::vector<float>>(800 * 2, 0.1f);
      bc::OutFrame f;
      f.tick = tick++;
      f.rate = {60, 1};
      f.bgra = img;
      f.audio = audio;
      out.submit(std::move(f));
      std::this_thread::sleep_for(16ms);
    }
  });

  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::FfmpegInput in(*gpu, pool, bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "127.0.0.1"}, {"port", 49153}}));
  Sink sink;
  in.start(sink);

  auto waitForWidth = [&](uint32_t w) {
    for (int i = 0; i < 100; ++i) {
      {
        std::lock_guard l(sink.m);
        if (sink.width == w && sink.video > 20) return;
      }
      std::this_thread::sleep_for(100ms);
    }
  };

  waitForWidth(640);  // the encoder opens from the first frame
  big = true;         // the frame size changes; the encoder must re-open at the new size
  waitForWidth(1280);

  in.stop();
  run = false;
  feeder.join();
  {
    std::lock_guard l(sink.m);
    CHECK(sink.width == 1280);  // re-encoded at the new size, not the first size
  }
  out.stop();
}

TEST_CASE("SrtOutput: stop() returns while a listener waits for a caller") {
  bc::SrtOutput out(bc::srtOutputOptionsFromParams({{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 49152}}));
  out.start();
  std::this_thread::sleep_for(300ms);
  auto t0 = std::chrono::steady_clock::now();
  out.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
}
