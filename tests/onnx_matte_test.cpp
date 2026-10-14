#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_ONNX
#include <windows.h>

#include <chrono>
#include <fstream>
#include <thread>

#include "engine/onnx_matte.h"
#include "gpu/device.h"
#include "support/temp_dir.h"

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

TEST_CASE("OnnxMatte: a broken session surfaces a worker error" BC_REQUIRES(bc::test::Req::Onnx)) {
  // An 87-byte valid ONNX file (Identity, input/output int64[1]): the session loads, but every
  // Run with our float image tensor throws. It stands in for a session that has gone bad at
  // runtime, which must surface instead of leaving a frozen mask reported as active.
  static const unsigned char kWrongModel[] = {
      0x08, 0x08, 0x3a, 0x4d, 0x0a, 0x19, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x12, 0x06, 0x6f,
      0x75, 0x74, 0x70, 0x75, 0x74, 0x22, 0x08, 0x49, 0x64, 0x65, 0x6e, 0x74, 0x69, 0x74, 0x79, 0x12,
      0x05, 0x77, 0x72, 0x6f, 0x6e, 0x67, 0x5a, 0x13, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x12,
      0x0a, 0x0a, 0x08, 0x08, 0x07, 0x12, 0x04, 0x0a, 0x02, 0x08, 0x01, 0x62, 0x14, 0x0a, 0x06, 0x6f,
      0x75, 0x74, 0x70, 0x75, 0x74, 0x12, 0x0a, 0x0a, 0x08, 0x08, 0x07, 0x12, 0x04, 0x0a, 0x02, 0x08,
      0x01, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x0d};
  bc::test::TempDir dir;
  const auto path = dir.path / "wrong.onnx";
  {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(kWrongModel), static_cast<std::streamsize>(sizeof(kWrongModel)));
  }
  Rig r;
  bc::OnnxMatte m(*r.gpu, r.pool, r.conv, path);
  auto frame = r.conv.solid(1280, 720, {0.3f, 0.3f, 0.3f, 1});
  bc::MatteJob job{"a", frame, bc::MatteQuality::Performance, nullptr};
  std::vector<bc::MatteJob*> ptrs{&job};
  // The worker records its first failure; process() must then throw so the stage guard bypasses.
  bool threw = false;
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!threw && std::chrono::steady_clock::now() < deadline) {
    try {
      m.process(ptrs);
    } catch (const std::runtime_error&) {
      threw = true;
    }
    if (!threw) std::this_thread::sleep_for(20ms);
  }
  CHECK(threw);
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
  // A single 896x512 MODNet inference can take well over 2 s on an integrated GPU (it is ~1.8 s on an
  // Intel UHD 630), so poll on a generous wall-clock budget instead of a fixed iteration count. The
  // expectation itself is unchanged: the result must still reach 896x512.
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (!big && std::chrono::steady_clock::now() < deadline) {
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
