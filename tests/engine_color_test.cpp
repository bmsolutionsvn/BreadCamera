#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
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

namespace {
// A solid BGRA frame of one colour, to push straight into a Camera (no input thread).
bc::VideoFrameIn solidFrame(bc::Converter& conv, uint8_t b, uint8_t g, uint8_t r, uint32_t w = 64, uint32_t h = 36) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (size_t p = 0; p < px.size(); p += 4) {
    px[p + 0] = b;
    px[p + 1] = g;
    px[p + 2] = r;
    px[p + 3] = 255;
  }
  return bc::VideoFrameIn{conv.uploadBgra8(px.data(), w, h, w * 4), bc::PixelLayout::BGRA8, true, 0};
}
double ratio(uint8_t a, uint8_t b) { return static_cast<double>(a) / std::max<int>(b, 1); }
}  // namespace

// spec D9/D10/D12, Review Focus 3: a Match camera eases towards the reference's measured skin and
// follows slowly. A is the blue-cast guest (sRGB 218,172,155), B the reference (sRGB 224,172,145).
TEST_CASE("Engine: Match brings a blue-cast camera to the reference's skin colour, slowly") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"mode", "match"}, {"matchTo", "b"}}, {{"bgra", {155, 172, 218}}}));
  show.cameras.push_back(cam("b", json::object(), {{"bgra", {145, 172, 224}}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(10s);

  auto seen = r.factory.output("o-a")->seen();
  REQUIRE(seen.size() > 2);
  // Skip the leading frames the async readback has not filled yet (centre pixel all zero).
  size_t i0 = 0;
  while (i0 < seen.size() && seen[i0].centerPixel[1] == 0) ++i0;
  REQUIRE(i0 + 1 < seen.size());
  // Follow slowly: no channel jumps by more than 2/255 between consecutive outputs (spec D12).
  for (size_t i = i0 + 1; i < seen.size(); ++i) {
    for (int ch = 0; ch < 4; ++ch) {
      CHECK(std::abs(static_cast<int>(seen[i].centerPixel[ch]) - static_cast<int>(seen[i - 1].centerPixel[ch])) <= 2);
    }
  }
  // A's output matches B's skin within 5%.
  const auto aC = seen.back().centerPixel;
  const auto bC = r.factory.output("o-b")->seen().back().centerPixel;
  REQUIRE(bC[1] > 0);
  CHECK(std::abs(ratio(aC[0], aC[1]) - ratio(bC[0], bC[1])) <= 0.05 * ratio(bC[0], bC[1]));  // B/G
  CHECK(std::abs(ratio(aC[2], aC[1]) - ratio(bC[2], bC[1])) <= 0.05 * ratio(bC[2], bC[1]));  // R/G
}

// Review Focus 3 (pinned here): the reference loses its picture, then is removed — the matching
// camera holds its last correction and the state says so.
TEST_CASE("Engine: Match holds when the reference loses its picture or is removed") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"mode", "match"}, {"matchTo", "b"}}, {{"bgra", {155, 172, 218}}}));
  show.cameras.push_back(cam("b", json::object(), {{"bgra", {145, 172, 224}}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(3s);  // part-way to convergence: a non-zero correction to hold

  // B loses its input: past the 250 ms stall threshold it is no longer Live, so A holds.
  r.factory.input("b")->paused = true;
  std::this_thread::sleep_for(1500ms);
  CHECK(r.at("/cameras/a/stages/color/match") == "holding-reference-down");
  auto held = r.factory.output("o-a")->seen().back().centerPixel;
  std::this_thread::sleep_for(2s);
  auto still = r.factory.output("o-a")->seen().back().centerPixel;
  for (int ch = 0; ch < 4; ++ch) CHECK(std::abs(static_cast<int>(still[ch]) - static_cast<int>(held[ch])) <= 1);

  // B is removed from the show: A still holds its correction, no exception.
  bc::Show without = show;
  without.cameras.pop_back();
  r.engine.apply(without);
  std::this_thread::sleep_for(1300ms);
  auto after = r.factory.output("o-a")->seen().back().centerPixel;
  for (int ch = 0; ch < 4; ++ch) CHECK(std::abs(static_cast<int>(after[ch]) - static_cast<int>(held[ch])) <= 1);
  CHECK(r.at("/cameras/a/stages/color/match") == "holding-reference-down");
}

// spec D13: with no reference Match auto-balances; with no skin it holds and says why.
TEST_CASE("Engine: Match with no reference auto-balances and reports it") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"mode", "match"}, {"matchTo", ""}}, {{"bgra", {155, 172, 218}}}));
  show.cameras.push_back(cam("b", {{"enabled", true}, {"mode", "match"}, {"matchTo", ""}}, {{"bgra", {200, 60, 40}}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1300ms);
  CHECK(r.at("/cameras/a/stages/color/match") == "auto");
  CHECK(r.at("/cameras/b/stages/color/match") == "holding-no-face");  // a blue wall has no skin
}

// spec D16: Match's easing correction lives in the camera, never in the show's stage params.
TEST_CASE("Camera: Match never writes the stage params (spec D16)") {
  Rig r;
  bc::StateStore state;
  bc::test::FakeFactory factory{r.conv};
  const json params = {{"enabled", true}, {"mode", "match"}, {"matchTo", ""}};
  bc::CameraConfig cfg = cam("a", params, {{"bgra", {155, 172, 218}}});
  cfg.outputs.clear();
  bc::Camera camera(cfg, factory,
                    bc::GpuTools{.conv = r.conv, .comp = r.comp, .grader = r.grader, .gpu = r.gpu.get()}, state, {});
  bc::TickSchedule sched{bc::Rate{60, 1}};
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  auto now = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < 60; ++i) {
    camera.onVideo(solidFrame(r.conv, 155, 172, 218));
    camera.beginTick(now, slate);
    camera.endTick(i, sched, now, nullptr);
    r.gpu->context()->Flush();
    std::this_thread::sleep_for(1ms);  // let the tiny readbacks land between ticks
    now += 100ms;                       // 10 s of wall time across the run, so the correction eases
  }
  camera.publishStatus();
  // The correction eased (auto balance moved the grade), but the stored params are untouched.
  CHECK(camera.colorParams() == bc::normalizeStageParams("color", params));
  CHECK(state.snapshot().at(json::json_pointer("/cameras/a/stages/color/match")) == "auto");
  camera.stop();
}
