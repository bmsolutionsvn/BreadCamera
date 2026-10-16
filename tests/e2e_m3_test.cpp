#include <doctest/doctest.h>

#include <thread>

#include "core/calibration.h"
#include "core/stage_params.h"
#include "core/state_store.h"
#include "engine/color_calibration_service.h"
#include "engine/engine.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "support/fake_input.h"
#include "support/requires.h"
#include "support/temp_dir.h"

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
const bc::ChartQuad kQuad{{{{0.25, 0.22}, {0.75, 0.22}, {0.75, 0.78}, {0.25, 0.78}}}};

bc::CameraConfig chartCam(const std::string& id, std::vector<float> cast) {
  bc::CameraConfig c;
  c.id = id;
  c.name = id;
  c.input.kind = "fake";
  c.input.params = {{"tag", id}, {"width", 960}, {"height", 540}, {"chart", true}, {"cast", cast}};
  c.outputs = {{"o-" + id, "fake", false, true, json::object()}};
  return c;
}
}  // namespace

TEST_CASE("M3 acceptance: printed card and tablet calibration through the GPU" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::Compositor comp(*gpu, pool);
  bc::ColorGrader grader(*gpu, pool);
  bc::StateStore state;
  bc::test::FakeFactory factory(conv);
  bc::Engine engine(bc::GpuTools{conv, comp, grader}, state, factory);
  bc::test::TempDir dir;
  bc::ColorCalibrationService svc(engine, dir.path / "luts");

  bc::Show show;
  show.cameras = {chartCam("card", {1.25f, 1.0f, 0.7f}), chartCam("t1", {1.2f, 1.0f, 0.8f}), chartCam("t2", {0.8f, 1.0f, 1.25f})};
  engine.apply(show);
  engine.start();
  std::this_thread::sleep_for(500ms);

  auto card = svc.calibrate("card", "card", kQuad);
  auto t1 = svc.calibrate("t1", "tablet", kQuad);
  auto t2 = svc.calibrate("t2", "tablet", kQuad);
  REQUIRE(t1.reference);
  auto setLut = [&](size_t i, const std::string& lut) {
    show.cameras[i].stages = {{"color", bc::normalizeStageParams("color", {{"enabled", true}, {"lut", lut}})}};
  };
  setLut(0, card.lutPath);
  setLut(1, t1.lutPath);
  setLut(2, t2.lutPath);
  engine.apply(show);
  std::this_thread::sleep_for(800ms);

  auto seen = [&](const std::string& id) {
    auto img = factory.output("o-" + id)->lastImage();
    REQUIRE(img);
    return bc::sampleChart({*img}, kQuad);
  };
  const double cardAfter = bc::meanDeltaE(seen("card"), bc::colorCheckerReference());
  const double tabletAfter = bc::meanDeltaE(seen("t1"), seen("t2"));
  MESSAGE("card: solver " << card.deltaEBefore << " -> " << card.deltaE << ", through the GPU " << cardAfter);
  MESSAGE("tablet: solver " << t2.deltaEBefore << " -> " << t2.deltaE << ", camera-to-camera through the GPU " << tabletAfter);
  CHECK(cardAfter < 2.5);
  CHECK(tabletAfter < 2.5);
  engine.stop();
}