// DOCTEST_CONFIG_DOUBLE_STRINGIFY: without it doctest's ADL toString picks
// bc::toString(SignalState) (string_view) and string_view + const char* fails to compile.
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
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