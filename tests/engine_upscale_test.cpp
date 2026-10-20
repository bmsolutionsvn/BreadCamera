#include <doctest/doctest.h>

#include <memory>
#include <thread>
#include <vector>

#include "core/rate.h"
#include "core/stage_params.h"
#include "core/state_store.h"
#include "core/tick_schedule.h"
#include "engine/camera.h"
#include "engine/engine.h"
#include "engine/gpu_tools.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/upscaler.h"
#include "io/input.h"
#include "support/fake_input.h"

using namespace std::chrono_literals;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};
  bc::ColorGrader grader{*gpu, pool};
  bc::Upscaler up{*gpu, pool};
  bc::StateStore state;
  bc::test::FakeFactory factory{conv};
  bc::Engine engine{bc::GpuTools{.conv = conv, .comp = comp, .grader = grader, .up = &up, .gpu = gpu.get()},
                    state, factory};
  ~Rig() { engine.stop(); }
  json at(const std::string& p) {
    auto s = state.snapshot();
    auto ptr = json::json_pointer(p);
    return s.contains(ptr) ? s.at(ptr) : json();
  }
};

bc::CameraConfig cam(const std::string& id, json upscale, json in = json::object()) {
  bc::CameraConfig c;
  c.id = id;
  c.name = id;
  c.input.kind = "fake";
  c.input.params = in;
  c.input.params["tag"] = id;
  c.outputs = {{"o-" + id, "fake", false, true, json::object()}};
  c.stages = {{"upscale", bc::normalizeStageParams("upscale", upscale)}};
  return c;
}

// A solid BGRA frame of one colour, to push straight into a Camera (no input thread).
bc::VideoFrameIn solidFrame(bc::Converter& conv, uint8_t red, uint32_t w, uint32_t h) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (size_t p = 0; p < px.size(); p += 4) {
    px[p + 0] = 0;
    px[p + 1] = 0;
    px[p + 2] = red;
    px[p + 3] = 255;
  }
  return bc::VideoFrameIn{conv.uploadBgra8(px.data(), w, h, w * 4), bc::PixelLayout::BGRA8, true, 0};
}
}  // namespace

TEST_CASE("Camera: Upscale raises a 640x360 input to 1920x1080") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"target", "1080"}}, {{"width", 640}, {"height", 360}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(700ms);
  auto seen = r.factory.output("o-a")->seen();
  REQUIRE_FALSE(seen.empty());
  CHECK(seen.back().width == 1920);
  REQUIRE(r.factory.output("o-a")->lastImage() != nullptr);
  CHECK(r.factory.output("o-a")->lastImage()->height == 1080);
}

TEST_CASE("Camera: Upscale keeps the target size when its stage is bypassed") {
  Rig r;
  bc::StateStore state;
  bc::test::FakeFactory factory{r.conv};
  bc::CameraConfig cfg = cam("a", {{"enabled", true}, {"target", "1080"}});
  cfg.outputs.clear();
  bc::Camera camera(cfg, factory,
                    bc::GpuTools{.conv = r.conv, .comp = r.comp, .grader = r.grader, .up = &r.up, .gpu = r.gpu.get()},
                    state, {});
  bc::TickSchedule sched{bc::Rate{60, 1}};
  auto slate = r.conv.solid(1280, 720, {0, 0, 0, 1});
  auto tick = [&](uint64_t i) {
    camera.onVideo(solidFrame(r.conv, 120, 640, 360));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
  };
  for (uint64_t i = 0; i < 4; ++i) tick(i);
  REQUIRE(camera.lastFrame() != nullptr);
  CHECK(camera.lastFrame()->width == 1920);

  // Force the guard to bypass (as a failure or overrun would): the frame stays at the target size.
  camera.upscaleGuard().failed("forced", Clock::now());
  for (uint64_t i = 4; i < 10; ++i) tick(i);
  REQUIRE(camera.lastFrame() != nullptr);
  CHECK(camera.lastFrame()->width == 1920);
  CHECK(camera.lastFrame()->height == 1080);
  camera.stop();
}

TEST_CASE("Camera: Upscale keeps the target size when the input renegotiates mid-show") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"target", "1080"}}, {{"width", 640}, {"height", 360}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  REQUIRE(r.factory.output("o-a")->seen().back().width == 1920);
  const int opens = r.factory.output("o-a")->opens.load();

  // The webcam renegotiates 640x360 → 960x540 mid-show: the frame stays at 1920x1080.
  r.factory.input("a")->width = 960;
  r.factory.input("a")->height = 540;
  std::this_thread::sleep_for(500ms);
  auto seen = r.factory.output("o-a")->seen();
  CHECK(seen.back().width == 1920);
  REQUIRE(r.factory.output("o-a")->lastImage() != nullptr);
  CHECK(r.factory.output("o-a")->lastImage()->height == 1080);
  CHECK(r.factory.output("o-a")->opens.load() == opens);  // no output re-opens
}

TEST_CASE("Camera: the slate is at the target size while Upscale is on") {
  Rig r;
  bc::StateStore state;
  bc::test::FakeFactory factory{r.conv};
  bc::CameraConfig cfg = cam("a", {{"enabled", true}, {"target", "1080"}});
  cfg.outputs.clear();
  bc::Camera camera(cfg, factory,
                    bc::GpuTools{.conv = r.conv, .comp = r.comp, .grader = r.grader, .up = &r.up, .gpu = r.gpu.get()},
                    state, {});
  bc::TickSchedule sched{bc::Rate{60, 1}};
  auto slateSource = r.conv.solid(320, 180, {0.05f, 0.05f, 0.06f, 1.0f});

  // Before any frame: the target's 16:9 fallback.
  camera.beginTick(Clock::now(), slateSource);
  CHECK(camera.tickFrame()->width == 1920);
  CHECK(camera.tickFrame()->height == 1080);

  // A frame arrives, then the input stops past the hold time: the slate is still the target size.
  camera.onVideo(solidFrame(r.conv, 120, 640, 360));
  const auto t0 = Clock::now();
  camera.beginTick(t0, slateSource);
  camera.endTick(t0, nullptr);
  REQUIRE(camera.lastFrame() != nullptr);
  CHECK(camera.lastFrame()->width == 1920);
  camera.beginTick(t0 + 6s, slateSource);
  camera.endTick(t0 + 6s, nullptr);
  REQUIRE(camera.lastFrame() != nullptr);
  CHECK(camera.lastFrame()->width == 1920);
  CHECK(camera.lastFrame()->height == 1080);
  camera.stop();
}

TEST_CASE("Camera: Upscale to 720p on a 1280x720 input changes nothing and says so") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"target", "720"}}, {{"width", 1280}, {"height", 720}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1300ms);  // at least one 1 Hz status publish
  auto seen = r.factory.output("o-a")->seen();
  REQUIRE_FALSE(seen.empty());
  CHECK(seen.back().width == 1280);
  REQUIRE(r.factory.output("o-a")->lastImage() != nullptr);
  CHECK(r.factory.output("o-a")->lastImage()->height == 720);
  CHECK(r.at("/cameras/a/stages/upscale/note") == "Input is already 720p");
}

// Review Focus 5: a show from an AMD PC that already has Clean up on a camera. tools_.rtx is null
// (no NVIDIA card), so the stage bypasses with a readable reason and the picture still flows.
TEST_CASE("Camera: Clean up without an RTX card is bypassed with a readable error") {
  Rig r;
  bc::StateStore state;
  bc::test::FakeFactory factory{r.conv};
  bc::CameraConfig cfg = cam("a", json::object());  // no Upscale; Clean up only
  cfg.stages["cleanup"] = bc::normalizeStageParams("cleanup", {{"enabled", true}});
  cfg.outputs.clear();
  bc::Camera camera(cfg, factory,
                    bc::GpuTools{.conv = r.conv, .comp = r.comp, .grader = r.grader, .up = &r.up, .gpu = r.gpu.get()},
                    state, {});
  bc::TickSchedule sched{bc::Rate{60, 1}};
  auto slate = r.conv.solid(640, 360, {0, 0, 0, 1});
  for (uint64_t i = 0; i < 3; ++i) {
    camera.onVideo(solidFrame(r.conv, 120, 640, 360));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
  }
  camera.publishStatus();
  auto s = state.snapshot();
  CHECK(s.at(json::json_pointer("/cameras/a/stages/cleanup/state")) == "bypassed");
  CHECK(s.at(json::json_pointer("/cameras/a/stages/cleanup/error/code")) == "needs-rtx");
  // Nothing crashes and frames keep flowing at the input size (Clean up is 1× and never resizes).
  REQUIRE(camera.lastFrame() != nullptr);
  CHECK(camera.lastFrame()->width == 640);
  CHECK(camera.lastFrame()->height == 360);
  camera.stop();
}