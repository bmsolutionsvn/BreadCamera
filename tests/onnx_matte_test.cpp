#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_ONNX
#include <windows.h>

#include <thread>

#include "engine/onnx_matte.h"
#include "gpu/device.h"

using namespace std::chrono_literals;

namespace {
std::filesystem::path modelPath() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path() / "models" / "modnet.onnx";
}
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
};
}  // namespace

TEST_CASE("OnnxMatte: a missing model is refused with its path") {
  Rig r;
  try {
    bc::OnnxMatte m(*r.gpu, r.pool, r.conv, "C:/nope/modnet.onnx");
    FAIL("should throw");
  } catch (const std::runtime_error& e) {
    CHECK(std::string(e.what()).find("modnet.onnx") != std::string::npos);
  }
}

TEST_CASE("OnnxMatte: opaque until the first result, then a model-size matte per camera" BC_REQUIRES(bc::test::Req::Onnx)) {
  Rig r;
  bc::OnnxMatte m(*r.gpu, r.pool, r.conv, modelPath());
  MESSAGE("backend: " << m.name());
  CHECK(m.name().rfind("onnx-", 0) == 0);
  auto frame = r.conv.solid(1920, 1080, {0.3f, 0.25f, 0.2f, 1});
  std::vector<bc::MatteJob> jobs{{"a", frame, bc::MatteQuality::Performance, nullptr}, {"b", frame, bc::MatteQuality::Performance, nullptr}};
  std::vector<bc::MatteJob*> ptrs{&jobs[0], &jobs[1]};
  m.process(ptrs);
  REQUIRE(jobs[0].alpha);
  const auto firstAlpha = jobs[0].alpha;
  bool updated = false;
  for (int i = 0; i < 100 && !updated; ++i) {
    std::this_thread::sleep_for(20ms);
    for (auto& j : jobs) j.alpha = nullptr;
    m.process(ptrs);
    updated = jobs[0].alpha != firstAlpha && jobs[1].alpha != firstAlpha;
  }
  REQUIRE(updated);
  CHECK(jobs[0].alpha->width == 448);
  CHECK(jobs[0].alpha->height == 256);
  CHECK(m.lastInferenceMs() > 0);
  MESSAGE("inference " << m.lastInferenceMs() << " ms per camera (448x256)");
}

TEST_CASE("OnnxMatte: quality switch and a forgotten camera are handled" BC_REQUIRES(bc::test::Req::Onnx)) {
  Rig r;
  bc::OnnxMatte m(*r.gpu, r.pool, r.conv, modelPath());
  auto frame = r.conv.solid(1280, 720, {0.3f, 0.3f, 0.3f, 1});
  bc::MatteJob job{"a", frame, bc::MatteQuality::Performance, nullptr};
  std::vector<bc::MatteJob*> ptrs{&job};
  for (int i = 0; i < 60; ++i) {
    m.process(ptrs);
    std::this_thread::sleep_for(20ms);
  }
  job.quality = bc::MatteQuality::Quality;
  bool big = false;
  for (int i = 0; i < 100 && !big; ++i) {
    m.process(ptrs);
    big = job.alpha && job.alpha->width == 896 && job.alpha->height == 512;
    std::this_thread::sleep_for(20ms);
  }
  CHECK(big);
  m.forget("a");  // possibly while a frame for "a" is in the worker
  bc::MatteJob other{"z", frame, bc::MatteQuality::Performance, nullptr};
  std::vector<bc::MatteJob*> p2{&other};
  m.process(p2);
  CHECK(other.alpha);
}
#else
TEST_CASE("OnnxMatte: opaque until the first result, then a model-size matte per camera" BC_REQUIRES(bc::test::Req::Onnx)) {}
#endif
