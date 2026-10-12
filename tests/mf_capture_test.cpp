// DOCTEST_CONFIG_DOUBLE_STRINGIFY: without it doctest's ADL toString picks
// bc::toString(SignalState) (string_view) and string_view + const char* fails to compile.
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

#include <cstdio>
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

TEST_CASE("MfCaptureInput: a capture device delivers frames" BC_REQUIRES(bc::test::Req::CaptureDevice)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  // The first enumerated device is often a virtual camera (an NDI Webcam, a phone's virtual
  // camera) that enumerates but never delivers a frame. Try each device briefly and use the
  // first that actually sends frames; if none does, this PC has no usable capture device.
  const auto devices = bc::listCaptureDevices();
  for (const auto& dev : devices) {
    bc::MfCaptureInput in(*gpu, pool, dev.id);
    Sink sink;
    in.start(sink);
    for (int i = 0; i < 30; ++i) {
      {
        std::lock_guard l(sink.m);
        if (sink.video > 20) break;
      }
      std::this_thread::sleep_for(100ms);
    }
    in.stop();
    std::lock_guard l(sink.m);
    if (sink.video > 20) {
      CHECK(sink.video > 20);
      return;
    }
  }
  std::fprintf(stderr, "[skip] no capture device delivered frames (%zu enumerated, all virtual or stalled)\n", devices.size());
}
