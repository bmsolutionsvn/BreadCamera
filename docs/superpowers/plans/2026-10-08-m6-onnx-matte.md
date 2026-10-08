# BreadCamera M6 — ONNX matte backend (non-NVIDIA path) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Background removal on AMD/Intel GPUs (and on NVIDIA when Maxine is missing): a `MatteBackend` running the MODNet portrait-matting model through ONNX Runtime with the DirectML execution provider, asynchronously so inference never stalls the tick.

**Architecture:** `OnnxMatte` implements M2's `MatteBackend`. On the tick thread `process()` only downsizes each camera's frame on the GPU, reads it back (≤ 896×512), hands it to a worker thread, and returns that camera's **latest finished** alpha (opaque until the first one). The worker runs MODNet per camera round-robin on DirectML (the same physical GPU, chosen by adapter LUID), smooths the alpha over time, and uploads it as an R8 texture. The mask therefore trails the picture by 1–2 frames; the picture itself is never delayed. `makeMatteBackend` prefers Maxine on NVIDIA, then ONNX, then none.

**Tech Stack:** ONNX Runtime 1.24.4 DirectML package (`Microsoft.ML.OnnxRuntime.DirectML`, MIT) + DirectML 1.15.4 (`Microsoft.AI.DirectML`), MODNet ONNX (`Xenova/modnet` @ `fa2fa546052fba4c08921230a26cc69a333fca12`, `onnx/model.onnx`, Apache-2.0, SHA-256 `07c308cf0fc7e6e8b2065a12ed7fc07e1de8febb7dc7839d7b7f15dd66584df9`).

**Spec:** `docs/superpowers/specs/2026-10-08-breadcamera-design.md` (§5.1, §7.2, §9 M6).

**Branch:** `bcam-m6`, cut from `bcam-m5`.

## Global Constraints

- All earlier Global Constraints apply; matte rules from M2 (stage guard, opaque passthrough on failure, 5 s retry) are unchanged.
- **Deviation from spec §5.1, recorded in follow-ups:** the ONNX backend uses the DirectML execution provider on every vendor (spec: TensorRT EP on NVIDIA). On NVIDIA the primary backend is Maxine; ONNX is only its fallback, so one DirectML package covers all GPUs.
- Model input: RGB, `(x/255 − 0.5)/0.5`, NCHW float32, `[1,3,H,W]`; output `[1,1,H,W]` alpha 0..1. Sizes: quality = 896×512, performance = 448×256 (both multiples of 32, ~16:9).
- Temporal smoothing: `alpha = 0.6·new + 0.4·previous` per camera (reset when the camera is forgotten or its quality changes).
- The model and runtimes are fetched by `scripts/fetch-sdks.ps1` with pinned SHA-256 and shipped in `<app>\models\modnet.onnx` and next to the executables.

## Review Focus

1. **Model file missing or corrupt** — `makeMatteBackend` falls back to "no backend" with a reason naming the file; the engine still starts — owned by Task 2.
2. **DirectML unavailable** (old driver, remote session) — the backend falls back to the CPU provider and says so in its name (`onnx-cpu`); the stage guard handles it if too slow — owned by Task 2.
3. **Inference slower than the camera rate** — frames are never delayed; the mask just updates less often; no tick overruns — owned by Task 3.
4. **A camera removed while its frame is in the worker** — no crash, no use of freed state — owned by Task 2.
5. **Quality switched mid-show** — the next result uses the new size; smoothing restarts; no mismatched buffer sizes — owned by Task 2.

## Execution notes for the controller

- 3 tasks → native mode (one implementer, one whole-branch review).
- Task 1 downloads ~60 MB (two NuGet packages + the model).

---

### Task 1: Fetch ONNX Runtime, DirectML and the model; CMake

**Files:**
- Create: `cmake/FindOnnxRuntime.cmake`
- Modify: `scripts/fetch-sdks.ps1`, `CMakeLists.txt` (`BC_WITH_ONNX`, runtime + model copy), `tests/support/requires.h/.cpp` (`Req::Onnx`)

**Interfaces:**
- Produces: `third_party/onnxruntime/` (`include/onnxruntime_cxx_api.h`, `include/dml_provider_factory.h`, `lib/onnxruntime.lib`, `bin/onnxruntime.dll`), `third_party/directml/bin/DirectML.dll`, `third_party/models/modnet.onnx`; CMake `BC_WITH_ONNX`, compile definition `BC_WITH_ONNX=0|1`; `bc_copy_runtime` copies `onnxruntime.dll`, `DirectML.dll` and `models/modnet.onnx` next to an executable; `Req::Onnx` (missing unless `BC_WITH_ONNX` and `<exe dir>\models\modnet.onnx` exists).

- [ ] **Step 1: Extend the fetch script**

Add to `scripts/fetch-sdks.ps1` (NuGet packages are zip files; the layouts below are those of the 1.24.4 and 1.15.4 packages — if a path differs, follow the package and ledger it):
```powershell
function Get-NuGet($name, $id, $version, $sha256, [hashtable]$copy) {
  $dest = Join-Path $root $name
  if (Test-Path (Join-Path $dest '.ok')) { Write-Host "$name already present"; return }
  $zip = Join-Path $env:TEMP "$id.$version.zip"
  Invoke-WebRequest -Uri "https://api.nuget.org/v3-flatcontainer/$($id.ToLower())/$version/$($id.ToLower()).$version.nupkg" -OutFile $zip
  $actual = (Get-FileHash $zip -Algorithm SHA256).Hash
  if ($sha256 -and $actual -ne $sha256) { throw "$name: SHA-256 mismatch ($actual)" }
  if (-not $sha256) { Write-Warning "$name: no pinned hash yet; actual SHA-256 is $actual - pin it in this script" }
  $tmp = Join-Path $env:TEMP "$name-x"
  Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
  Expand-Archive $zip $tmp
  foreach ($from in $copy.Keys) {
    $to = Join-Path $dest $copy[$from]
    New-Item -ItemType Directory -Force $to | Out-Null
    Copy-Item -Recurse -Force (Join-Path $tmp $from) $to
  }
  New-Item (Join-Path $dest '.ok') -ItemType File | Out-Null
}

Get-NuGet 'onnxruntime' 'Microsoft.ML.OnnxRuntime.DirectML' '1.24.4' '' @{
  'build\native\include\*' = 'include'
  'runtimes\win-x64\native\onnxruntime.lib' = 'lib'
  'runtimes\win-x64\native\onnxruntime.dll' = 'bin'
}
Get-NuGet 'directml' 'Microsoft.AI.DirectML' '1.15.4' '' @{ 'bin\x64-win\DirectML.dll' = 'bin' }

$model = Join-Path $root 'models\modnet.onnx'
if (-not (Test-Path $model)) {
  New-Item -ItemType Directory -Force (Split-Path $model) | Out-Null
  Invoke-WebRequest -Uri 'https://huggingface.co/Xenova/modnet/resolve/fa2fa546052fba4c08921230a26cc69a333fca12/onnx/model.onnx' -OutFile $model
}
$h = (Get-FileHash $model -Algorithm SHA256).Hash.ToLower()
if ($h -ne '07c308cf0fc7e6e8b2065a12ed7fc07e1de8febb7dc7839d7b7f15dd66584df9') { Remove-Item $model; throw "modnet.onnx: SHA-256 mismatch ($h)" }
```
Run it, pin the two printed NuGet hashes, run again (expect "already present" and a passing model hash).

- [ ] **Step 2: CMake**

`cmake/FindOnnxRuntime.cmake`:
```cmake
set(_ort "${CMAKE_SOURCE_DIR}/third_party/onnxruntime")
find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h PATHS "${_ort}/include" NO_DEFAULT_PATH)
find_path(ORT_DML_INCLUDE_DIR dml_provider_factory.h PATHS "${_ort}/include" NO_DEFAULT_PATH)
find_library(ORT_LIBRARY onnxruntime PATHS "${_ort}/lib" NO_DEFAULT_PATH)
find_file(ORT_DLL onnxruntime.dll PATHS "${_ort}/bin" NO_DEFAULT_PATH)
find_file(DML_DLL DirectML.dll PATHS "${CMAKE_SOURCE_DIR}/third_party/directml/bin" NO_DEFAULT_PATH)
find_file(MODNET_ONNX modnet.onnx PATHS "${CMAKE_SOURCE_DIR}/third_party/models" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OnnxRuntime DEFAULT_MSG ORT_INCLUDE_DIR ORT_DML_INCLUDE_DIR ORT_LIBRARY ORT_DLL DML_DLL MODNET_ONNX)
if(OnnxRuntime_FOUND AND NOT TARGET ORT::ORT)
  add_library(ORT::ORT UNKNOWN IMPORTED)
  set_target_properties(ORT::ORT PROPERTIES IMPORTED_LOCATION "${ORT_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${ORT_INCLUDE_DIR}")
endif()
```
`CMakeLists.txt`: the usual option block (`find_package(OnnxRuntime)`, `option(BC_WITH_ONNX ... ${OnnxRuntime_FOUND})`, warning + OFF when missing, status line); after `bc_engine`:
```cmake
if(BC_WITH_ONNX)
  target_sources(bc_engine PRIVATE src/engine/onnx_matte.cpp)
  target_link_libraries(bc_engine PUBLIC ORT::ORT dxgi)
  target_compile_definitions(bc_engine PUBLIC BC_WITH_ONNX=1)
else()
  target_compile_definitions(bc_engine PUBLIC BC_WITH_ONNX=0)
endif()
```
inside `bc_copy_runtime`:
```cmake
  if(BC_WITH_ONNX)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORT_DLL}" "${DML_DLL}" "$<TARGET_FILE_DIR:${target}>"
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/models"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MODNET_ONNX}" "$<TARGET_FILE_DIR:${target}>/models/modnet.onnx")
  endif()
```
Note: Windows ships an old `onnxruntime.dll`/`DirectML.dll` in System32 on some builds; copying ours next to the exe makes the loader pick ours first. `requires`: `Onnx` → missing when `BC_WITH_ONNX == 0` or `<exe dir>\models\modnet.onnx` is absent; reason "ONNX Runtime/model not fetched (scripts/fetch-sdks.ps1)".

Run `scripts/build.ps1` → `BC_WITH_ONNX=ON`, `build/release/models/modnet.onnx` present.

- [ ] **Step 3: Commit**

```bash
git add scripts/fetch-sdks.ps1 cmake/FindOnnxRuntime.cmake CMakeLists.txt tests/support
git commit -m "build: pinned ONNX Runtime (DirectML), DirectML and the MODNet model"
```

---

### Task 2: `OnnxMatte` backend

**Files:**
- Create: `src/engine/onnx_matte.h`, `src/engine/onnx_matte.cpp`
- Modify: `src/engine/matte.cpp` (ONNX branch), `src/engine/engine.cpp` (publish `/engine/matteMs` when the backend reports it)
- Test: `tests/onnx_matte_test.cpp`

**Interfaces:**
- Consumes: `MatteBackend`, `MatteJob`, `MatteQuality`, `MatteBackendChoice`, `makeMatteBackend` (M2 Task 4); `Converter::toBgra8`, `Converter::readback`, `TexturePool`, `GpuDevice` (M1).
- Produces: `bc::OnnxMatte(GpuDevice&, TexturePool&, Converter&, std::filesystem::path modelPath)` (throws `std::runtime_error` naming the model file when it cannot load); `name()` → `"onnx-directml"` or `"onnx-cpu"`; `double lastInferenceMs() const`; `MatteBackend` gains `virtual double lastInferenceMs() const { return 0; }` (the engine sets `/engine/matteMs` from it once per second).

- [ ] **Step 1: Write the failing tests**

`tests/onnx_matte_test.cpp`:
```cpp
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
```
Add it to `bc_tests`. Run `-Filter "OnnxMatte*"` → build FAILS.

- [ ] **Step 2: Implement**

`src/engine/onnx_matte.h`:
```cpp
#pragma once
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/matte.h"

namespace Ort {
struct Env;
struct Session;
}  // namespace Ort

namespace bc {
// MODNet portrait matting on ONNX Runtime + DirectML, run off the tick thread (spec §5.1 fallback).
class OnnxMatte final : public MatteBackend {
 public:
  OnnxMatte(GpuDevice& gpu, TexturePool& pool, Converter& conv, std::filesystem::path modelPath);
  ~OnnxMatte() override;
  std::string name() const override { return name_; }
  void process(std::vector<MatteJob*>& jobs) override;
  void forget(const std::string& cameraId) override;
  double lastInferenceMs() const override { return lastMs_; }

 private:
  struct Cam {
    CpuImage pending;
    bool hasPending = false;
    MatteQuality quality = MatteQuality::Quality;
    std::vector<float> smooth;  // previous alpha at the current quality's size
    TextureRef latest;
    uint64_t generation = 0;    // bumps on forget/quality change; stale results are dropped
  };
  void worker();
  void infer(const std::string& id, const CpuImage& img, MatteQuality q, uint64_t generation);

  GpuDevice& gpu_;
  TexturePool& pool_;
  Converter& conv_;
  std::string name_;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
  std::string inName_, outName_;
  TextureRef opaque_;
  std::mutex m_;
  std::condition_variable cv_;
  std::map<std::string, Cam> cams_;
  std::atomic<bool> stop_{false};
  std::atomic<double> lastMs_{0};
  std::thread thread_;
};
}  // namespace bc
```

`src/engine/onnx_matte.cpp`:
```cpp
#include "engine/onnx_matte.h"

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace bc {
namespace {
struct Dims {
  int w, h;
};
Dims dimsFor(MatteQuality q) { return q == MatteQuality::Quality ? Dims{896, 512} : Dims{448, 256}; }

// DirectML must use the same physical GPU as our D3D11 device: find its DXGI index by LUID.
int dxgiIndexOf(GpuDevice& gpu) {
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
  gpu.device()->QueryInterface(IID_PPV_ARGS(&dxgiDev));
  Microsoft::WRL::ComPtr<IDXGIAdapter> ours;
  dxgiDev->GetAdapter(&ours);
  DXGI_ADAPTER_DESC od{};
  ours->GetDesc(&od);
  Microsoft::WRL::ComPtr<IDXGIFactory1> f;
  CreateDXGIFactory1(IID_PPV_ARGS(&f));
  Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
  for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    if (d.AdapterLuid.LowPart == od.AdapterLuid.LowPart && d.AdapterLuid.HighPart == od.AdapterLuid.HighPart) return static_cast<int>(i);
  }
  return 0;
}
}  // namespace

OnnxMatte::OnnxMatte(GpuDevice& gpu, TexturePool& pool, Converter& conv, std::filesystem::path modelPath)
    : gpu_(gpu), pool_(pool), conv_(conv) {
  if (!std::filesystem::exists(modelPath)) throw std::runtime_error("matting model not found: " + modelPath.string());
  env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "breadcamera");
  auto makeSession = [&](bool dml) {
    Ort::SessionOptions so;
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (dml) {
      so.DisableMemPattern();
      so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
      Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(so, dxgiIndexOf(gpu_)));
    }
    return std::make_unique<Ort::Session>(*env_, modelPath.c_str(), so);
  };
  try {
    session_ = makeSession(true);
    name_ = "onnx-directml";
  } catch (const Ort::Exception&) {
    try {
      session_ = makeSession(false);
      name_ = "onnx-cpu";
    } catch (const Ort::Exception& e) {
      throw std::runtime_error("matting model " + modelPath.string() + " could not load: " + e.what());
    }
  }
  Ort::AllocatorWithDefaultOptions alloc;
  inName_ = session_->GetInputNameAllocated(0, alloc).get();
  outName_ = session_->GetOutputNameAllocated(0, alloc).get();
  std::vector<uint8_t> ones(16 * 16, 255);
  opaque_ = pool_.acquire(16, 16, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
  {
    auto lock = gpu_.lock();
    gpu_.context()->UpdateSubresource(opaque_->tex.Get(), 0, nullptr, ones.data(), 16, 0);
  }
  thread_ = std::thread([this] { worker(); });
}

OnnxMatte::~OnnxMatte() {
  stop_ = true;
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void OnnxMatte::process(std::vector<MatteJob*>& jobs) {
  for (auto* job : jobs) {
    const auto d = dimsFor(job->quality);
    auto img = conv_.readback(*conv_.toBgra8(*job->frame, static_cast<uint32_t>(d.w), static_cast<uint32_t>(d.h)));
    std::lock_guard l(m_);
    auto& c = cams_[job->cameraId];
    if (c.quality != job->quality) {
      c.quality = job->quality;
      c.smooth.clear();
      ++c.generation;
    }
    c.pending = std::move(img);
    c.hasPending = true;
    job->alpha = c.latest ? c.latest : opaque_;
  }
  cv_.notify_one();
}

void OnnxMatte::forget(const std::string& id) {
  std::lock_guard l(m_);
  cams_.erase(id);
}

void OnnxMatte::worker() {
  std::string lastId;
  while (!stop_) {
    std::string id;
    CpuImage img;
    MatteQuality q;
    uint64_t gen;
    {
      std::unique_lock l(m_);
      cv_.wait(l, [&] {
        return stop_ || std::any_of(cams_.begin(), cams_.end(), [](auto& kv) { return kv.second.hasPending; });
      });
      if (stop_) return;
      // Round-robin: the first pending camera after the last one served.
      auto it = cams_.upper_bound(lastId);
      for (size_t n = 0; n < cams_.size(); ++n, ++it) {
        if (it == cams_.end()) it = cams_.begin();
        if (it->second.hasPending) break;
      }
      id = it->first;
      img = std::move(it->second.pending);
      it->second.hasPending = false;
      q = it->second.quality;
      gen = it->second.generation;
    }
    lastId = id;
    try {
      infer(id, img, q, gen);
    } catch (const std::exception&) {
      // A failed inference leaves the previous alpha in place; the stage guard sees no failure
      // because process() never throws for it. ponytail: surface worker errors as a stage error
      // if they turn out to happen in practice.
    }
  }
}

void OnnxMatte::infer(const std::string& id, const CpuImage& img, MatteQuality q, uint64_t generation) {
  const auto d = dimsFor(q);
  if (static_cast<int>(img.width) != d.w || static_cast<int>(img.height) != d.h) return;
  const size_t plane = static_cast<size_t>(d.w) * d.h;
  std::vector<float> input(plane * 3);
  for (size_t i = 0; i < plane; ++i) {  // BGRA bytes → RGB planes, (x/255 − 0.5)/0.5
    input[i] = img.bgra[i * 4 + 2] / 127.5f - 1.0f;
    input[plane + i] = img.bgra[i * 4 + 1] / 127.5f - 1.0f;
    input[2 * plane + i] = img.bgra[i * 4] / 127.5f - 1.0f;
  }
  const std::array<int64_t, 4> shape{1, 3, d.h, d.w};
  auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape.data(), shape.size());
  const char* in[] = {inName_.c_str()};
  const char* out[] = {outName_.c_str()};
  const auto t0 = std::chrono::steady_clock::now();
  auto result = session_->Run(Ort::RunOptions{nullptr}, in, &tensor, 1, out, 1);
  lastMs_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const float* alpha = result[0].GetTensorData<float>();

  std::vector<uint8_t> bytes(plane);
  {
    std::lock_guard l(m_);
    auto it = cams_.find(id);
    if (it == cams_.end() || it->second.generation != generation) return;  // forgotten or quality changed meanwhile
    auto& s = it->second.smooth;
    if (s.size() != plane) s.assign(alpha, alpha + plane);
    for (size_t i = 0; i < plane; ++i) {
      s[i] = 0.6f * std::clamp(alpha[i], 0.0f, 1.0f) + 0.4f * s[i];
      bytes[i] = static_cast<uint8_t>(s[i] * 255.0f + 0.5f);
    }
  }
  auto tex = pool_.acquire(static_cast<uint32_t>(d.w), static_cast<uint32_t>(d.h), DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
  {
    auto lock = gpu_.lock();
    gpu_.context()->UpdateSubresource(tex->tex.Get(), 0, nullptr, bytes.data(), static_cast<UINT>(d.w), 0);
  }
  std::lock_guard l(m_);
  auto it = cams_.find(id);
  if (it != cams_.end() && it->second.generation == generation) it->second.latest = tex;
}
}  // namespace bc
```
Note on `forget` racing `infer`: `infer` re-finds the camera by id under the lock twice and drops the result if it is gone or its generation changed, so a forgotten camera's state is never touched after `erase`.

`src/engine/matte.h`: add `virtual double lastInferenceMs() const { return 0; }` to `MatteBackend`.

`src/engine/matte.cpp` — after the Maxine branch:
```cpp
#if BC_WITH_ONNX
  {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto model = std::filesystem::path(exe).parent_path() / "models" / "modnet.onnx";
    try {
      return {std::make_unique<OnnxMatte>(gpu, pool, conv, model), ""};
    } catch (const std::exception& e) {
      return {nullptr, std::string("background removal is unavailable: ") + e.what()};
    }
  }
#endif
```
(with `#include <windows.h>`, `<filesystem>`, and `engine/onnx_matte.h` under `#if BC_WITH_ONNX`).

`src/engine/engine.cpp` — in the once-per-second block of `run()`: `if (matte_) state_.set("/engine/matteMs", matte_->lastInferenceMs());`.

- [ ] **Step 3: Run the tests**

Run: `scripts/test.ps1 -Filter "OnnxMatte*,Matte*"` → the 3 ONNX cases pass on the RX 6600 (log shows `onnx-directml` and the inference time); the M2 Matte cases still pass.

- [ ] **Step 4: Commit**

```bash
git add src/engine tests CMakeLists.txt
git commit -m "feat(engine): asynchronous MODNet matte on ONNX Runtime + DirectML as the non-NVIDIA backend"
```

---

### Task 3: M6 acceptance on the AMD build PC; follow-ups

**Files:**
- Create: `tests/e2e_m6_test.cpp`
- Modify: `docs/superpowers/follow-ups.md`, `installer/THIRD_PARTY_NOTICES.md` (ONNX Runtime, DirectML, MODNet entries already listed in M5 — confirm the versions), `CMakeLists.txt`

- [ ] **Step 1: Acceptance test**

`tests/e2e_m6_test.cpp` — the M2 acceptance (4× 1080p60 SRT, matte + blur/keyed/replace) with the **real** backend from `makeMatteBackend` instead of `TestMatte`:
```cpp
#include <doctest/doctest.h>

#include <thread>

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
    CHECK(snap["cameras"][id]["stages"]["matte"]["state"] == "active");
    CHECK(st.cameras.at(id).latencyMs <= 50.0);
  }
  for (auto& p : pushers) p->stop();
  engine.stop();
}
```
Add it to `bc_tests`. Run `scripts/test.ps1 -Filter "M6 acceptance*"` → PASS; paste the backend name, inference time and latency lines.

- [ ] **Step 2: Follow-ups**

Append to `docs/superpowers/follow-ups.md`:
```markdown
## M6
- ONNX backend uses DirectML on every vendor; spec §5.1 named TensorRT EP for NVIDIA (Maxine is NVIDIA's primary backend) (`src/engine/onnx_matte.cpp`).
- MODNet is a portrait model run frame by frame with simple EWMA smoothing: expect more edge flicker than Maxine; a person must judge quality on real cameras (human check).
- The ONNX mask trails the picture by 1–2 frames because inference is asynchronous (`src/engine/onnx_matte.h`).
- Worker inference errors keep the previous alpha and are not surfaced as stage errors (`src/engine/onnx_matte.cpp`, `worker`).
```

- [ ] **Step 3: Full verification and commit**

`scripts/test.ps1` (all), `ui/` `npm test`, `BC_FORCE_WARP=1` run (ONNX on WARP will choose `onnx-cpu` or skip — record which).

```bash
git add tests docs/superpowers/follow-ups.md CMakeLists.txt installer/THIRD_PARTY_NOTICES.md
git commit -m "test: M6 acceptance (4x1080p60 with the ONNX matte on the build PC) and follow-ups"
```
