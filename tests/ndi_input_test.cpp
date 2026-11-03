// DOCTEST_CONFIG_DOUBLE_STRINGIFY: without it doctest's ADL toString picks
// bc::toString(SignalState) (string_view) and string_view + const char* fails to compile.
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_NDI
#include <atomic>
#include <thread>
#include <vector>

#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/ndi_discovery.h"
#include "io/ndi_input.h"
#include "io/ndi_output.h"
#include "io/uyvy.h"

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

struct FormatSink : bc::InputSink {
  std::mutex m;
  int uyvy = 0, bgra = 0;
  bc::TextureRef uyvyTex, bgraTex;
  bc::CpuPixel lastCpu = bc::CpuPixel::BGRA;
  void onVideo(bc::VideoFrameIn f) override {
    std::lock_guard l(m);
    if (f.layout == bc::PixelLayout::UYVY) {
      ++uyvy;
      uyvyTex = f.tex;
    } else if (f.layout == bc::PixelLayout::BGRA8) {
      ++bgra;
      bgraTex = f.tex;
    }
  }
  bool wantsCpuVideo() const override { return true; }
  void onCpuVideo(const bc::CpuVideoFrame& cf) override {
    std::lock_guard l(m);
    lastCpu = cf.format;
  }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
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

TEST_CASE("NdiInput: a UYVY source arrives as UYVY, a BGRA one with alpha as BGRA8"
          BC_REQUIRES(bc::test::Req::Ndi)) {
  const uint32_t w = 64, h = 36;
  const uint8_t r = 230, g = 40, b = 20;
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < px.size(); i += 4) {
    px[i] = b;
    px[i + 1] = g;
    px[i + 2] = r;
    px[i + 3] = 128;  // alpha: ask NDI for the BGRA (not UYVY) delivery
  }
  std::vector<uint8_t> uyvy(static_cast<size_t>(w) * h * 2);
  bc::bgraToUyvy(px.data(), static_cast<int>(w), static_cast<int>(h), uyvy.data(), static_cast<int>(w) * 2,
                 false);
  const std::string name = "BreadCamera NdiIn fmt " + std::to_string(GetCurrentProcessId());
  auto sender = std::make_unique<bc::NdiOutput>(name);
  sender->start();
  std::atomic<bool> run{true}, sendUyvy{true};
  std::thread feeder([&] {
    uint64_t t = 0;
    while (run) {
      auto img = std::make_shared<bc::CpuImage>();
      img->width = w;
      img->height = h;
      bc::OutFrame f;
      f.tick = t++;
      f.rate = {30, 1};
      if (sendUyvy) {
        img->bgra = uyvy;
        img->layout = bc::CpuLayout::Uyvy;
        f.uyvy = img;
      } else {
        img->bgra = px;
        f.bgra = img;
        f.hasAlpha = true;
      }
      sender->submit(std::move(f));
      std::this_thread::sleep_for(33ms);
    }
  });
  bool listed = false;
  for (int i = 0; i < 6 && !listed; ++i) {
    for (auto& s : bc::listNdiSources(1000ms)) listed = listed || s.find(name) != std::string::npos;
  }
  REQUIRE(listed);

  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::NdiInput in(*gpu, pool, name);
  FormatSink sink;
  in.start(sink);
  auto waitFor = [&](int FormatSink::*count, int n) {
    for (int i = 0; i < 100; ++i) {
      {
        std::lock_guard l(sink.m);
        if (sink.*count >= n) return true;
      }
      std::this_thread::sleep_for(100ms);
    }
    return false;
  };
  CHECK(waitFor(&FormatSink::uyvy, 5));
  {
    std::lock_guard l(sink.m);
    CHECK(sink.lastCpu == bc::CpuPixel::UYVY);
  }
  sendUyvy = false;
  CHECK(waitFor(&FormatSink::bgra, 5));
  {
    std::lock_guard l(sink.m);
    CHECK(sink.lastCpu == bc::CpuPixel::BGRA);
  }
  in.stop();
  run = false;
  feeder.join();
  sender->stop();

  bc::TextureRef uyvyTex, bgraTex;
  {
    std::lock_guard l(sink.m);
    uyvyTex = sink.uyvyTex;
    bgraTex = sink.bgraTex;
  }
  REQUIRE(uyvyTex);
  REQUIRE(bgraTex);
  bc::Converter conv(*gpu, pool);
  const auto centre = [](const bc::CpuImage& img) {
    return &img.bgra[(static_cast<size_t>(img.height / 2) * img.width + img.width / 2) * 4];
  };
  auto gotUyvy = conv.readback(*conv.toBgra8(*conv.toLinear(*uyvyTex, bc::PixelLayout::UYVY, false), w, h));
  auto gotBgra = conv.readback(*conv.toBgra8(*conv.toLinear(*bgraTex, bc::PixelLayout::BGRA8, true), w, h));
  for (const auto& [img, what] : {std::pair<const bc::CpuImage*, const char*>{&gotUyvy, "UYVY"},
                                  {&gotBgra, "BGRA"}}) {
    INFO(what);
    CHECK(std::abs(centre(*img)[0] - b) <= 6);
    CHECK(std::abs(centre(*img)[1] - g) <= 6);
    CHECK(std::abs(centre(*img)[2] - r) <= 6);
  }
}
#else
TEST_CASE("NdiInput: receives our own NDI output, loses it, and gets it back" BC_REQUIRES(bc::test::Req::Ndi)) {}
#endif
