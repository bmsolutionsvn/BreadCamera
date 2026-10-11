#include <doctest/doctest.h>

#include <thread>

#include "core/stage_params.h"
#include "core/state_store.h"
#include "engine/engine.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "support/fake_input.h"
#include "support/test_matte.h"

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};
  bc::StateStore state;
  bc::test::FakeFactory factory{conv};
  bc::Engine engine{conv, comp, state, factory};
  bc::test::TestMatte* matte = nullptr;
  explicit Rig(bool withMatte = true) {
    if (withMatte) {
      auto m = std::make_unique<bc::test::TestMatte>(*gpu, pool);
      matte = m.get();
      engine.setMatteBackend(std::move(m), "");
    } else {
      engine.setMatteBackend(nullptr, "background removal needs an NVIDIA RTX GPU");
    }
  }
  ~Rig() { engine.stop(); }
  json at(const std::string& ptr) {
    auto s = state.snapshot();
    auto p = json::json_pointer(ptr);
    return s.contains(p) ? s.at(p) : json();
  }
};

bc::CameraConfig cam(const std::string& id, const std::string& mode, json inParams = json::object()) {
  bc::CameraConfig c;
  c.id = id;
  c.name = id;
  c.input.kind = "fake";
  c.input.params = inParams;
  c.input.params["tag"] = id;
  c.outputs = {{"o-" + id, "fake", false, true, json::object()}};
  c.stages = {{"matte", bc::normalizeStageParams("matte", {{"enabled", true}, {"feather", 0}})},
              {"background", bc::normalizeStageParams("background", {{"mode", mode}})}};
  return c;
}

// Small show format so the tests stay cheap (the fake input is 64x36 by default); it also
// keeps each tick well under the status-publish grid so a guard change is visible on time.
bc::Show smallShow() {
  bc::Show s;
  s.settings.width = 64;
  s.settings.height = 36;
  return s;
}

std::chrono::milliseconds maxGap(const std::vector<bc::test::FakeOutput::Seen>& seen) {
  std::chrono::milliseconds g{0};
  for (size_t i = 1; i < seen.size(); ++i) {
    g = std::max(g, std::chrono::duration_cast<std::chrono::milliseconds>(seen[i].at - seen[i - 1].at));
  }
  return g;
}
}  // namespace

TEST_CASE("Matte: keyed output carries the person's alpha") {
  Rig r;
  bc::Show show = smallShow();
  show.cameras.push_back(cam("a", "keyed"));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1200ms);
  auto s = r.factory.output("o-a")->seen().back();
  CHECK(s.firstPixel[3] == 0);     // corner: background
  CHECK(s.centerPixel[3] == 255);  // centre: person
  CHECK(r.at("/cameras/a/stages/matte/state") == "active");
  CHECK(r.at("/cameras/a/stages/background/state") == "active");
}

TEST_CASE("Matte: blur and replace outputs stay opaque") {
  Rig r;
  bc::Show show = smallShow();
  show.cameras.push_back(cam("a", "blur"));
  show.cameras.push_back(cam("b", "replace"));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(600ms);
  CHECK(r.factory.output("o-a")->seen().back().firstPixel[3] == 255);
  CHECK(r.factory.output("o-b")->seen().back().firstPixel[3] == 255);
  REQUIRE_FALSE(r.matte->batchSizes.empty());
  CHECK(r.matte->batchSizes.back() == 2);  // one batched call for both cameras
}

TEST_CASE("Matte: a failing backend bypasses to opaque passthrough, then recovers") {
  Rig r;
  bc::Show show = smallShow();
  show.cameras.push_back(cam("a", "keyed"));
  r.engine.apply(show);
  r.matte->fail = true;
  r.engine.start();
  std::this_thread::sleep_for(1200ms);
  CHECK(r.at("/cameras/a/stages/matte/state") == "bypassed");
  CHECK(r.at("/cameras/a/stages/matte/error/code") == "stage-failed");
  auto seen = r.factory.output("o-a")->seen();
  CHECK(maxGap(seen) < 50ms);
  CHECK(seen.back().firstPixel[3] == 255);  // opaque, not transparent, not black
  r.matte->fail = false;
  std::this_thread::sleep_for(5500ms);
  CHECK(r.at("/cameras/a/stages/matte/state") == "active");
  CHECK(r.factory.output("o-a")->seen().back().firstPixel[3] == 0);
}

TEST_CASE("Matte: no backend reports matte-unavailable and frames pass through") {
  Rig r(false);
  bc::Show show = smallShow();
  show.cameras.push_back(cam("a", "keyed"));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1200ms);
  CHECK(r.at("/cameras/a/stages/matte/error/code") == "matte-unavailable");
  CHECK(r.at("/cameras/a/stages/matte/error/message").get<std::string>().find("NVIDIA") != std::string::npos);
  CHECK(r.factory.output("o-a")->seen().back().firstPixel[3] == 255);
}

TEST_CASE("Matte: a slow backend is bypassed for overrun and the tick stops overrunning") {
  Rig r;
  bc::Show show = smallShow();
  show.cameras.push_back(cam("a", "keyed"));
  r.engine.apply(show);
  r.matte->delayMs = 25;  // > 60% of a 60 fps tick
  r.engine.start();
  // Status publishes only every 60 ticks and the tick index skips while the tick
  // overruns, so a fixed sleep can read a stale grid slot; poll for the bypass.
  bool bypassed = false;
  for (int i = 0; i < 100 && !bypassed; ++i) {  // up to ~5 s
    bypassed = r.at("/cameras/a/stages/matte/state") == "bypassed";
    if (!bypassed) std::this_thread::sleep_for(50ms);
  }
  REQUIRE(bypassed);
  CHECK(r.at("/cameras/a/stages/matte/error/code") == "stage-overrun");
  size_t batches;
  {
    std::lock_guard l(r.matte->m);
    batches = r.matte->batchSizes.size();
  }
  std::this_thread::sleep_for(1500ms);  // still inside the 5 s retry pause
  std::lock_guard l(r.matte->m);
  CHECK(r.matte->batchSizes.size() == batches);  // the bypassed matte is no longer called
}

TEST_CASE("Matte: removing a camera releases its matte state; mixed sizes each get their own alpha") {
  Rig r;
  bc::Show show;
  // Every camera's frame is fitted to the show format (§4.3), so an input size alone
  // cannot change the matte job size; a 32x18 show keeps camera b's frame at its
  // native input size.
  show.settings.width = 32;
  show.settings.height = 18;
  show.cameras.push_back(cam("a", "keyed"));
  show.cameras.push_back(cam("b", "keyed", {{"width", 32}, {"height", 18}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(600ms);
  auto b = r.factory.output("o-b")->seen().back();
  CHECK(b.width == 32);
  CHECK(b.centerPixel[3] == 255);
  CHECK(b.firstPixel[3] == 0);
  show.cameras.pop_back();
  r.engine.apply(show);
  std::lock_guard l(r.matte->m);
  CHECK(r.matte->forgotten == std::vector<std::string>{"b"});
}
