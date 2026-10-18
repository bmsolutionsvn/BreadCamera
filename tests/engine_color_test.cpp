#include <doctest/doctest.h>

#include <thread>

#include "core/lut.h"
#include "core/rate.h"
#include "core/stage_params.h"
#include "core/state_store.h"
#include "core/tick_schedule.h"
#include "engine/camera.h"
#include "engine/engine.h"
#include "engine/gpu_tools.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "io/input.h"
#include "support/fake_input.h"
#include "support/temp_dir.h"

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};
  bc::ColorGrader grader{*gpu, pool};
  bc::StateStore state;
  bc::test::FakeFactory factory{conv};
  bc::Engine engine{bc::GpuTools{.conv = conv, .comp = comp, .grader = grader, .gpu = gpu.get()}, state, factory};
  ~Rig() { engine.stop(); }
  json at(const std::string& p) {
    auto s = state.snapshot();
    auto ptr = json::json_pointer(p);
    return s.contains(ptr) ? s.at(ptr) : json();
  }
};

bc::CameraConfig cam(const std::string& id, json color, json in = json::object()) {
  bc::CameraConfig c;
  c.id = id;
  c.name = id;
  c.input.kind = "fake";
  c.input.params = in;
  c.input.params["tag"] = id;
  c.outputs = {{"o-" + id, "fake", false, true, json::object()}};
  c.stages = {{"color", bc::normalizeStageParams("color", color)}};
  return c;
}
}  // namespace

TEST_CASE("Colour stage: exposure brightens the output") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"exposure", 1}}, {{"bgra", {131, 131, 131, 255}}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  auto px = r.factory.output("o-a")->seen().back().firstPixel;
  CHECK(std::abs(px[1] - 174) <= 3);  // 131 ≈ 0.2 linear → ×2 → 174
  CHECK(r.at("/cameras/a/stages/color/state") == "active");
}

TEST_CASE("Colour stage: a missing LUT file reports color-lut and keeps the trims") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"exposure", 1}, {"lut", "C:/nope/missing.cube"}}, {{"bgra", {131, 131, 131, 255}}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1200ms);
  CHECK(r.at("/cameras/a/stages/color/error/code") == "color-lut");
  auto px = r.factory.output("o-a")->seen().back().firstPixel;
  CHECK(std::abs(px[1] - 174) <= 3);
}

TEST_CASE("Colour stage: a LUT file is applied") {
  Rig r;
  bc::test::TempDir dir;
  auto lut = bc::Lut3D::identity(2);
  for (auto& e : lut.data) e = {1, 0, 0};
  bc::writeCube(dir.path / "red.cube", lut, "red");
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"lut", (dir.path / "red.cube").string()}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  auto px = r.factory.output("o-a")->seen().back().firstPixel;
  CHECK(px[2] >= 253);
  CHECK(px[1] <= 2);
}

TEST_CASE("Engine::capture returns frames before stages, and refuses a camera with no picture") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"exposure", 2}}, {{"bgra", {20, 40, 200, 255}}}));
  show.cameras.push_back(cam("b", json::object()));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  auto got = r.engine.capture({"a"}, 10, 2, 256, 144);
  REQUIRE(got["a"].size() == 10);
  CHECK(got["a"][0].width == 256);
  CHECK(std::abs(got["a"][0].bgra[2] - 200) <= 3);  // raw, not the +2 EV grade
  r.factory.input("b")->paused = true;
  std::this_thread::sleep_for(6000ms);  // past the 5 s hold → slate
  try {
    r.engine.capture({"a", "b"});
    FAIL("capture should refuse camera b");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()).find("b") != std::string::npos);
  }
  CHECK_THROWS_AS(r.engine.capture({"zzz"}), std::invalid_argument);
}

TEST_CASE("Camera: the colour stage publishes its GPU time") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"exposure", 1}}, {{"bgra", {131, 131, 131, 255}}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1300ms);
  auto published = r.at("/cameras/a/stages/color/gpuMs");
  REQUIRE(published.is_number());
  CHECK(published.get<double>() >= 0.0);

  // stageGpuMs is the number publishStatus writes: drive one camera by hand and watch it fill.
  std::vector<uint8_t> px(64 * 36 * 4, 131);
  for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
  bc::StateStore state;
  bc::test::FakeFactory factory{r.conv};
  bc::Camera camera(cam("a", {{"enabled", true}, {"exposure", 1}}), factory,
                    bc::GpuTools{.conv = r.conv, .comp = r.comp, .grader = r.grader, .gpu = r.gpu.get()}, state, {});
  bc::TickSchedule sched{bc::Rate{60, 1}};
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  for (int i = 0; i < 60 && camera.stageGpuMs("color") <= 0; ++i) {
    const auto now = std::chrono::steady_clock::now();
    camera.onVideo(bc::VideoFrameIn{r.conv.uploadBgra8(px.data(), 64, 36, 64 * 4), bc::PixelLayout::BGRA8, true, i});
    camera.beginTick(now, slate);
    camera.endTick(static_cast<uint64_t>(i), sched, now, nullptr);
    r.gpu->context()->Flush();
    std::this_thread::sleep_for(2ms);
  }
  CHECK(camera.stageGpuMs("color") > 0);
  camera.publishStatus();
  auto gpuMs = state.snapshot().at(json::json_pointer("/cameras/a/stages/color/gpuMs"));
  REQUIRE(gpuMs.is_number());
  CHECK(gpuMs.get<double>() >= 0.0);  // the published number is stageGpuMs rounded to 0.1
}
