#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "support/requires.h"

#if BC_WITH_OMT
#include <windows.h>

#include "io/omt_input.h"
#include "omthx/omthx.h"

using namespace std::chrono_literals;

namespace {
struct CountingSink : bc::InputSink {
  std::atomic<int> videos{0};
  std::atomic<int> audios{0};
  std::atomic<uint32_t> width{0};
  std::atomic<uint32_t> height{0};
  std::atomic<bool> live{false};

  void onVideo(bc::VideoFrameIn f) override {
    width = f.tex ? f.tex->width : 0;
    height = f.tex ? f.tex->height : 0;
    ++videos;
  }
  void onAudio(bc::AudioBlock) override { ++audios; }
  void onState(bc::SignalState state, std::optional<bc::ErrorInfo>) override {
    if (state == bc::SignalState::Live) live = true;
  }
};
}  // namespace

TEST_CASE("OmtInput: receives an OMT HX source as pictures and audio" BC_REQUIRES(bc::test::Req::Omt)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  if (!gpu) return;
  bc::TexturePool pool{*gpu};

  const std::string name = "BreadCamera OMT input " + std::to_string(GetCurrentProcessId());
  omthx::Sender sender(name, omthx::Mode::Hx, omthx::Quality::Medium);

  omthx::VideoFormat format;
  format.width = 320;
  format.height = 180;
  format.fpsN = 30;
  format.fpsD = 1;
  const std::vector<uint8_t> bgra(static_cast<size_t>(320) * 180 * 4, 100);
  const std::vector<float> tone(1600 * 2, 0.01f);

  std::atomic<bool> run{true};
  std::thread feeder([&] {
    int64_t timestamp = 0;
    while (run) {
      sender.sendVideo(format, bgra, false, timestamp);
      sender.sendAudio(tone, 48000, 2, timestamp);
      timestamp += 333333;  // 30 fps in 100 ns units
      std::this_thread::sleep_for(33ms);
    }
  });

  const std::string address = sender.address();
  REQUIRE_FALSE(address.empty());

  CountingSink sink;
  bc::OmtInput input(*gpu, pool, address);
  input.start(sink);
  for (int i = 0; i < 100 && sink.videos.load() == 0; ++i) std::this_thread::sleep_for(50ms);
  input.stop();
  run = false;
  if (feeder.joinable()) feeder.join();

  CHECK(sink.videos.load() > 0);
  CHECK(sink.width.load() == 320);
  CHECK(sink.height.load() == 180);
  CHECK(sink.live.load());
}
#endif