#include <doctest/doctest.h>

#include <thread>

#include "core/stage_params.h"
#include "core/state_store.h"
#include "engine/engine.h"
#include "engine/gpu_tools.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "support/fake_input.h"
#include "support/requires.h"
#include "support/temp_dir.h"
#include "support/test_matte.h"
#include "support/test_media.h"

using namespace std::chrono_literals;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {
struct HybridFactory : bc::AdapterFactory {
  bc::DefaultAdapterFactory real;
  bc::test::FakeFactory fake;
  HybridFactory(bc::GpuDevice& g, bc::TexturePool& p, bc::Converter& c) : real(g, p), fake(c) {}
  std::unique_ptr<bc::Input> makeInput(const bc::InputConfig& cfg) override { return real.makeInput(cfg); }
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig& cfg, const bc::CameraConfig& cam, bc::Rate rate) override {
    return fake.makeOutput(cfg, cam, rate);
  }
};
}  // namespace

TEST_CASE("M2 acceptance: 4x 1080p60 with matte refine and background stages, no overruns" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  bc::test::TempDir dir;
  const auto clip = dir.path / "1080p60.ts";
  bc::test::writeTestTs(clip, 1920, 1080, 60, 10);
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::Compositor comp(*gpu, pool);
  bc::ColorGrader grader(*gpu, pool);
  bc::StateStore state;
  HybridFactory factory(*gpu, pool, conv);
  bc::Engine engine(bc::GpuTools{conv, comp, grader}, state, factory);
  engine.setMatteBackend(std::make_unique<bc::test::TestMatte>(*gpu, pool), "");

  const char* modes[] = {"blur", "blur", "keyed", "replace"};
  bc::Show show;
  for (int i = 0; i < 4; ++i) {
    bc::CameraConfig c;
    c.id = "cam" + std::to_string(i);
    c.name = c.id;
    c.input.kind = "srt";
    c.input.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 49100 + i}};
    c.outputs.push_back({"out" + std::to_string(i), "fake", false, true, json::object()});
    c.stages = {{"matte", bc::normalizeStageParams("matte", {{"enabled", true}, {"choke", 1}, {"feather", 2}})},
                {"background", bc::normalizeStageParams("background", {{"mode", modes[i]}, {"blur", 30}})}};
    show.cameras.push_back(c);
  }
  engine.apply(show);
  engine.start();
  std::vector<std::unique_ptr<bc::test::TsPusher>> pushers;
  for (int i = 0; i < 4; ++i) {
    pushers.push_back(std::make_unique<bc::test::TsPusher>(clip, "srt://127.0.0.1:" + std::to_string(49100 + i) + "?mode=caller", true));
  }
  auto live = [&] {
    auto s = state.snapshot();
    for (int i = 0; i < 4; ++i) {
      auto p = json::json_pointer("/cameras/cam" + std::to_string(i) + "/phase");
      if (!s.contains(p) || s.at(p) != "live") return false;
    }
    return true;
  };
  const auto warm = Clock::now() + 20s;
  while (!live() && Clock::now() < warm) std::this_thread::sleep_for(100ms);
  REQUIRE(live());
  std::this_thread::sleep_for(3s);

  const auto o0 = engine.stats().overruns;
  std::this_thread::sleep_for(20s);
  const auto st = engine.stats();
  CHECK(st.overruns - o0 == 0);
  auto snap = state.snapshot();
  for (int i = 0; i < 4; ++i) {
    const auto id = "cam" + std::to_string(i);
    MESSAGE(id << " (" << std::string(modes[i]) << "): latency " << st.cameras.at(id).latencyMs << " ms");
    CHECK(st.cameras.at(id).latencyMs <= 50.0);
    CHECK(snap["cameras"][id]["stages"]["matte"]["state"] == "active");
    CHECK(snap["cameras"][id]["stages"]["background"]["state"] == "active");
  }
  const auto seen = factory.fake.output("out2")->seen();
  REQUIRE_FALSE(seen.empty());
  auto keyed = seen.back();
  CHECK(keyed.firstPixel[3] == 0);
  CHECK(keyed.centerPixel[3] == 255);
  for (auto& p : pushers) p->stop();
  engine.stop();
}
