#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_VCAM
#include <thread>

#include "gpu/device.h"
#include "io/mf_capture_input.h"
#include "io/vcam_output.h"

using namespace std::chrono_literals;

namespace {
struct Grab : bc::InputSink {
  std::atomic<int> video{0};
  void onVideo(bc::VideoFrameIn) override { ++video; }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
};
}  // namespace

TEST_CASE("VirtualCameraOutput: Windows sees the camera and a capture app reads it" BC_REQUIRES(bc::test::Req::VcamInstalled)) {
  const std::string name = "BreadCamera Test " + std::to_string(GetCurrentProcessId());
  bc::VirtualCameraOutput out(1, name);
  out.start();
  std::atomic<bool> run{true};
  std::thread feeder([&] {
    auto img = std::make_shared<bc::CpuImage>();
    img->width = 1920;
    img->height = 1080;
    img->bgra.assign(img->width * img->height * 4, 160);
    for (uint64_t t = 0; run; ++t) {
      bc::OutFrame f;
      f.tick = t;
      f.rate = {60, 1};
      f.bgra = img;
      out.submit(std::move(f));
      std::this_thread::sleep_for(16ms);
    }
  });
  std::this_thread::sleep_for(2s);
  bool listed = false;
  for (auto& d : bc::listCaptureDevices()) listed = listed || d.name == name;
  CHECK(listed);
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::MfCaptureInput cap(*gpu, pool, name);
  Grab grab;
  cap.start(grab);
  std::this_thread::sleep_for(4s);
  cap.stop();
  run = false;
  feeder.join();
  out.stop();
  CHECK(grab.video > 30);
}
#else
TEST_CASE("VirtualCameraOutput: Windows sees the camera and a capture app reads it" BC_REQUIRES(bc::test::Req::VcamInstalled)) {}
#endif