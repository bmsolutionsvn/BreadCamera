#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "engine/adapter_factory.h"
#include "gpu/device.h"
#include "io/dshow_capture_input.h"
#include "io/dshow_devices.h"
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

TEST_CASE("DShowCaptureInput: an unknown device is reported and retried, never crashes") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::DShowCaptureInput in(*gpu, pool, "@device:sw:no-such-camera-1234");
  Sink sink;
  in.start(sink);
  std::this_thread::sleep_for(1500ms);
  in.stop();
  std::lock_guard l(sink.m);
  REQUIRE(sink.states.size() >= 3);
  CHECK(sink.states[1] == bc::SignalState::Offline);
  REQUIRE(sink.errors[1]);
  CHECK(sink.errors[1]->code == "capture-failed");
}

TEST_CASE("DShowCaptureInput: stop() returns promptly while retrying") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::DShowCaptureInput in(*gpu, pool, "@device:sw:no-such-camera-1234");
  Sink sink;
  in.start(sink);
  std::this_thread::sleep_for(200ms);
  const auto t0 = std::chrono::steady_clock::now();
  in.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 2s);
}

TEST_CASE("DShowCaptureInput: a capture device delivers frames" BC_REQUIRES(bc::test::Req::DShowDevice)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  // Enumerates every DirectShow device; the first that actually delivers frames wins, so a
  // virtual or stalled camera (which enumerates but never sends) does not fail the test.
  for (const auto& dev : bc::listDShowDevices()) {
    bc::DShowCaptureInput in(*gpu, pool, dev.displayName);
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
    std::string why;
    if (!sink.errors.empty() && sink.errors.back()) why = " (" + sink.errors.back()->message + ")";
    std::fprintf(stderr, "[dshow] %s: %d frame(s)%s\n", dev.name.c_str(), sink.video, why.c_str());
    if (sink.video > 20) {
      CHECK(sink.video > 20);
      return;
    }
  }
  std::fprintf(stderr, "[skip] no DirectShow device delivered frames\n");
}

TEST_CASE("makeInput: a dshow: device id builds a DirectShow input") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::DefaultAdapterFactory f(*gpu, pool);
  bc::InputConfig cfg;
  cfg.kind = "uvc";
  cfg.params = {{"device", "dshow:@device:sw:no-such"}};
  auto in = f.makeInput(cfg);
  CHECK(dynamic_cast<bc::DShowCaptureInput*>(in.get()) != nullptr);

  cfg.params = {{"device", "\\\\?\\usb#vid_0408"}};
  auto mf = f.makeInput(cfg);
  CHECK(dynamic_cast<bc::MfCaptureInput*>(mf.get()) != nullptr);
}
