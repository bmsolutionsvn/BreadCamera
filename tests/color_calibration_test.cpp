#include <doctest/doctest.h>

#include <thread>

#include "core/calibration.h"
#include "core/lut.h"
#include "core/stage_params.h"
#include "core/state_store.h"
#include "core/utf8.h"
#include "engine/color_calibration_service.h"
#include "engine/engine.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "support/fake_input.h"
#include "support/temp_dir.h"

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
const bc::ChartQuad kQuad{{{{0.25, 0.22}, {0.75, 0.22}, {0.75, 0.78}, {0.25, 0.78}}}};

struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};
  bc::ColorGrader grader{*gpu, pool};
  bc::StateStore state;
  bc::test::FakeFactory factory{conv};
  bc::Engine engine{bc::GpuTools{conv, comp, grader}, state, factory};
  bc::test::TempDir dir;
  bc::ColorCalibrationService svc{engine, dir.path / "luts"};
  ~Rig() { engine.stop(); }
};

bc::CameraConfig chartCam(const std::string& id, std::vector<float> cast) {
  bc::CameraConfig c;
  c.id = id;
  c.name = id;
  c.input.kind = "fake";
  c.input.params = {{"tag", id}, {"width", 960}, {"height", 540}, {"chart", true}, {"cast", cast}};
  c.outputs = {{"o-" + id, "fake", false, true, json::object()}};
  return c;
}

bc::Show showOf(std::vector<bc::CameraConfig> cams) {
  bc::Show s;
  s.settings.width = 960;
  s.settings.height = 540;
  s.cameras = std::move(cams);
  return s;
}
}  // namespace

TEST_CASE("Calibration: a printed card corrects a cast camera") {
  Rig r;
  r.engine.apply(showOf({chartCam("a", {1.25f, 1.0f, 0.7f})}));
  r.engine.start();
  std::this_thread::sleep_for(400ms);
  auto res = r.svc.calibrate("a", "card", kQuad);
  MESSAGE("card: before " << res.deltaEBefore << ", after " << res.deltaE);
  CHECK(res.deltaEBefore > 5);
  CHECK(res.deltaE < 2);
  CHECK(res.rating == "good");
  CHECK_FALSE(res.reference);
  CHECK(bc::readCube(std::filesystem::path(bc::toWide(res.lutPath))).size == 33);
}

TEST_CASE("Calibration: tablet mode matches the second camera to the first") {
  Rig r;
  r.engine.apply(showOf({chartCam("a", {1.2f, 1.0f, 0.8f}), chartCam("b", {0.8f, 1.0f, 1.25f})}));
  r.engine.start();
  std::this_thread::sleep_for(400ms);
  auto first = r.svc.calibrate("a", "tablet", kQuad);
  CHECK(first.reference);
  CHECK(first.deltaE == 0);
  auto second = r.svc.calibrate("b", "tablet", kQuad);
  CHECK_FALSE(second.reference);
  MESSAGE("tablet: before " << second.deltaEBefore << ", after " << second.deltaE);
  CHECK(second.deltaE < 2);
  r.svc.forget("a");  // the reference camera is cleared
  auto again = r.svc.calibrate("b", "tablet", kQuad);
  CHECK(again.reference);  // b is the new reference
}

TEST_CASE("Calibration: wrong corners and a camera without picture are refused") {
  Rig r;
  r.engine.apply(showOf({chartCam("a", {1, 1, 1})}));
  r.engine.start();
  std::this_thread::sleep_for(400ms);
  bc::ChartQuad off{{{{0.01, 0.01}, {0.15, 0.01}, {0.15, 0.1}, {0.01, 0.1}}}};
  CHECK_THROWS_WITH_AS(r.svc.calibrate("a", "card", off), doctest::Contains("not on the chart"), std::invalid_argument);
  r.factory.input("a")->paused = true;
  std::this_thread::sleep_for(6000ms);
  CHECK_THROWS_WITH_AS(r.svc.calibrate("a", "card", kQuad), doctest::Contains("no picture"), std::invalid_argument);
}