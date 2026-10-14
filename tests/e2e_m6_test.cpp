#include <doctest/doctest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/stage_params.h"
#include "core/state_store.h"
#include "engine/engine.h"
#include "engine/matte.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "support/fake_input.h"
#include "support/requires.h"
#include "support/temp_dir.h"
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
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig& cfg, const bc::CameraConfig& cam) override { return fake.makeOutput(cfg, cam); }
};
}  // namespace

TEST_CASE("M6 acceptance: 4x 1080p60 with the ONNX matte, no tick overruns" BC_REQUIRES(bc::test::Req::Onnx)) {
  bc::test::TempDir dir;
  bc::test::writeTestTs(dir.path / "clip.ts", 1920, 1080, 60, 10);
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::Compositor comp(*gpu, pool);
  bc::ColorGrader grader(*gpu, pool);
  bc::StateStore state;
  HybridFactory factory(*gpu, pool, conv);
  bc::Engine engine(bc::GpuTools{conv, comp, grader}, state, factory);
  auto choice = bc::makeMatteBackend(*gpu, pool, conv);
  REQUIRE(choice.backend);
  MESSAGE("matte backend: " << choice.backend->name());
  engine.setMatteBackend(std::move(choice.backend), choice.unavailableReason);
  bc::Show show;
  const char* modes[] = {"blur", "blur", "keyed", "replace"};
  for (int i = 0; i < 4; ++i) {
    bc::CameraConfig c;
    c.id = "cam" + std::to_string(i);
    c.name = c.id;
    c.input.kind = "srt";
    c.input.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47991 + i}};
    c.outputs.push_back({"out" + std::to_string(i), "fake", false, true, {}});
    c.stages = {{"matte", bc::normalizeStageParams("matte", {{"enabled", true}, {"quality", "performance"}})},
                {"background", bc::normalizeStageParams("background", {{"mode", modes[i]}})}};
    show.cameras.push_back(c);
  }
  engine.apply(show);
  engine.start();
  std::vector<std::unique_ptr<bc::test::TsPusher>> pushers;
  for (int i = 0; i < 4; ++i) {
    pushers.push_back(std::make_unique<bc::test::TsPusher>(dir.path / "clip.ts", "srt://127.0.0.1:" + std::to_string(47991 + i) + "?mode=caller", true));
  }
  std::this_thread::sleep_for(10s);
  const auto o0 = engine.stats().overruns;
  std::this_thread::sleep_for(20s);
  const auto st = engine.stats();
  auto snap = state.snapshot();
  MESSAGE("ONNX inference: " << snap["engine"].value("matteMs", 0.0) << " ms per camera");
  CHECK(st.overruns - o0 == 0);
  for (int i = 0; i < 4; ++i) {
    const auto id = "cam" + std::to_string(i);
    MESSAGE(id << " (" << std::string(modes[i]) << "): latency " << st.cameras.at(id).latencyMs << " ms");
    CHECK(snap["cameras"][id]["stages"]["matte"]["state"] == "active");
    CHECK(st.cameras.at(id).latencyMs <= 50.0);
  }
  for (auto& p : pushers) p->stop();
  engine.stop();
}