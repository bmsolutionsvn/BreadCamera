#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <thread>
#include <vector>

#include "core/state_store.h"
#include "engine/adapter_factory.h"
#include "engine/engine.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "support/requires.h"

#if BC_WITH_OMT
#include <windows.h>

#include <psapi.h>

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

namespace {
struct CodedSink : bc::InputSink {
  std::atomic<int> coded{0};
  std::atomic<int> wrong{0};
  std::atomic<int> pictures{0};

  void onVideo(bc::VideoFrameIn) override { ++pictures; }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
  bool wantsCodedVmx() const override { return true; }
  bool wantsGpuVideo() const override { return false; }
  void onCodedVideo(const bc::CodedVideoFrame& f) override {
    ++coded;
    if (f.codec != "vmx" || f.data.empty() || f.width != 320 || f.height != 180) ++wrong;
  }
};
}  // namespace

TEST_CASE("OmtInput: a coded sink gets VMX bytes" BC_REQUIRES(bc::test::Req::Omt)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  if (!gpu) return;
  bc::TexturePool pool{*gpu};

  const std::string name = "BreadCamera OMT coded " + std::to_string(GetCurrentProcessId());
  omthx::Sender sender(name, omthx::Mode::Full, omthx::Quality::Medium);

  omthx::VideoFormat format;
  format.width = 320;
  format.height = 180;
  format.fpsN = 30;
  format.fpsD = 1;
  const std::vector<uint8_t> bgra(static_cast<size_t>(320) * 180 * 4, 100);

  std::atomic<bool> run{true};
  std::thread feeder([&] {
    int64_t timestamp = 0;
    while (run) {
      sender.sendVideo(format, bgra, false, timestamp);
      timestamp += 333333;  // 30 fps in 100 ns units
      std::this_thread::sleep_for(33ms);
    }
  });

  CodedSink sink;
  bc::OmtInput input(*gpu, pool, sender.address());
  input.start(sink);
  for (int i = 0; i < 140 && sink.coded.load() < 20; ++i) std::this_thread::sleep_for(50ms);
  input.stop();
  run = false;
  if (feeder.joinable()) feeder.join();

  CHECK(sink.coded.load() >= 20);
  CHECK(sink.wrong.load() == 0);
  CHECK(sink.pictures.load() == 0);
}

namespace {
uint64_t privateBytes() {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                            sizeof counters)) {
    return 0;
  }
  return counters.PrivateUsage;
}
}  // namespace

// The audit's critical finding: every OMT input kept about one BGRA picture per received frame.
// 300 frames of 1080p through the real Camera path, and the process must not grow per frame.
TEST_CASE("OmtInput: memory stays flat while a camera receives 300 frames" BC_REQUIRES(bc::test::Req::Omt)
          BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};
  bc::ColorGrader grader{*gpu, pool};
  bc::StateStore state;
  bc::DefaultAdapterFactory factory{*gpu, pool};
  bc::Engine engine{bc::GpuTools{conv, comp, grader}, state, factory};

  std::atomic<int> frames{0};
  engine.setFrameTap([&](uint64_t, const std::vector<bc::FrameTap>& tapped) {
    for (const auto& f : tapped) {
      if (f.id == "a") ++frames;
    }
  });

  omthx::Sender sender("bc-leak-test", omthx::Mode::Full, omthx::Quality::Medium);
  omthx::VideoFormat format;
  format.width = 1920;
  format.height = 1080;
  format.fpsN = 30;
  format.fpsD = 1;
  const std::vector<uint8_t> bgra(static_cast<size_t>(1920) * 1080 * 4, 100);

  std::atomic<bool> run{true};
  std::thread feeder([&] {
    int64_t timestamp = 0;
    while (run) {
      sender.sendVideo(format, bgra, false, timestamp);
      timestamp += 333333;  // 30 fps in 100 ns units
      std::this_thread::sleep_for(33ms);
    }
  });

  bc::Show show;
  bc::CameraConfig camera;
  camera.id = "a";
  camera.name = "Cam a";
  camera.input.kind = "omt";
  camera.input.params = {{"source", sender.address()}};
  show.cameras.push_back(camera);
  engine.apply(show);
  engine.start();

  auto waitFrames = [&](int want) {
    for (int i = 0; i < 2000 && frames.load() < want; ++i) std::this_thread::sleep_for(20ms);  // 40 s
  };
  waitFrames(60);
  const int64_t start = static_cast<int64_t>(privateBytes());
  waitFrames(300);
  const int64_t end = static_cast<int64_t>(privateBytes());
  const int64_t grown = end - start;
  const int taken = frames.load();

  // The fix moves every picture from VRAM to system memory (sampled through an SRV): the frame the
  // real pipeline captures must still be the colour the source sent. Captured before stop(), while
  // the camera is live.
  const auto captured = engine.capture({"a"}, 1, 1, 64, 36, std::chrono::milliseconds(5000));

  engine.stop();
  run = false;
  if (feeder.joinable()) feeder.join();

  INFO("frames " << taken << ", start " << start << " bytes, grown " << grown << " bytes");
  REQUIRE(taken >= 300);
  REQUIRE(start > 0);
  REQUIRE(end > 0);
  CHECK(grown < 150LL * 1024 * 1024);

  REQUIRE(captured.count("a") == 1);
  REQUIRE(captured.at("a").size() == 1);
  const bc::CpuImage& pic = captured.at("a").front();
  REQUIRE(pic.width == 64);
  REQUIRE(pic.height == 36);
  const size_t centre = (static_cast<size_t>(pic.height / 2) * pic.width + pic.width / 2) * 4;
  // Colour channels: the source was sent as 100 on every channel. Plain OMT carries no alpha
  // (VideoFormat::alpha is false), so libomt returns the picture opaque; the sender's 100 alpha
  // never reaches the wire.
  for (size_t c = 0; c < 3; ++c) CHECK(std::abs(static_cast<int>(pic.bgra[centre + c]) - 100) <= 8);
  CHECK(pic.bgra[centre + 3] == 255);
}

#endif
