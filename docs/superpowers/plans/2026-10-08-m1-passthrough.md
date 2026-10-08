# BreadCamera M1 — End-to-end passthrough Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A working BreadCamera skeleton: SRT cameras in → GPU decode → master tick (frame sync, hold, slate) → NDI and fullscreen out, driven by a token-protected localhost control API, a crash-safe show file, a supervising WebView2 shell and the native UI window (title bar, toolbar, Multiview, Inspector, status bar), with the show format and per-output format.

**Amended 2026-10-08 after the UI mockup review (owner):** English-only UI with Unicode names; native pro-tool window (Task 13 rewritten, Task 12 custom title bar); new Task 11A (show resolution, per-output size and half rate, input health, name limits in characters). The approved mockup is `docs/design/mockups/index.html`.

**Architecture:** Headless `breadcamera-engine.exe` (C++20, one D3D11 device, FFmpeg decode, compute-shader colour conversion, per-output worker threads behind drop-oldest queues) plus `BreadCamera.exe` (Win32 + WebView2 host, engine supervisor, tray). The React UI talks to the engine over WebSocket on 127.0.0.1. Pure logic (timing, show file, state, routing, supervision) lives in libraries with doctest unit tests; GPU code is tested on the real GPU or the WARP software adapter.

**Tech Stack:** C++20, Visual Studio 2026 (MSVC), CMake + Ninja + vcpkg (bundled with VS), Direct3D 11 + HLSL (fxc), FFmpeg 7 (LGPL build, libsrt), NDI 6 SDK (optional), ixwebsocket, nlohmann-json, doctest, WebView2 + WIL; UI: React 18 + TypeScript + Vite 6 + MUI 6 + `@bmsolutionsvn/design`, vitest.

**Spec:** `docs/superpowers/specs/2026-10-08-breadcamera-design.md` (binding; this plan argues from it).

**Branch:** `bcam-m1`, cut from `main`.

## Global Constraints

- C++20, warnings `/W4 /permissive- /utf-8`, no exceptions disabled. Build only through `scripts/build.ps1`; run tests only through `scripts/test.ps1` (they open the VS 2026 developer shell).
- FFmpeg is LGPL-only: never enable `gpl`, `nonfree`, `x264`, `x265` or any GPL feature in `vcpkg.json`. Link dynamically (`x64-windows` triplet).
- Every vendor/hardware SDK is behind `BC_WITH_<NAME>` (spec §7.2): ON when found, OFF with a status message when not. A missing SDK never fails the build.
- A test that needs an SDK, a hardware GPU or an encoder uses `BC_REQUIRES(...)` and is **skipped** (doctest skip) with a printed reason — never passed silently.
- Never black (spec §1, §3.6): every code path that produces an output frame emits the held frame or the slate, never nothing and never an all-zero frame.
- Control API binds **127.0.0.1 only**; every WebSocket connection must present the per-launch token (spec §6.1).
- Secrets (`passphrase`) are DPAPI-encrypted on disk and redacted (`{"set":true}`) in anything sent to the UI (spec §6.2).
- Signal states are exactly `ready`, `connecting`, `live`, `offline`; errors are separate `{code,message}` objects (spec §6.1). Inputs additionally report health `receiving | degraded | down | offline` (Task 11A). The UI is **English only**; names the operator types are Unicode end to end and limited in characters, not bytes. Errors are persistent blocks, never toasts; destructive actions use a confirm dialog whose primary button is a verb (spec §6.4).
- Show rate is one of `50/1`, `60000/1001`, `60/1`; show resolution is `1280x720` or `1920x1080` (spec §1.3). An output may send 1280×720 or 960×540 and/or half rate (spec §4.3).
- Commit subjects use conventional prefixes (`build:`, `feat:`, `test:`, `docs:`); one commit per task (plus fix commits).

## Review Focus

1. **An input that never connects or stalls mid-stream** (listener with no caller, caller to a dead host, network pull) — `stop()` must return within 1 s, and every output must keep receiving frames (held, then slate) — owned by Task 6 (stop-while-listening test) and Task 9 (hold→slate test).
2. **Resolution change mid-stream** (a camera switches 1080p → 720p) — every output keeps the show size with no crash, gap or stuck frame — owned by Task 11A (size-switch test).
3. **Hostile or malformed control messages** (non-JSON text, wrong types, unknown commands, huge payloads) — an error response, never a crash or a hung connection — owned by Task 11 (garbage-message test).
4. **Corrupt or hand-edited show file, or a missing slate image** — the engine still starts from last-known-good or defaults, with a visible warning in the state tree — owned by Task 3 (corrupt-main test) and Task 9 (missing-slate test).
5. **Engine restarts while the UI is open** — the UI reconnects and replaces its state from the new snapshot, with no stale cameras — owned by Task 13 (reconnect test).

## File Structure

```
CMakeLists.txt  CMakePresets.json  vcpkg.json  .gitignore
cmake/FindNDI.cmake
scripts/build.ps1  scripts/test.ps1
.github/workflows/ci.yml
src/core/      rate.h/.cpp, tick_schedule.h/.cpp, frame_sync.h, bounded_queue.h, backoff.h,
               audio_delay.h/.cpp, show.h/.cpp, secrets.h/.cpp, show_store.h/.cpp,
               signal_state.h/.cpp, state_store.h/.cpp
src/gpu/       device.h/.cpp, texture_pool.h/.cpp, converter.h/.cpp, image_io.h/.cpp,
               shaders/*.hlsl
src/io/        input.h, output.h, queued_output.h/.cpp, ffmpeg_input.h/.cpp, srt_params.h/.cpp,
               ndi_output.h/.cpp, fullscreen_output.h/.cpp, monitors.h/.cpp
src/engine/    adapter_factory.h/.cpp, camera.h/.cpp, engine.h/.cpp, preview_encoder.h/.cpp,
               preview_hub.h/.cpp, command_router.h/.cpp, control_server.h/.cpp, engine_main.cpp
src/shell/     supervisor.h/.cpp, process_launcher.h/.cpp, shell_main.cpp, shell.rc, app.ico
tests/         main.cpp, support/requires.h/.cpp, support/test_media.h/.cpp, *_test.cpp
ui/            Vite React app (Task 13)
docs/superpowers/follow-ups.md (Task 14)
```

Libraries: `bc_core` (no GPU), `bc_gpu`, `bc_io`, `bc_engine`, `bc_shell`; executables `breadcamera-engine`, `BreadCamera` (WIN32), `bc_tests`.

## Not in M1 (later milestones)

- Effects in the Inspector (Add effect, stage groups), matte, background modes — M2.
- Colour calibration in the Colour effect — M3.
- NDI/DeckLink/AJA/UVC inputs, OMT/SRT/virtual camera/SDI outputs — M4.
- Settings dialog (show rate and resolution, hold time, slate image, GPU/backend), installer, error text, soak tests — M5.

## Execution notes for the controller

- 15 tasks (1–11, 11A, 12–14) → **batched reviews** (batches: 1–3, 4–6, 7–9, 10–11A, 12–13, 14).
- The first `scripts/build.ps1` builds FFmpeg and friends through vcpkg (20–40 min). That is expected; do not cancel it.
- The NDI SDK may be missing on the build PC. Task 8 must still compile with `BC_WITH_NDI=OFF` and its tests must report as skipped.

---

### Task 1: Repository scaffold, build scripts and `Rate`

**Files:**
- Create: `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `.gitignore`, `cmake/FindNDI.cmake`, `scripts/build.ps1`, `scripts/test.ps1`, `.github/workflows/ci.yml`
- Create: `src/core/rate.h`, `src/core/rate.cpp`
- Create: `tests/main.cpp`, `tests/support/requires.h`, `tests/support/requires.cpp`, `tests/rate_test.cpp`

**Interfaces:**
- Produces: `bc::Rate { int num; int den; double fps() const; std::chrono::nanoseconds period() const; std::string str() const; }`, `std::optional<bc::Rate> bc::parseShowRate(std::string_view)`; `BC_REQUIRES(bc::test::Req::X)` doctest decorator with `Req { HardwareGpu, Ndi, H264Encoder }`; CMake targets `bc_core`, `bc_tests`; options `BC_WITH_NDI`.

- [ ] **Step 1: Write `.gitignore`, `vcpkg.json`, `CMakePresets.json`**

`.gitignore`:
```
/build/
/ui/node_modules/
/ui/dist/
/.superpowers/
*.user
```

`vcpkg.json`:
```json
{
  "name": "breadcamera",
  "version-string": "0.1.0",
  "dependencies": [
    {
      "name": "ffmpeg",
      "default-features": false,
      "features": ["avcodec", "avformat", "swresample", "swscale", "srt", "nvcodec", "amf"]
    },
    "ixwebsocket",
    "nlohmann-json",
    "doctest",
    "webview2",
    "wil"
  ]
}
```

`CMakePresets.json`:
```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "base",
      "hidden": true,
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build/${presetName}",
      "toolchainFile": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
      "cacheVariables": { "VCPKG_TARGET_TRIPLET": "x64-windows" }
    },
    { "name": "release", "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" } },
    { "name": "debug", "inherits": "base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } }
  ],
  "buildPresets": [
    { "name": "release", "configurePreset": "release" },
    { "name": "debug", "configurePreset": "debug" }
  ]
}
```

- [ ] **Step 2: Add the vcpkg baseline**

Run (bash): `powershell -NoProfile -Command "& 'C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg\vcpkg.exe' x-update-baseline --add-initial-baseline"` in the repo root.
Expected: `vcpkg.json` gains a `"builtin-baseline": "<40-hex sha>"` line. If the bundled vcpkg refuses (`vcpkg-configuration` or registry error), run it inside the dev shell from `scripts/build.ps1`'s first three lines and ledger the command used.

- [ ] **Step 3: Write the build scripts**

`scripts/build.ps1`:
```powershell
param([ValidateSet('release', 'debug')][string]$Preset = 'release')
$ErrorActionPreference = 'Stop'
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "$vs\VC\vcpkg" }
Set-Location (Join-Path $PSScriptRoot '..')
cmake --preset $Preset
if ($LASTEXITCODE) { exit $LASTEXITCODE }
cmake --build --preset $Preset
exit $LASTEXITCODE
```

`scripts/test.ps1`:
```powershell
param(
  [ValidateSet('release', 'debug')][string]$Preset = 'release',
  [string]$Filter = ''
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -Preset $Preset
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$exe = Join-Path $PSScriptRoot "..\build\$Preset\bc_tests.exe"
if ($Filter) { & $exe "--test-case=$Filter" } else { & $exe }
exit $LASTEXITCODE
```

From bash, run them as `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Rate*"`.

- [ ] **Step 4: Write `cmake/FindNDI.cmake`**

```cmake
# Finds the NDI 6 SDK (spec §7.2: optional). Defines NDI_FOUND, NDI::NDI and NDI_DLL.
set(_ndi_root "$ENV{NDI_SDK_DIR}")
if(NOT _ndi_root)
  set(_ndi_root "C:/Program Files/NDI/NDI 6 SDK")
endif()
find_path(NDI_INCLUDE_DIR Processing.NDI.Lib.h PATHS "${_ndi_root}/Include" NO_DEFAULT_PATH)
find_library(NDI_LIBRARY Processing.NDI.Lib.x64 PATHS "${_ndi_root}/Lib/x64" NO_DEFAULT_PATH)
find_file(NDI_DLL Processing.NDI.Lib.x64.dll PATHS "${_ndi_root}/Bin/x64" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(NDI DEFAULT_MSG NDI_INCLUDE_DIR NDI_LIBRARY NDI_DLL)
if(NDI_FOUND AND NOT TARGET NDI::NDI)
  add_library(NDI::NDI UNKNOWN IMPORTED)
  set_target_properties(NDI::NDI PROPERTIES
    IMPORTED_LOCATION "${NDI_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${NDI_INCLUDE_DIR}")
endif()
```

- [ ] **Step 5: Write the root `CMakeLists.txt`**

Later tasks append their sources to the lists marked `# Task N`; keep the list structure.

```cmake
cmake_minimum_required(VERSION 3.28)
project(BreadCamera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
add_compile_options(/W4 /permissive- /utf-8 /EHsc)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)

find_package(FFMPEG REQUIRED)
find_package(ixwebsocket CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_package(unofficial-webview2 CONFIG REQUIRED)
find_package(wil CONFIG REQUIRED)

# Optional SDKs (spec §7.2)
find_package(NDI)
option(BC_WITH_NDI "Build the NDI adapters" ${NDI_FOUND})
if(BC_WITH_NDI AND NOT NDI_FOUND)
  message(WARNING "BC_WITH_NDI=ON but the NDI SDK was not found; turning it off")
  set(BC_WITH_NDI OFF)
endif()
message(STATUS "BC_WITH_NDI=${BC_WITH_NDI}")

# ---- core (no GPU) ----
add_library(bc_core STATIC
  src/core/rate.cpp
  # Task 2
  # Task 3
  # Task 4
)
target_include_directories(bc_core PUBLIC src)
target_link_libraries(bc_core PUBLIC nlohmann_json::nlohmann_json)

# ---- tests ----
add_executable(bc_tests
  tests/main.cpp
  tests/support/requires.cpp
  tests/rate_test.cpp
  # Task 2..14 test files
)
target_include_directories(bc_tests PRIVATE tests)
target_link_libraries(bc_tests PRIVATE bc_core doctest::doctest)
target_compile_definitions(bc_tests PRIVATE BC_WITH_NDI=$<BOOL:${BC_WITH_NDI}>)
```

- [ ] **Step 6: Write the doctest main and the requirement decorator**

`tests/main.cpp`:
```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

`tests/support/requires.h`:
```cpp
#pragma once
#include <doctest/doctest.h>

namespace bc::test {
enum class Req { HardwareGpu, Ndi, H264Encoder };
// True when the requirement is missing. Prints the reason to stderr once per requirement
// so a skipped test is visible in the log (spec §7.2).
bool missing(Req r);
}  // namespace bc::test

#define BC_REQUIRES(r) *doctest::skip(::bc::test::missing(r))
```

`tests/support/requires.cpp` (later tasks fill in the `HardwareGpu` and `H264Encoder` checks; Task 1 ships them as "missing" so nothing depends on them yet):
```cpp
#include "support/requires.h"

#include <cstdio>
#include <mutex>
#include <set>

namespace bc::test {
namespace {
bool check(Req r, const char*& reason) {
  switch (r) {
    case Req::Ndi:
      reason = "NDI SDK not built (BC_WITH_NDI=OFF)";
      return BC_WITH_NDI == 0;
    case Req::HardwareGpu:
      reason = "no hardware GPU check yet (Task 5)";
      return true;
    case Req::H264Encoder:
      reason = "no H.264 encoder check yet (Task 10)";
      return true;
  }
  return true;
}
}  // namespace

bool missing(Req r) {
  static std::mutex m;
  static std::set<int> printed;
  const char* reason = "";
  bool miss = check(r, reason);
  std::lock_guard lock(m);
  if (miss && printed.insert(static_cast<int>(r)).second) {
    std::fprintf(stderr, "[skip] requirement %d missing: %s\n", static_cast<int>(r), reason);
  }
  return miss;
}
}  // namespace bc::test
```

- [ ] **Step 7: Write the failing `Rate` test**

`tests/rate_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "core/rate.h"

using namespace std::chrono_literals;

TEST_CASE("Rate: parseShowRate accepts only the three show rates") {
  CHECK(bc::parseShowRate("60/1") == bc::Rate{60, 1});
  CHECK(bc::parseShowRate("50/1") == bc::Rate{50, 1});
  CHECK(bc::parseShowRate("60000/1001") == bc::Rate{60000, 1001});
  CHECK_FALSE(bc::parseShowRate("30/1"));
  CHECK_FALSE(bc::parseShowRate("60"));
  CHECK_FALSE(bc::parseShowRate("60/0"));
  CHECK_FALSE(bc::parseShowRate("abc/1"));
  CHECK_FALSE(bc::parseShowRate(""));
}

TEST_CASE("Rate: period and string form") {
  CHECK(bc::Rate{60, 1}.period() == 16'666'666ns);
  CHECK(bc::Rate{50, 1}.period() == 20ms);
  CHECK(bc::Rate{60000, 1001}.period() == 16'683'333ns);
  CHECK(bc::Rate{60000, 1001}.str() == "60000/1001");
  CHECK(bc::Rate{60000, 1001}.fps() == doctest::Approx(59.94).epsilon(0.001));
}
```

- [ ] **Step 8: Run it to verify it fails**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Rate*"`
Expected: build FAILS with `cannot open include file 'core/rate.h'` (after the vcpkg dependency build completes).

- [ ] **Step 9: Implement `Rate`**

`src/core/rate.h`:
```cpp
#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bc {
struct Rate {
  int num = 60;
  int den = 1;
  double fps() const { return static_cast<double>(num) / den; }
  // One tick, truncated to whole nanoseconds. Use TickSchedule for exact offsets.
  std::chrono::nanoseconds period() const {
    return std::chrono::nanoseconds(1'000'000'000LL * den / num);
  }
  std::string str() const { return std::to_string(num) + "/" + std::to_string(den); }
  bool operator==(const Rate&) const = default;
};

// Accepts "50/1", "60000/1001" or "60/1" only (spec §1.3).
std::optional<Rate> parseShowRate(std::string_view s);
}  // namespace bc
```

`src/core/rate.cpp`:
```cpp
#include "core/rate.h"

#include <charconv>

namespace bc {
std::optional<Rate> parseShowRate(std::string_view s) {
  auto slash = s.find('/');
  if (slash == std::string_view::npos) return std::nullopt;
  int num = 0, den = 0;
  auto a = s.substr(0, slash), b = s.substr(slash + 1);
  auto ra = std::from_chars(a.data(), a.data() + a.size(), num);
  auto rb = std::from_chars(b.data(), b.data() + b.size(), den);
  // The whole substring must be consumed, so "60x/1" is rejected.
  if (ra.ec != std::errc{} || ra.ptr != a.data() + a.size() ||
      rb.ec != std::errc{} || rb.ptr != b.data() + b.size()) {
    return std::nullopt;
  }
  Rate r{num, den};
  if (r == Rate{50, 1} || r == Rate{60000, 1001} || r == Rate{60, 1}) return r;
  return std::nullopt;
}
}  // namespace bc
```

- [ ] **Step 10: Add the CI workflow**

`.github/workflows/ci.yml`:
```yaml
name: ci
on: [push, pull_request]
jobs:
  build:
    runs-on: windows-2025
    env:
      BC_FORCE_WARP: "1"
    steps:
      - uses: actions/checkout@v4
      - uses: ilammy/msvc-dev-cmd@v1
      - name: Build and test
        shell: pwsh
        run: |
          $env:VCPKG_ROOT = $env:VCPKG_INSTALLATION_ROOT
          cmake --preset release
          cmake --build --preset release
          ./build/release/bc_tests.exe
```

- [ ] **Step 11: Run the test to verify it passes**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Rate*"`
Expected: `[doctest] test cases: 2 | 2 passed`. The configure log shows `BC_WITH_NDI=ON` or `OFF`.

- [ ] **Step 12: Commit**

```bash
git add .gitignore vcpkg.json CMakePresets.json CMakeLists.txt cmake scripts .github src/core/rate.h src/core/rate.cpp tests
git commit -m "build: scaffold CMake/vcpkg project, test harness and show Rate"
```

### Task 2: Timing primitives — TickSchedule, FrameSync, BoundedQueue, Backoff, AudioDelay

**Files:**
- Create: `src/core/tick_schedule.h`, `src/core/tick_schedule.cpp`, `src/core/frame_sync.h`, `src/core/bounded_queue.h`, `src/core/backoff.h`, `src/core/audio_delay.h`, `src/core/audio_delay.cpp`
- Modify: `CMakeLists.txt` (add `src/core/tick_schedule.cpp src/core/audio_delay.cpp` under `# Task 2`; add the test files to `bc_tests`)
- Test: `tests/tick_schedule_test.cpp`, `tests/frame_sync_test.cpp`, `tests/bounded_queue_test.cpp`, `tests/audio_delay_test.cpp`

**Interfaces:**
- Consumes: `bc::Rate` (Task 1).
- Produces:
  - `bc::TickSchedule(Rate)`: `std::chrono::nanoseconds offset(uint64_t index) const`, `uint64_t nextIndexAtOrAfter(std::chrono::nanoseconds elapsed) const`, `uint32_t audioFramesForTick(uint64_t index, int sampleRate) const`, `Rate rate() const`.
  - `bc::FeedPhase { NoSignal, Live, Holding, Slate }`, `const char* bc::toString(FeedPhase)`, `bc::SyncPolicy { milliseconds stallAfter{250}; milliseconds hold{5000}; }`, `template<class F> bc::FrameSync<F>` with `push(F, time_point)`, `Pick pick(time_point)` (`Pick { std::optional<F> frame; FeedPhase phase; bool fresh; }`), `setPolicy`, `drops()`, `repeats()`, `reset()`.
  - `template<class T> bc::BoundedQueue<T>(size_t)`: `bool push(T)` (true = oldest dropped), `std::optional<T> pop(milliseconds)`, `void close()`, `void reopen()`, `uint64_t dropped() const`.
  - `bc::Backoff(milliseconds initial = 500ms, milliseconds max = 5000ms)`: `milliseconds next()`, `void reset()`.
  - `bc::AudioDelay(int sampleRate, int channels)`: `setDelay(milliseconds)`, `push(std::span<const float>)`, `std::vector<float> pull(size_t frames)`, `size_t bufferedFrames() const`, `uint64_t underruns() const`.

- [ ] **Step 1: Write the failing tests**

`tests/tick_schedule_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "core/tick_schedule.h"

using namespace std::chrono_literals;

TEST_CASE("TickSchedule: offsets are exact, with no accumulated rounding") {
  bc::TickSchedule s({60000, 1001});
  CHECK(s.offset(0) == 0ns);
  CHECK(s.offset(60000) == std::chrono::nanoseconds(1001'000'000'000LL));  // exactly 1001 s
  CHECK(s.offset(1) == 16'683'333ns);
  bc::TickSchedule s60({60, 1});
  CHECK(s60.offset(60) == 1s);
  CHECK(s60.offset(3) == 50ms);
}

TEST_CASE("TickSchedule: nextIndexAtOrAfter skips missed ticks instead of bursting") {
  bc::TickSchedule s({60, 1});
  CHECK(s.nextIndexAtOrAfter(0ns) == 0);
  CHECK(s.nextIndexAtOrAfter(1ns) == 1);
  CHECK(s.nextIndexAtOrAfter(50ms) == 3);  // exactly on tick 3
  CHECK(s.nextIndexAtOrAfter(51ms) == 4);
}

TEST_CASE("TickSchedule: audio frames per tick sum exactly to the sample rate") {
  bc::TickSchedule s({60000, 1001});
  uint64_t sum = 0;
  for (uint64_t i = 0; i < 5; ++i) sum += s.audioFramesForTick(i, 48000);
  CHECK(sum == 4004);  // 48000 * 1001 * 5 / 60000
  bc::TickSchedule s60({60, 1});
  CHECK(s60.audioFramesForTick(0, 48000) == 800);
}
```

`tests/frame_sync_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "core/frame_sync.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST_CASE("FrameSync: no frame yet is NoSignal with no frame") {
  bc::FrameSync<int> sync({250ms, 5000ms});
  auto p = sync.pick(Clock::now());
  CHECK(p.phase == bc::FeedPhase::NoSignal);
  CHECK_FALSE(p.frame);
}

TEST_CASE("FrameSync: newest frame wins, older unpicked frames count as drops") {
  bc::FrameSync<int> sync({250ms, 5000ms});
  auto t0 = Clock::now();
  sync.push(1, t0);
  sync.push(2, t0);
  auto p = sync.pick(t0);
  CHECK(p.frame == 2);
  CHECK(p.fresh);
  CHECK(p.phase == bc::FeedPhase::Live);
  CHECK(sync.drops() == 1);
}

TEST_CASE("FrameSync: a slow input repeats its last frame") {
  bc::FrameSync<int> sync({250ms, 5000ms});
  auto t0 = Clock::now();
  sync.push(7, t0);
  sync.pick(t0);
  auto p = sync.pick(t0 + 16ms);
  CHECK(p.frame == 7);
  CHECK_FALSE(p.fresh);
  CHECK(sync.repeats() == 1);
}

TEST_CASE("FrameSync: stalled input holds, then goes to slate") {
  bc::FrameSync<int> sync({250ms, 1000ms});
  auto t0 = Clock::now();
  sync.push(3, t0);
  CHECK(sync.pick(t0 + 100ms).phase == bc::FeedPhase::Live);
  auto hold = sync.pick(t0 + 300ms);
  CHECK(hold.phase == bc::FeedPhase::Holding);
  CHECK(hold.frame == 3);
  auto slate = sync.pick(t0 + 1300ms);
  CHECK(slate.phase == bc::FeedPhase::Slate);
  CHECK_FALSE(slate.frame);
  sync.push(4, t0 + 1400ms);
  CHECK(sync.pick(t0 + 1400ms).phase == bc::FeedPhase::Live);
}

TEST_CASE("FrameSync: reset forgets the last frame") {
  bc::FrameSync<int> sync({250ms, 1000ms});
  auto t0 = Clock::now();
  sync.push(3, t0);
  sync.reset();
  CHECK(sync.pick(t0).phase == bc::FeedPhase::NoSignal);
}
```

`tests/bounded_queue_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <thread>

#include "core/backoff.h"
#include "core/bounded_queue.h"

using namespace std::chrono_literals;

TEST_CASE("BoundedQueue: drops the oldest item when full and never blocks") {
  bc::BoundedQueue<int> q(2);
  CHECK_FALSE(q.push(1));
  CHECK_FALSE(q.push(2));
  CHECK(q.push(3));  // drops 1
  CHECK(q.dropped() == 1);
  CHECK(q.pop(0ms) == 2);
  CHECK(q.pop(0ms) == 3);
  CHECK_FALSE(q.pop(0ms));
}

TEST_CASE("BoundedQueue: pop wakes on push and returns nullopt after close") {
  bc::BoundedQueue<int> q(2);
  std::thread t([&] {
    std::this_thread::sleep_for(20ms);
    q.push(5);
  });
  CHECK(q.pop(1000ms) == 5);
  t.join();
  std::thread c([&] {
    std::this_thread::sleep_for(20ms);
    q.close();
  });
  auto start = std::chrono::steady_clock::now();
  CHECK_FALSE(q.pop(5000ms));
  CHECK(std::chrono::steady_clock::now() - start < 1000ms);
  c.join();
}

TEST_CASE("Backoff: doubles up to the cap and resets") {
  bc::Backoff b(500ms, 5000ms);
  CHECK(b.next() == 500ms);
  CHECK(b.next() == 1000ms);
  CHECK(b.next() == 2000ms);
  CHECK(b.next() == 4000ms);
  CHECK(b.next() == 5000ms);
  CHECK(b.next() == 5000ms);
  b.reset();
  CHECK(b.next() == 500ms);
}
```

`tests/audio_delay_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <numeric>

#include "core/audio_delay.h"

using namespace std::chrono_literals;

TEST_CASE("AudioDelay: releases samples after the delay, silence before") {
  bc::AudioDelay d(1000, 1);  // 1 kHz mono: 10 ms = 10 frames
  d.setDelay(10ms);
  std::vector<float> in(20);
  std::iota(in.begin(), in.end(), 1.0f);
  d.push(in);
  CHECK(d.pull(10) == std::vector<float>(10, 0.0f));
  CHECK(d.pull(5) == std::vector<float>{1, 2, 3, 4, 5});
}

TEST_CASE("AudioDelay: underrun pads silence and is counted") {
  bc::AudioDelay d(1000, 2);
  d.setDelay(0ms);
  d.push(std::vector<float>{1, 1, 2, 2});
  CHECK(d.pull(4) == std::vector<float>{1, 1, 2, 2, 0, 0, 0, 0});
  CHECK(d.underruns() == 1);
}

TEST_CASE("AudioDelay: a buffer more than 100 ms over the delay is trimmed back to the delay") {
  bc::AudioDelay d(1000, 1);
  d.setDelay(10ms);
  d.push(std::vector<float>(500, 1.0f));
  CHECK(d.bufferedFrames() == 10);
}
```

Add the four test files to `bc_tests` in `CMakeLists.txt`.

- [ ] **Step 2: Run the tests to verify they fail**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1`
Expected: build FAILS — `core/tick_schedule.h` not found.

- [ ] **Step 3: Implement `TickSchedule`**

`src/core/tick_schedule.h`:
```cpp
#pragma once
#include <chrono>
#include <cstdint>

#include "core/rate.h"

namespace bc {
// Exact tick deadlines for the master clock (spec §3.3). Offsets are computed from the
// tick index, so rounding never accumulates over a long show.
class TickSchedule {
 public:
  explicit TickSchedule(Rate r) : rate_(r) {}
  Rate rate() const { return rate_; }
  std::chrono::nanoseconds offset(uint64_t index) const;
  // First index whose offset is >= elapsed. After an overrun the tick loop jumps here
  // instead of running the missed ticks back to back.
  uint64_t nextIndexAtOrAfter(std::chrono::nanoseconds elapsed) const;
  // Audio sample frames that belong to tick `index`, so the total per second is exact
  // (800.8 per tick at 59.94).
  uint32_t audioFramesForTick(uint64_t index, int sampleRate) const;

 private:
  Rate rate_;
};
}  // namespace bc
```

`src/core/tick_schedule.cpp`:
```cpp
#include "core/tick_schedule.h"

#include <cmath>

namespace bc {
std::chrono::nanoseconds TickSchedule::offset(uint64_t index) const {
  // index * den / num seconds, split to avoid 64-bit overflow.
  const uint64_t num = static_cast<uint64_t>(rate_.num);
  const uint64_t scaled = index * static_cast<uint64_t>(rate_.den);
  const uint64_t secs = scaled / num;
  const uint64_t rem = scaled % num;
  return std::chrono::nanoseconds(secs * 1'000'000'000ULL + rem * 1'000'000'000ULL / num);
}

uint64_t TickSchedule::nextIndexAtOrAfter(std::chrono::nanoseconds elapsed) const {
  if (elapsed.count() <= 0) return 0;
  long double approx = static_cast<long double>(elapsed.count()) * rate_.num /
                       (static_cast<long double>(rate_.den) * 1e9L);
  uint64_t idx = static_cast<uint64_t>(std::ceil(approx));
  while (offset(idx) < elapsed) ++idx;
  while (idx > 0 && offset(idx - 1) >= elapsed) --idx;
  return idx;
}

uint32_t TickSchedule::audioFramesForTick(uint64_t index, int sampleRate) const {
  auto total = [&](uint64_t i) {
    return i * static_cast<uint64_t>(sampleRate) * static_cast<uint64_t>(rate_.den) /
           static_cast<uint64_t>(rate_.num);
  };
  return static_cast<uint32_t>(total(index + 1) - total(index));
}
}  // namespace bc
```

- [ ] **Step 4: Implement `FrameSync`, `BoundedQueue`, `Backoff`**

`src/core/frame_sync.h`:
```cpp
#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace bc {
enum class FeedPhase { NoSignal, Live, Holding, Slate };

inline const char* toString(FeedPhase p) {
  switch (p) {
    case FeedPhase::NoSignal: return "nosignal";
    case FeedPhase::Live: return "live";
    case FeedPhase::Holding: return "holding";
    case FeedPhase::Slate: return "slate";
  }
  return "nosignal";
}

struct SyncPolicy {
  std::chrono::milliseconds stallAfter{250};  // no new frame for this long = input stalled
  std::chrono::milliseconds hold{5000};       // hold the last frame this long, then slate
};

// Latest-frame slot between an input thread and the master tick (spec §3.3).
// The tick always takes the newest frame; slower inputs repeat, faster ones drop.
template <class Frame>
class FrameSync {
 public:
  using Clock = std::chrono::steady_clock;
  struct Pick {
    std::optional<Frame> frame;  // empty for NoSignal and Slate
    FeedPhase phase;
    bool fresh;                  // true when this pick took a frame not seen before
  };

  explicit FrameSync(SyncPolicy p) : policy_(p) {}

  void setPolicy(SyncPolicy p) {
    std::lock_guard l(m_);
    policy_ = p;
  }

  void push(Frame f, Clock::time_point arrival) {
    std::lock_guard l(m_);
    if (pending_) ++drops_;
    pending_ = std::move(f);
    lastArrival_ = arrival;
  }

  Pick pick(Clock::time_point now) {
    std::lock_guard l(m_);
    bool fresh = false;
    if (pending_) {
      current_ = std::move(pending_);
      pending_.reset();
      fresh = true;
    } else if (current_) {
      ++repeats_;
    }
    FeedPhase phase = FeedPhase::NoSignal;
    if (lastArrival_) {
      auto gap = now - *lastArrival_;
      if (gap <= policy_.stallAfter) phase = FeedPhase::Live;
      else if (gap <= policy_.stallAfter + policy_.hold) phase = FeedPhase::Holding;
      else phase = FeedPhase::Slate;
    }
    std::optional<Frame> out;
    if (phase == FeedPhase::Live || phase == FeedPhase::Holding) out = current_;
    return {std::move(out), phase, fresh};
  }

  void reset() {
    std::lock_guard l(m_);
    pending_.reset();
    current_.reset();
    lastArrival_.reset();
  }

  uint64_t drops() const {
    std::lock_guard l(m_);
    return drops_;
  }
  uint64_t repeats() const {
    std::lock_guard l(m_);
    return repeats_;
  }

 private:
  mutable std::mutex m_;
  SyncPolicy policy_;
  std::optional<Frame> pending_;
  std::optional<Frame> current_;
  std::optional<Clock::time_point> lastArrival_;
  uint64_t drops_ = 0;
  uint64_t repeats_ = 0;
};
}  // namespace bc
```

`src/core/bounded_queue.h`:
```cpp
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace bc {
// Drop-oldest queue between the tick and one output (spec §4.1). push() never blocks,
// so a slow consumer can never slow the producer.
template <class T>
class BoundedQueue {
 public:
  explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

  bool push(T v) {
    bool dropped = false;
    {
      std::lock_guard l(m_);
      if (closed_) return false;
      if (q_.size() >= capacity_) {
        q_.pop_front();
        ++dropped_;
        dropped = true;
      }
      q_.push_back(std::move(v));
    }
    cv_.notify_one();
    return dropped;
  }

  std::optional<T> pop(std::chrono::milliseconds timeout) {
    std::unique_lock l(m_);
    cv_.wait_for(l, timeout, [&] { return closed_ || !q_.empty(); });
    if (q_.empty()) return std::nullopt;
    T v = std::move(q_.front());
    q_.pop_front();
    return v;
  }

  void close() {
    {
      std::lock_guard l(m_);
      closed_ = true;
      q_.clear();
    }
    cv_.notify_all();
  }

  void reopen() {
    std::lock_guard l(m_);
    closed_ = false;
  }

  uint64_t dropped() const {
    std::lock_guard l(m_);
    return dropped_;
  }

 private:
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<T> q_;
  size_t capacity_;
  bool closed_ = false;
  uint64_t dropped_ = 0;
};
}  // namespace bc
```

`src/core/backoff.h`:
```cpp
#pragma once
#include <algorithm>
#include <chrono>

namespace bc {
// Reconnect delay: initial, doubling, capped (spec §3.6).
class Backoff {
 public:
  explicit Backoff(std::chrono::milliseconds initial = std::chrono::milliseconds(500),
                   std::chrono::milliseconds max = std::chrono::milliseconds(5000))
      : initial_(initial), max_(max), next_(initial) {}
  std::chrono::milliseconds next() {
    auto d = next_;
    next_ = std::min(next_ * 2, max_);
    return d;
  }
  void reset() { next_ = initial_; }

 private:
  std::chrono::milliseconds initial_, max_, next_;
};
}  // namespace bc
```

- [ ] **Step 5: Implement `AudioDelay`**

`src/core/audio_delay.h`:
```cpp
#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

namespace bc {
// Interleaved float FIFO that releases audio `delay` after it arrives, so it stays in
// sync with the delayed video (spec §3.5).
// ponytail: no clock-drift resampling; a buffer that grows 100 ms past the delay is
// trimmed (one audible skip). Add an adaptive resampler if long shows drift audibly.
class AudioDelay {
 public:
  AudioDelay(int sampleRate, int channels) : rate_(sampleRate), channels_(channels) {}
  void setDelay(std::chrono::milliseconds d);
  void push(std::span<const float> interleaved);
  std::vector<float> pull(size_t frames);
  size_t bufferedFrames() const;
  uint64_t underruns() const;
  int sampleRate() const { return rate_; }
  int channels() const { return channels_; }

 private:
  size_t delayFrames() const { return static_cast<size_t>(rate_) * delay_.count() / 1000; }
  mutable std::mutex m_;
  int rate_;
  int channels_;
  std::chrono::milliseconds delay_{0};
  std::deque<float> buf_;
  uint64_t underruns_ = 0;
};
}  // namespace bc
```

`src/core/audio_delay.cpp`:
```cpp
#include "core/audio_delay.h"

#include <algorithm>

namespace bc {
void AudioDelay::setDelay(std::chrono::milliseconds d) {
  std::lock_guard l(m_);
  delay_ = d;
  buf_.assign(delayFrames() * channels_, 0.0f);  // prime with silence
}

void AudioDelay::push(std::span<const float> interleaved) {
  std::lock_guard l(m_);
  buf_.insert(buf_.end(), interleaved.begin(), interleaved.end());
  const size_t maxFrames = delayFrames() + static_cast<size_t>(rate_) / 10;
  if (buf_.size() / channels_ > maxFrames) {
    const size_t excess = buf_.size() - delayFrames() * channels_;
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(excess));
  }
}

std::vector<float> AudioDelay::pull(size_t frames) {
  std::lock_guard l(m_);
  std::vector<float> out(frames * channels_, 0.0f);
  const size_t have = std::min(out.size(), buf_.size());
  std::copy(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(have), out.begin());
  buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(have));
  if (have < out.size()) ++underruns_;
  return out;
}

size_t AudioDelay::bufferedFrames() const {
  std::lock_guard l(m_);
  return buf_.size() / channels_;
}

uint64_t AudioDelay::underruns() const {
  std::lock_guard l(m_);
  return underruns_;
}
}  // namespace bc
```

The trim test: delay 10 frames, push 500 → 510 buffered > 10 + 100 → trimmed to exactly 10 frames.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1`
Expected: all test cases pass (Rate 2, TickSchedule 3, FrameSync 5, BoundedQueue/Backoff 3, AudioDelay 3).

- [ ] **Step 7: Commit**

```bash
git add src/core tests CMakeLists.txt
git commit -m "feat(core): tick schedule, frame sync, bounded queue, backoff, audio delay"
```

### Task 3: Show file — model, JSON, DPAPI secrets, atomic store with last-known-good and autosave

**Files:**
- Create: `src/core/show.h`, `src/core/show.cpp`, `src/core/secrets.h`, `src/core/secrets.cpp`, `src/core/show_store.h`, `src/core/show_store.cpp`
- Create: `tests/support/temp_dir.h`
- Modify: `CMakeLists.txt` (add the three `.cpp` under `# Task 3`; `target_link_libraries(bc_core PUBLIC crypt32)`; add tests)
- Test: `tests/show_test.cpp`, `tests/show_store_test.cpp`

**Interfaces:**
- Consumes: `bc::Rate`, `bc::parseShowRate` (Task 1).
- Produces:
  - `bc::InputConfig { std::string kind = "srt"; nlohmann::json params; }`, `bc::OutputConfig { std::string id, kind; bool keyed = false; bool enabled = true; nlohmann::json params; }`, `bc::CameraConfig { std::string id, name; InputConfig input; std::vector<OutputConfig> outputs; }`, `bc::ShowSettings { Rate rate{60,1}; int holdMs = 5000; std::string slatePath; }`, `bc::Show { static constexpr int kSchemaVersion = 1; ShowSettings settings; std::vector<CameraConfig> cameras; CameraConfig* findCamera(std::string_view); }` — all with `operator==`.
  - `bc::ShowError : std::runtime_error`; `enum class bc::SecretMode { Encrypt, Redact, Plain }`; `nlohmann::json bc::showToJson(const Show&, SecretMode)`; `bc::Show bc::showFromJson(const nlohmann::json&)`; `nlohmann::json bc::cameraToJson(const CameraConfig&, SecretMode)`; `bc::CameraConfig bc::cameraFromJson(const nlohmann::json&)`; `nlohmann::json bc::outputToJson(const OutputConfig&)`; `bc::OutputConfig bc::outputFromJson(const nlohmann::json&)`; `std::string bc::newId()`; `bc::kSecretKeys`.
  - `std::string bc::protectSecret(std::string_view)`, `std::string bc::unprotectSecret(std::string_view)`.
  - `bc::ShowStore(std::filesystem::path dir, std::chrono::milliseconds debounce = 500ms)`: `Loaded load()` (`Loaded { Show show; From from; std::string warning; }`, `From { Main, LastKnownGood, Default }`), `void saveNow(const Show&)`, `void scheduleSave(Show)`, `void flush()`, `uint64_t saveCount() const`, `std::string lastSaveError() const`, `mainPath()`, `lkgPath()`.

On-disk format (spec §6.2):
```json
{
  "schemaVersion": 1,
  "settings": { "rate": "60/1", "holdMs": 5000, "slatePath": "" },
  "cameras": [
    {
      "id": "a1b2c3d4",
      "name": "Cam 1",
      "input": { "kind": "srt", "params": { "mode": "listener", "port": 9000, "passphrase": { "$dpapi": "AQAAANCM..." } } },
      "outputs": [ { "id": "e5f6a7b8", "kind": "ndi", "keyed": false, "enabled": true, "params": { "name": "BreadCamera - Cam 1" } } ]
    }
  ]
}
```
A secret is any `params` key listed in `kSecretKeys` (`"passphrase"`). `Encrypt` writes `{"$dpapi": "<base64>"}` (empty string stays `""`); `Redact` writes `{"set": true}` or `{"set": false}`; `Plain` writes the string. `showFromJson` accepts a plain string or `{"$dpapi": ...}` and rejects any other shape.

- [ ] **Step 1: Write the test helper and the failing tests**

`tests/support/temp_dir.h`:
```cpp
#pragma once
#include <filesystem>
#include <random>
#include <string>

namespace bc::test {
// A unique folder under %TEMP%, removed on destruction.
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    std::random_device rd;
    path = std::filesystem::temp_directory_path() / ("bc-test-" + std::to_string(rd()));
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
}  // namespace bc::test
```

`tests/show_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "core/secrets.h"
#include "core/show.h"

namespace {
bc::Show sampleShow() {
  bc::Show s;
  bc::CameraConfig cam;
  cam.id = "cam1";
  cam.name = "Cam 1";
  cam.input.kind = "srt";
  cam.input.params = {{"mode", "listener"}, {"port", 9000}, {"passphrase", "supersecret123"}};
  cam.outputs.push_back({"out1", "ndi", false, true, {{"name", "BreadCamera - Cam 1"}}});
  s.cameras.push_back(cam);
  return s;
}
}  // namespace

TEST_CASE("Secrets: DPAPI round-trip, and garbage is rejected") {
  auto enc = bc::protectSecret("hello world");
  CHECK(enc.find("hello") == std::string::npos);
  CHECK(bc::unprotectSecret(enc) == "hello world");
  CHECK_THROWS(bc::unprotectSecret("not-base64!!"));
}

TEST_CASE("Show: JSON round-trip encrypts secrets on disk") {
  auto s = sampleShow();
  auto j = bc::showToJson(s, bc::SecretMode::Encrypt);
  auto text = j.dump();
  CHECK(text.find("supersecret123") == std::string::npos);
  CHECK(j["cameras"][0]["input"]["params"]["passphrase"].contains("$dpapi"));
  CHECK(bc::showFromJson(j) == s);
}

TEST_CASE("Show: redacted JSON never carries the secret") {
  auto j = bc::showToJson(sampleShow(), bc::SecretMode::Redact);
  CHECK(j["cameras"][0]["input"]["params"]["passphrase"] == nlohmann::json{{"set", true}});
  bc::Show empty = sampleShow();
  empty.cameras[0].input.params["passphrase"] = "";
  auto j2 = bc::showToJson(empty, bc::SecretMode::Redact);
  CHECK(j2["cameras"][0]["input"]["params"]["passphrase"] == nlohmann::json{{"set", false}});
}

TEST_CASE("Show: validation rejects bad files") {
  auto good = bc::showToJson(sampleShow(), bc::SecretMode::Plain);
  auto bad = good;
  bad["schemaVersion"] = 2;
  CHECK_THROWS_AS(bc::showFromJson(bad), bc::ShowError);
  bad = good;
  bad["settings"]["rate"] = "30/1";
  CHECK_THROWS_AS(bc::showFromJson(bad), bc::ShowError);
  bad = good;
  bad["settings"]["holdMs"] = 70000;
  CHECK_THROWS_AS(bc::showFromJson(bad), bc::ShowError);
  bad = good;
  bad["cameras"].push_back(bad["cameras"][0]);  // duplicate id
  CHECK_THROWS_AS(bc::showFromJson(bad), bc::ShowError);
  bad = good;
  bad["cameras"][0]["input"]["params"]["passphrase"] = {{"set", true}};
  CHECK_THROWS_AS(bc::showFromJson(bad), bc::ShowError);
  CHECK_THROWS_AS(bc::showFromJson(nlohmann::json::array()), bc::ShowError);
}

TEST_CASE("Show: newId gives 8 lowercase hex chars and differs") {
  auto a = bc::newId(), b = bc::newId();
  CHECK(a.size() == 8);
  CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
  CHECK(a != b);
}
```

`tests/show_store_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <fstream>
#include <thread>

#include "core/show_store.h"
#include "support/temp_dir.h"

using namespace std::chrono_literals;

namespace {
bc::Show showNamed(const std::string& name) {
  bc::Show s;
  bc::CameraConfig cam;
  cam.id = "cam1";
  cam.name = name;
  cam.input.params = {{"mode", "listener"}, {"port", 9000}, {"passphrase", "supersecret123"}};
  s.cameras.push_back(cam);
  return s;
}
std::string readAll(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
}  // namespace

TEST_CASE("ShowStore: save then load returns the same show, secrets encrypted on disk") {
  bc::test::TempDir dir;
  bc::ShowStore store(dir.path);
  store.saveNow(showNamed("Cam A"));
  CHECK(readAll(store.mainPath()).find("supersecret123") == std::string::npos);
  auto loaded = store.load();
  CHECK(loaded.from == bc::ShowStore::From::Main);
  CHECK(loaded.warning.empty());
  CHECK(loaded.show == showNamed("Cam A"));
}

TEST_CASE("ShowStore: a corrupt show.json falls back to last-known-good with a warning") {
  bc::test::TempDir dir;
  bc::ShowStore store(dir.path);
  store.saveNow(showNamed("Cam A"));
  std::ofstream(store.mainPath(), std::ios::trunc) << "{not json";
  auto loaded = store.load();
  CHECK(loaded.from == bc::ShowStore::From::LastKnownGood);
  CHECK_FALSE(loaded.warning.empty());
  CHECK(loaded.show == showNamed("Cam A"));
}

TEST_CASE("ShowStore: nothing on disk gives the default show without a warning") {
  bc::test::TempDir dir;
  bc::ShowStore store(dir.path);
  auto loaded = store.load();
  CHECK(loaded.from == bc::ShowStore::From::Default);
  CHECK(loaded.warning.empty());
  CHECK(loaded.show.cameras.empty());
  CHECK(loaded.show.settings.rate == bc::Rate{60, 1});
}

TEST_CASE("ShowStore: rapid changes are coalesced into one save") {
  bc::test::TempDir dir;
  bc::ShowStore store(dir.path, 50ms);
  store.scheduleSave(showNamed("one"));
  store.scheduleSave(showNamed("two"));
  store.scheduleSave(showNamed("three"));
  std::this_thread::sleep_for(400ms);
  CHECK(store.saveCount() == 1);
  CHECK(store.load().show.cameras[0].name == "three");
}

TEST_CASE("ShowStore: destruction flushes a pending save") {
  bc::test::TempDir dir;
  {
    bc::ShowStore store(dir.path, 10s);
    store.scheduleSave(showNamed("pending"));
  }
  bc::ShowStore again(dir.path);
  CHECK(again.load().show.cameras[0].name == "pending");
}
```

Add `tests/show_test.cpp tests/show_store_test.cpp` to `bc_tests`.

- [ ] **Step 2: Run them to verify they fail**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Show*"`
Expected: build FAILS — `core/show.h` not found.

- [ ] **Step 3: Implement DPAPI secrets**

`src/core/secrets.h`:
```cpp
#pragma once
#include <string>
#include <string_view>

namespace bc {
// DPAPI (current user) + base64. Spec §6.2: secrets never sit on disk as plain text.
std::string protectSecret(std::string_view plain);
// Throws std::runtime_error when the input is not base64 or not decryptable by this user.
std::string unprotectSecret(std::string_view base64);
}  // namespace bc
```

`src/core/secrets.cpp`:
```cpp
#include "core/secrets.h"

#include <windows.h>
#include <wincrypt.h>

#include <stdexcept>
#include <vector>

namespace bc {
std::string protectSecret(std::string_view plain) {
  DATA_BLOB in{static_cast<DWORD>(plain.size()),
               reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
  DATA_BLOB out{};
  if (!CryptProtectData(&in, L"BreadCamera", nullptr, nullptr, nullptr,
                        CRYPT_PROTECT_UI_FORBIDDEN, &out)) {
    throw std::runtime_error("CryptProtectData failed: " + std::to_string(GetLastError()));
  }
  DWORD len = 0;
  CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       nullptr, &len);
  std::string b64(len, '\0');
  CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                       b64.data(), &len);
  b64.resize(len);
  LocalFree(out.pbData);
  return b64;
}

std::string unprotectSecret(std::string_view base64) {
  DWORD len = 0;
  if (!CryptStringToBinaryA(base64.data(), static_cast<DWORD>(base64.size()),
                            CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr)) {
    throw std::runtime_error("secret is not valid base64");
  }
  std::vector<BYTE> bin(len);
  CryptStringToBinaryA(base64.data(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64,
                       bin.data(), &len, nullptr, nullptr);
  DATA_BLOB in{len, bin.data()};
  DATA_BLOB out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPT_PROTECT_UI_FORBIDDEN,
                          &out)) {
    throw std::runtime_error("CryptUnprotectData failed: " + std::to_string(GetLastError()));
  }
  std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
  LocalFree(out.pbData);
  return plain;
}
}  // namespace bc
```

- [ ] **Step 4: Implement the show model and JSON**

`src/core/show.h`:
```cpp
#pragma once
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/rate.h"

namespace bc {
struct InputConfig {
  std::string kind = "srt";
  nlohmann::json params = nlohmann::json::object();
  bool operator==(const InputConfig&) const = default;
};

struct OutputConfig {
  std::string id;
  std::string kind;  // "ndi" | "fullscreen" in M1
  bool keyed = false;
  bool enabled = true;
  nlohmann::json params = nlohmann::json::object();
  bool operator==(const OutputConfig&) const = default;
};

struct CameraConfig {
  std::string id;
  std::string name;
  InputConfig input;
  std::vector<OutputConfig> outputs;
  bool operator==(const CameraConfig&) const = default;
};

struct ShowSettings {
  Rate rate{60, 1};
  int holdMs = 5000;
  std::string slatePath;  // empty = built-in slate
  bool operator==(const ShowSettings&) const = default;
};

struct Show {
  static constexpr int kSchemaVersion = 1;
  ShowSettings settings;
  std::vector<CameraConfig> cameras;
  CameraConfig* findCamera(std::string_view id);
  const CameraConfig* findCamera(std::string_view id) const;
  bool operator==(const Show&) const = default;
};

class ShowError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

inline constexpr std::array<std::string_view, 1> kSecretKeys{"passphrase"};

enum class SecretMode { Encrypt, Redact, Plain };

nlohmann::json showToJson(const Show& show, SecretMode mode);
Show showFromJson(const nlohmann::json& j);  // throws ShowError
nlohmann::json cameraToJson(const CameraConfig& cam, SecretMode mode);
CameraConfig cameraFromJson(const nlohmann::json& j);  // throws ShowError
nlohmann::json outputToJson(const OutputConfig& out);
OutputConfig outputFromJson(const nlohmann::json& j);  // throws ShowError
std::string newId();
}  // namespace bc
```

`src/core/show.cpp`:
```cpp
#include "core/show.h"

#include <algorithm>
#include <random>
#include <set>

#include "core/secrets.h"

namespace bc {
namespace {
using nlohmann::json;

bool isSecretKey(const std::string& key) {
  return std::find(kSecretKeys.begin(), kSecretKeys.end(), key) != kSecretKeys.end();
}

json secretsOut(const json& params, SecretMode mode) {
  json out = params;
  for (auto& [key, value] : out.items()) {
    if (!isSecretKey(key) || !value.is_string()) continue;
    const auto plain = value.get<std::string>();
    switch (mode) {
      case SecretMode::Encrypt:
        if (!plain.empty()) value = json{{"$dpapi", protectSecret(plain)}};
        break;
      case SecretMode::Redact:
        value = json{{"set", !plain.empty()}};
        break;
      case SecretMode::Plain:
        break;
    }
  }
  return out;
}

json secretsIn(const json& params) {
  if (!params.is_object()) throw ShowError("params must be an object");
  json out = params;
  for (auto& [key, value] : out.items()) {
    if (!isSecretKey(key)) continue;
    if (value.is_string()) continue;
    if (value.is_object() && value.size() == 1 && value.contains("$dpapi") &&
        value["$dpapi"].is_string()) {
      try {
        value = unprotectSecret(value["$dpapi"].get<std::string>());
      } catch (const std::exception& e) {
        throw ShowError(std::string("cannot decrypt ") + key + ": " + e.what());
      }
      continue;
    }
    throw ShowError("secret '" + key + "' must be a string or {\"$dpapi\": ...}");
  }
  return out;
}

std::string str(const json& j, const char* key) {
  if (!j.contains(key) || !j[key].is_string()) throw ShowError(std::string("missing string '") + key + "'");
  return j[key].get<std::string>();
}
}  // namespace

CameraConfig* Show::findCamera(std::string_view id) {
  for (auto& c : cameras) if (c.id == id) return &c;
  return nullptr;
}
const CameraConfig* Show::findCamera(std::string_view id) const {
  for (auto& c : cameras) if (c.id == id) return &c;
  return nullptr;
}

json outputToJson(const OutputConfig& o) {
  return {{"id", o.id}, {"kind", o.kind}, {"keyed", o.keyed}, {"enabled", o.enabled}, {"params", o.params}};
}

OutputConfig outputFromJson(const json& j) {
  if (!j.is_object()) throw ShowError("output must be an object");
  OutputConfig o;
  o.id = str(j, "id");
  o.kind = str(j, "kind");
  if (o.id.empty() || o.kind.empty()) throw ShowError("output id and kind must not be empty");
  o.keyed = j.value("keyed", false);
  o.enabled = j.value("enabled", true);
  o.params = j.value("params", json::object());
  if (!o.params.is_object()) throw ShowError("output params must be an object");
  return o;
}

json cameraToJson(const CameraConfig& c, SecretMode mode) {
  json outs = json::array();
  for (auto& o : c.outputs) outs.push_back(outputToJson(o));
  return {{"id", c.id},
          {"name", c.name},
          {"input", {{"kind", c.input.kind}, {"params", secretsOut(c.input.params, mode)}}},
          {"outputs", outs}};
}

CameraConfig cameraFromJson(const json& j) {
  if (!j.is_object()) throw ShowError("camera must be an object");
  CameraConfig c;
  c.id = str(j, "id");
  c.name = str(j, "name");
  if (c.id.empty()) throw ShowError("camera id must not be empty");
  if (!j.contains("input") || !j["input"].is_object()) throw ShowError("camera input missing");
  c.input.kind = str(j["input"], "kind");
  c.input.params = secretsIn(j["input"].value("params", json::object()));
  std::set<std::string> ids;
  for (auto& o : j.value("outputs", json::array())) {
    auto out = outputFromJson(o);
    if (!ids.insert(out.id).second) throw ShowError("duplicate output id " + out.id);
    c.outputs.push_back(std::move(out));
  }
  return c;
}

json showToJson(const Show& s, SecretMode mode) {
  json cams = json::array();
  for (auto& c : s.cameras) cams.push_back(cameraToJson(c, mode));
  return {{"schemaVersion", Show::kSchemaVersion},
          {"settings",
           {{"rate", s.settings.rate.str()},
            {"holdMs", s.settings.holdMs},
            {"slatePath", s.settings.slatePath}}},
          {"cameras", cams}};
}

Show showFromJson(const json& j) {
  if (!j.is_object()) throw ShowError("show must be a JSON object");
  if (j.value("schemaVersion", 0) != Show::kSchemaVersion) {
    throw ShowError("unsupported schemaVersion " + j.value("schemaVersion", json(0)).dump());
  }
  Show s;
  auto settings = j.value("settings", json::object());
  auto rate = parseShowRate(settings.value("rate", std::string("60/1")));
  if (!rate) throw ShowError("settings.rate must be 50/1, 60000/1001 or 60/1");
  s.settings.rate = *rate;
  s.settings.holdMs = settings.value("holdMs", 5000);
  if (s.settings.holdMs < 0 || s.settings.holdMs > 60000) throw ShowError("settings.holdMs must be 0..60000");
  s.settings.slatePath = settings.value("slatePath", std::string());
  std::set<std::string> ids;
  for (auto& cj : j.value("cameras", json::array())) {
    auto cam = cameraFromJson(cj);
    if (!ids.insert(cam.id).second) throw ShowError("duplicate camera id " + cam.id);
    s.cameras.push_back(std::move(cam));
  }
  return s;
}

std::string newId() {
  static thread_local std::mt19937 rng{std::random_device{}()};
  static constexpr char kHex[] = "0123456789abcdef";
  std::string id(8, '0');
  for (auto& ch : id) ch = kHex[rng() % 16];
  return id;
}
}  // namespace bc
```

Note: `j.value(...)` throws `nlohmann::json::type_error` on a type mismatch; wrap the body of `showFromJson` and `cameraFromJson` so any `nlohmann::json::exception` is rethrown as `ShowError(e.what())`. Do that with a `try { ... } catch (const nlohmann::json::exception& e) { throw ShowError(e.what()); }` around each function body.

- [ ] **Step 5: Implement `ShowStore`**

`src/core/show_store.h`:
```cpp
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/show.h"

namespace bc {
// show.json + show.lkg.json in one folder (spec §6.2). Writes are atomic; the
// last-known-good copy is only replaced by a file that parsed back cleanly.
class ShowStore {
 public:
  enum class From { Main, LastKnownGood, Default };
  struct Loaded {
    Show show;
    From from = From::Default;
    std::string warning;  // non-empty when the engine must show a warning
  };

  explicit ShowStore(std::filesystem::path dir,
                     std::chrono::milliseconds debounce = std::chrono::milliseconds(500));
  ~ShowStore();
  ShowStore(const ShowStore&) = delete;
  ShowStore& operator=(const ShowStore&) = delete;

  Loaded load();
  void saveNow(const Show& show);  // throws ShowError
  void scheduleSave(Show show);    // debounced, on the store's thread
  void flush();                    // writes a pending save now
  uint64_t saveCount() const;
  std::string lastSaveError() const;
  std::filesystem::path mainPath() const { return dir_ / "show.json"; }
  std::filesystem::path lkgPath() const { return dir_ / "show.lkg.json"; }

 private:
  void run();
  std::filesystem::path dir_;
  std::chrono::milliseconds debounce_;
  mutable std::mutex m_;
  std::mutex fileMutex_;
  std::condition_variable cv_;
  std::optional<Show> pending_;
  std::chrono::steady_clock::time_point lastChange_{};
  bool stop_ = false;
  uint64_t saveCount_ = 0;
  std::string lastError_;
  std::thread thread_;
};
}  // namespace bc
```

`src/core/show_store.cpp`:
```cpp
#include "core/show_store.h"

#include <windows.h>

#include <fstream>

namespace bc {
namespace {
nlohmann::json readJson(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw ShowError("cannot open " + p.string());
  try {
    return nlohmann::json::parse(f);
  } catch (const nlohmann::json::exception& e) {
    throw ShowError(p.filename().string() + " is not valid JSON: " + e.what());
  }
}
}  // namespace

ShowStore::ShowStore(std::filesystem::path dir, std::chrono::milliseconds debounce)
    : dir_(std::move(dir)), debounce_(debounce), thread_([this] { run(); }) {}

ShowStore::~ShowStore() {
  flush();
  {
    std::lock_guard l(m_);
    stop_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

ShowStore::Loaded ShowStore::load() {
  std::string mainProblem;
  if (std::filesystem::exists(mainPath())) {
    try {
      return {showFromJson(readJson(mainPath())), From::Main, ""};
    } catch (const std::exception& e) {
      mainProblem = e.what();
    }
  }
  if (std::filesystem::exists(lkgPath())) {
    try {
      std::string why = mainProblem.empty() ? "show.json is missing" : "show.json unreadable (" + mainProblem + ")";
      return {showFromJson(readJson(lkgPath())), From::LastKnownGood, why + "; loaded the last-known-good copy"};
    } catch (const std::exception& e) {
      return {Show{}, From::Default, std::string("show files unreadable (") + e.what() + "); started with an empty show"};
    }
  }
  if (!mainProblem.empty()) {
    return {Show{}, From::Default, "show.json unreadable (" + mainProblem + "); started with an empty show"};
  }
  return {Show{}, From::Default, ""};
}

void ShowStore::saveNow(const Show& show) {
  std::lock_guard fl(fileMutex_);
  std::filesystem::create_directories(dir_);
  const auto text = showToJson(show, SecretMode::Encrypt).dump(2);
  const auto tmp = dir_ / "show.json.tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    f << text;
    f.flush();
    if (!f) throw ShowError("cannot write " + tmp.string());
  }
  if (!MoveFileExW(tmp.c_str(), mainPath().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    throw ShowError("cannot replace show.json (error " + std::to_string(GetLastError()) + ")");
  }
  showFromJson(readJson(mainPath()));  // parse back before promoting to last-known-good
  CopyFileW(mainPath().c_str(), lkgPath().c_str(), FALSE);
  std::lock_guard l(m_);
  ++saveCount_;
}

void ShowStore::scheduleSave(Show show) {
  {
    std::lock_guard l(m_);
    pending_ = std::move(show);
    lastChange_ = std::chrono::steady_clock::now();
  }
  cv_.notify_all();
}

void ShowStore::flush() {
  std::optional<Show> s;
  {
    std::lock_guard l(m_);
    s = std::move(pending_);
    pending_.reset();
  }
  if (!s) return;
  try {
    saveNow(*s);
  } catch (const std::exception& e) {
    std::lock_guard l(m_);
    lastError_ = e.what();
  }
}

void ShowStore::run() {
  std::unique_lock l(m_);
  while (!stop_) {
    if (!pending_) {
      cv_.wait(l);
      continue;
    }
    const auto due = lastChange_ + debounce_;
    if (cv_.wait_until(l, due, [&] { return stop_ || !pending_ || lastChange_ + debounce_ > due; })) {
      continue;  // stopped, flushed elsewhere, or rescheduled
    }
    Show s = std::move(*pending_);
    pending_.reset();
    l.unlock();
    try {
      saveNow(s);
    } catch (const std::exception& e) {
      std::lock_guard el(m_);
      lastError_ = e.what();
    }
    l.lock();
  }
}

uint64_t ShowStore::saveCount() const {
  std::lock_guard l(m_);
  return saveCount_;
}

std::string ShowStore::lastSaveError() const {
  std::lock_guard l(m_);
  return lastError_;
}
}  // namespace bc
```

Careful: `saveNow` takes `m_` at the end, and `run()` calls `saveNow` with `m_` unlocked — keep it that way (no deadlock). `lastError_` is cleared on a successful save: add `lastError_.clear();` next to `++saveCount_`.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Show*"` then `-Filter "Secrets*"`
Expected: all Show/ShowStore/Secrets cases pass.

- [ ] **Step 7: Commit**

```bash
git add src/core tests CMakeLists.txt
git commit -m "feat(core): show file model, DPAPI secrets and crash-safe show store"
```

### Task 4: Live state — signal states, errors and the snapshot/patch state store

**Files:**
- Create: `src/core/signal_state.h`, `src/core/signal_state.cpp`, `src/core/state_store.h`, `src/core/state_store.cpp`
- Modify: `CMakeLists.txt` (`# Task 4` sources, test file)
- Test: `tests/state_store_test.cpp`

**Interfaces:**
- Produces:
  - `enum class bc::SignalState { Ready, Connecting, Live, Offline }`, `std::string_view bc::toString(SignalState)` → `"ready" | "connecting" | "live" | "offline"`.
  - `struct bc::ErrorInfo { std::string code; std::string message; bool operator== }`, `nlohmann::json bc::errorToJson(const std::optional<ErrorInfo>&)` → `null` or `{"code","message"}`.
  - `bc::StateStore`: `void set(std::string_view pointer, nlohmann::json value)`, `void erase(std::string_view pointer)`, `nlohmann::json snapshot() const`, `nlohmann::json takePatch()`.

Patch format (spec §6.1): an array of `{"op":"replace","path":"/cameras/abc/input/state","value":"live"}` and `{"op":"remove","path":"/cameras/abc"}`. **`replace` creates missing parents** (this is our protocol, not strict RFC 6902; the UI's `applyPatch` in Task 13 follows the same rule). Ops are in order; a later op on the same path wins.

State tree paths used by later tasks:
```
/engine/gpu            "AMD Radeon RX 6600"
/engine/rate           "60/1"
/engine/warnings/<source>  "" or a message; sources: slate (Task 9), show (Task 11)
/engine/overruns       number
/cameras/<id>/name
/cameras/<id>/phase    "nosignal" | "live" | "holding" | "slate"
/cameras/<id>/input/state   signal state string
/cameras/<id>/input/error   null | {code,message}
/cameras/<id>/outputs/<oid>/kind
/cameras/<id>/outputs/<oid>/state
/cameras/<id>/outputs/<oid>/error
```

- [ ] **Step 1: Write the failing test**

`tests/state_store_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "core/signal_state.h"
#include "core/state_store.h"

using nlohmann::json;

TEST_CASE("SignalState: wire names match the UX contract order") {
  CHECK(bc::toString(bc::SignalState::Ready) == "ready");
  CHECK(bc::toString(bc::SignalState::Connecting) == "connecting");
  CHECK(bc::toString(bc::SignalState::Live) == "live");
  CHECK(bc::toString(bc::SignalState::Offline) == "offline");
  CHECK(bc::errorToJson(std::nullopt).is_null());
  CHECK(bc::errorToJson(bc::ErrorInfo{"x", "y"}) == json{{"code", "x"}, {"message", "y"}});
}

TEST_CASE("StateStore: set creates parents and records one replace op") {
  bc::StateStore s;
  s.set("/cameras/abc/input/state", "live");
  CHECK(s.snapshot()["cameras"]["abc"]["input"]["state"] == "live");
  auto p = s.takePatch();
  REQUIRE(p.size() == 1);
  CHECK(p[0] == json{{"op", "replace"}, {"path", "/cameras/abc/input/state"}, {"value", "live"}});
  CHECK(s.takePatch().empty());
}

TEST_CASE("StateStore: setting the same value records nothing") {
  bc::StateStore s;
  s.set("/engine/rate", "60/1");
  s.takePatch();
  s.set("/engine/rate", "60/1");
  CHECK(s.takePatch().empty());
}

TEST_CASE("StateStore: erase removes the subtree and records a remove op") {
  bc::StateStore s;
  s.set("/cameras/abc/name", "Cam");
  s.takePatch();
  s.erase("/cameras/abc");
  CHECK_FALSE(s.snapshot()["cameras"].contains("abc"));
  auto p = s.takePatch();
  REQUIRE(p.size() == 1);
  CHECK(p[0] == json{{"op", "remove"}, {"path", "/cameras/abc"}});
  s.erase("/cameras/missing");
  CHECK(s.takePatch().empty());
}

TEST_CASE("StateStore: snapshot starts as an empty object") {
  bc::StateStore s;
  CHECK(s.snapshot() == json::object());
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "*State*"`
Expected: build FAILS — headers not found.

- [ ] **Step 3: Implement**

`src/core/signal_state.h`:
```cpp
#pragma once
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace bc {
// UX-contract signal states, in their fixed order. Errors are separate (spec §6.1).
enum class SignalState { Ready, Connecting, Live, Offline };

std::string_view toString(SignalState s);

struct ErrorInfo {
  std::string code;
  std::string message;
  bool operator==(const ErrorInfo&) const = default;
};

nlohmann::json errorToJson(const std::optional<ErrorInfo>& e);
}  // namespace bc
```

`src/core/signal_state.cpp`:
```cpp
#include "core/signal_state.h"

namespace bc {
std::string_view toString(SignalState s) {
  switch (s) {
    case SignalState::Ready: return "ready";
    case SignalState::Connecting: return "connecting";
    case SignalState::Live: return "live";
    case SignalState::Offline: return "offline";
  }
  return "offline";
}

nlohmann::json errorToJson(const std::optional<ErrorInfo>& e) {
  if (!e) return nullptr;
  return {{"code", e->code}, {"message", e->message}};
}
}  // namespace bc
```

`src/core/state_store.h`:
```cpp
#pragma once
#include <mutex>
#include <string_view>

#include <nlohmann/json.hpp>

namespace bc {
// Engine-side live state pushed to the UI: one snapshot on connect, then patches
// (spec §6.1). Thread-safe; inputs and outputs report from their own threads.
class StateStore {
 public:
  void set(std::string_view pointer, nlohmann::json value);
  void erase(std::string_view pointer);
  nlohmann::json snapshot() const;
  nlohmann::json takePatch();

 private:
  mutable std::mutex m_;
  nlohmann::json state_ = nlohmann::json::object();
  nlohmann::json ops_ = nlohmann::json::array();
};
}  // namespace bc
```

`src/core/state_store.cpp`:
```cpp
#include "core/state_store.h"

namespace bc {
void StateStore::set(std::string_view pointer, nlohmann::json value) {
  nlohmann::json::json_pointer ptr{std::string(pointer)};
  std::lock_guard l(m_);
  if (state_.contains(ptr) && state_.at(ptr) == value) return;
  state_[ptr] = value;
  ops_.push_back({{"op", "replace"}, {"path", std::string(pointer)}, {"value", std::move(value)}});
}

void StateStore::erase(std::string_view pointer) {
  nlohmann::json::json_pointer ptr{std::string(pointer)};
  std::lock_guard l(m_);
  if (!state_.contains(ptr)) return;
  auto parent = ptr.parent_pointer();
  state_.at(parent).erase(ptr.back());
  ops_.push_back({{"op", "remove"}, {"path", std::string(pointer)}});
}

nlohmann::json StateStore::snapshot() const {
  std::lock_guard l(m_);
  return state_;
}

nlohmann::json StateStore::takePatch() {
  std::lock_guard l(m_);
  nlohmann::json out = std::move(ops_);
  ops_ = nlohmann::json::array();
  return out;
}
}  // namespace bc
```

Camera and output ids are `newId()` hex, so they never contain `/` or `~` and need no JSON-Pointer escaping.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "*State*"`
Expected: 5 test cases pass.

- [ ] **Step 5: Commit**

```bash
git add src/core tests CMakeLists.txt
git commit -m "feat(core): signal states and snapshot/patch state store"
```

### Task 5: GPU device, texture pool and colour conversion shaders

**Files:**
- Create: `src/core/utf8.h`
- Create: `src/gpu/device.h`, `src/gpu/device.cpp`, `src/gpu/texture_pool.h`, `src/gpu/texture_pool.cpp`, `src/gpu/converter.h`, `src/gpu/converter.cpp`, `src/gpu/image_io.h`, `src/gpu/image_io.cpp`
- Create: `src/gpu/shaders/yuv_to_linear.hlsl`, `src/gpu/shaders/bgra_to_linear.hlsl`, `src/gpu/shaders/linear_to_bgra8.hlsl`
- Modify: `CMakeLists.txt` (shader compilation + `bc_gpu` library; link `bc_gpu` into `bc_tests`), `tests/support/requires.cpp` (real `HardwareGpu` check)
- Test: `tests/gpu_test.cpp`

**Interfaces:**
- Produces:
  - `std::string bc::toUtf8(std::wstring_view)`, `std::wstring bc::toWide(std::string_view)`.
  - `bc::GpuInfo { std::string adapterName; uint32_t vendorId; bool warp; }`; `bc::GpuDevice`: `static std::unique_ptr<GpuDevice> create(Pref)` with `Pref { Hardware, Warp, HardwareOrWarp }` (env `BC_FORCE_WARP=1` forces WARP), `ID3D11Device* device()`, `ID3D11DeviceContext* context()`, `const GpuInfo& info()`, `bool isNvidia()`, `bool supportsVideoDecode()`, `std::unique_lock<std::recursive_mutex> lock()` (**every multi-call sequence on the immediate context must hold it**), `std::recursive_mutex& contextMutex()`.
  - `bc::GpuTexture { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv, srvLuma, srvChroma; ComPtr<ID3D11UnorderedAccessView> uav; ComPtr<ID3D11RenderTargetView> rtv; DXGI_FORMAT format; uint32_t width, height; }`, `using bc::TextureRef = std::shared_ptr<GpuTexture>`; `bc::TexturePool(GpuDevice&)`: `TextureRef acquire(w, h, DXGI_FORMAT, UINT bind, D3D11_USAGE = DEFAULT, UINT cpuAccess = 0)`, `size_t freeCount() const`; free function `TextureRef bc::wrapTexture(ID3D11Device*, Microsoft::WRL::ComPtr<ID3D11Texture2D>)`.
  - `bc::PixelLayout { NV12, P010, BGRA8 }`, `bc::CpuImage { uint32_t width, height; std::vector<uint8_t> bgra; }`, `bc::YuvMatrix { std::array<std::array<float,4>,3> rows; }`, `YuvMatrix bc::yuvToRgbMatrix(uint32_t height, bool fullRange)`.
  - `bc::Converter(GpuDevice&, TexturePool&)`: `TextureRef toLinear(const GpuTexture&, PixelLayout, bool fullRange)`, `TextureRef toBgra8(const GpuTexture& linear, uint32_t outW, uint32_t outH)`, `CpuImage readback(const GpuTexture& rgba8)`, `TextureRef solid(uint32_t w, uint32_t h, std::array<float,4> linearRgba)`, `TextureRef uploadBgra8(const uint8_t*, uint32_t w, uint32_t h, uint32_t stride)`, `static constexpr DXGI_FORMAT kLinearFormat = DXGI_FORMAT_R16G16B16A16_FLOAT`.
  - `bc::CpuImage bc::loadImageBgra(const std::filesystem::path&)` (WIC; throws `std::runtime_error`).

Colour model (spec §3.2): the pipeline works in **linear light, RGBA16F**. Decode: YUV → R'G'B' with BT.709 (height ≥ 720) or BT.601 matrix, limited range unless the source says full; then R'G'B' → linear with gamma 2.4. Encode for outputs: linear → gamma 1/2.4 → 8-bit. `toBgra8` writes into an **R8G8B8A8_UNORM** texture with the channels swizzled, so the bytes in memory are B,G,R,A (typed UAV stores to B8G8R8A8 are not allowed in D3D11).

- [ ] **Step 1: Write the failing tests**

`tests/gpu_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <wrl/client.h>

#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/image_io.h"
#include "gpu/texture_pool.h"

using Microsoft::WRL::ComPtr;

namespace {
std::array<float, 3> apply(const bc::YuvMatrix& m, float y, float cb, float cr) {
  std::array<float, 3> out{};
  for (int r = 0; r < 3; ++r) {
    out[r] = m.rows[r][0] * y + m.rows[r][1] * cb + m.rows[r][2] * cr + m.rows[r][3];
  }
  return out;
}

struct Gpu {
  std::unique_ptr<bc::GpuDevice> dev = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*dev};
  bc::Converter conv{*dev, pool};
};

bc::TextureRef makeNv12(bc::GpuDevice& dev, uint32_t w, uint32_t h, uint8_t y, uint8_t cb, uint8_t cr) {
  std::vector<uint8_t> data(w * h * 3 / 2);
  std::fill(data.begin(), data.begin() + w * h, y);
  for (size_t i = w * h; i < data.size(); i += 2) {
    data[i] = cb;
    data[i + 1] = cr;
  }
  D3D11_TEXTURE2D_DESC d{w, h, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT,
                         D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_SUBRESOURCE_DATA init{data.data(), w, 0};
  ComPtr<ID3D11Texture2D> tex;
  REQUIRE(SUCCEEDED(dev.device()->CreateTexture2D(&d, &init, &tex)));
  return bc::wrapTexture(dev.device(), tex);
}

void checkAll(const bc::CpuImage& img, int b, int g, int r, int tol) {
  REQUIRE(img.bgra.size() == img.width * img.height * 4);
  for (size_t i = 0; i < img.bgra.size(); i += 4) {
    CHECK(std::abs(img.bgra[i] - b) <= tol);
    CHECK(std::abs(img.bgra[i + 1] - g) <= tol);
    CHECK(std::abs(img.bgra[i + 2] - r) <= tol);
  }
}
}  // namespace

TEST_CASE("YuvMatrix: BT.709 limited range white, black and red") {
  auto m = bc::yuvToRgbMatrix(1080, false);
  auto white = apply(m, 235 / 255.f, 128 / 255.f, 128 / 255.f);
  for (float v : white) CHECK(v == doctest::Approx(1.0f).epsilon(0.01));
  auto black = apply(m, 16 / 255.f, 128 / 255.f, 128 / 255.f);
  for (float v : black) CHECK(v == doctest::Approx(0.0f).epsilon(0.01));
  auto red = apply(m, 63 / 255.f, 102 / 255.f, 240 / 255.f);
  CHECK(red[0] == doctest::Approx(1.0f).epsilon(0.03));
  CHECK(std::abs(red[1]) < 0.03f);
  CHECK(std::abs(red[2]) < 0.03f);
}

TEST_CASE("YuvMatrix: full range maps Y=255 to white") {
  auto m = bc::yuvToRgbMatrix(1080, true);
  auto white = apply(m, 1.0f, 128 / 255.f, 128 / 255.f);
  for (float v : white) CHECK(v == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("GpuDevice: creates a device and names the adapter") {
  auto dev = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  CHECK_FALSE(dev->info().adapterName.empty());
  MESSAGE("adapter: " << dev->info().adapterName << (dev->info().warp ? " (WARP)" : ""));
}

TEST_CASE("TexturePool: a released texture is reused for the same key") {
  Gpu g;
  ID3D11Texture2D* first = nullptr;
  {
    auto t = g.pool.acquire(64, 64, bc::Converter::kLinearFormat,
                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    first = t->tex.Get();
  }
  CHECK(g.pool.freeCount() == 1);
  auto again = g.pool.acquire(64, 64, bc::Converter::kLinearFormat,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  CHECK(again->tex.Get() == first);
  CHECK(g.pool.freeCount() == 0);
  CHECK_THROWS(g.pool.acquire(63, 64, DXGI_FORMAT_NV12, D3D11_BIND_SHADER_RESOURCE));
}

TEST_CASE("Converter: NV12 white and black survive the round trip") {
  Gpu g;
  auto white = makeNv12(*g.dev, 16, 16, 235, 128, 128);
  auto lin = g.conv.toLinear(*white, bc::PixelLayout::NV12, false);
  checkAll(g.conv.readback(*g.conv.toBgra8(*lin, 16, 16)), 255, 255, 255, 2);
  auto black = makeNv12(*g.dev, 16, 16, 16, 128, 128);
  checkAll(g.conv.readback(*g.conv.toBgra8(*g.conv.toLinear(*black, bc::PixelLayout::NV12, false), 16, 16)), 0, 0, 0, 2);
}

TEST_CASE("Converter: NV12 BT.709 red lands in the red channel") {
  Gpu g;
  // Height must be >= 720: yuvToRgbMatrix picks BT.709 for height >= 720 (BT.601
  // below), and these codes are BT.709 — a shorter fixture decodes through BT.601.
  auto red = makeNv12(*g.dev, 16, 720, 63, 102, 240);
  auto img = g.conv.readback(*g.conv.toBgra8(*g.conv.toLinear(*red, bc::PixelLayout::NV12, false), 16, 720));
  checkAll(img, 0, 0, 255, 4);
}

TEST_CASE("Converter: downscaling blends in linear light") {
  Gpu g;
  // 2x2 BGRA: left column black, right column white.
  const uint8_t px[] = {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255};
  auto src = g.conv.uploadBgra8(px, 2, 2, 8);
  auto lin = g.conv.toLinear(*src, bc::PixelLayout::BGRA8, true);
  auto img = g.conv.readback(*g.conv.toBgra8(*lin, 1, 1));
  checkAll(img, 191, 191, 191, 3);  // 0.5 linear -> 0.5^(1/2.4) = 0.749
}

TEST_CASE("Converter: solid fill and BGRA upload round trips") {
  Gpu g;
  checkAll(g.conv.readback(*g.conv.toBgra8(*g.conv.solid(4, 4, {0.214f, 0.214f, 0.214f, 1}), 4, 4)), 134, 134, 134, 2);
  std::vector<uint8_t> px(4 * 4 * 4);
  for (size_t i = 0; i < px.size(); i += 4) {
    px[i] = 10;
    px[i + 1] = 120;
    px[i + 2] = 230;
    px[i + 3] = 255;
  }
  auto up = g.conv.uploadBgra8(px.data(), 4, 4, 16);
  auto img = g.conv.readback(*g.conv.toBgra8(*g.conv.toLinear(*up, bc::PixelLayout::BGRA8, true), 4, 4));
  checkAll(img, 10, 120, 230, 2);
}

TEST_CASE("loadImageBgra: a missing file throws") {
  CHECK_THROWS(bc::loadImageBgra("C:/definitely/not/here.png"));
}
```

Add `tests/gpu_test.cpp` to `bc_tests`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "*Converter*"`
Expected: build FAILS — `gpu/converter.h` not found.

- [ ] **Step 3: Add the shader build and `bc_gpu` to `CMakeLists.txt`**

Insert after the `bc_core` block:
```cmake
# ---- gpu ----
find_program(FXC fxc REQUIRED)  # Windows SDK; on PATH in the VS developer shell
set(BC_SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${BC_SHADER_DIR})
set(BC_SHADER_HEADERS)
function(bc_shader name profile)
  set(src ${CMAKE_SOURCE_DIR}/src/gpu/shaders/${name}.hlsl)
  set(out ${BC_SHADER_DIR}/${name}.h)
  add_custom_command(OUTPUT ${out}
    COMMAND ${FXC} /nologo /O3 /T ${profile} /E main /Vn g_${name} /Fh ${out} ${src}
    DEPENDS ${src} VERBATIM)
  set(BC_SHADER_HEADERS ${BC_SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()
bc_shader(yuv_to_linear cs_5_0)
bc_shader(bgra_to_linear cs_5_0)
bc_shader(linear_to_bgra8 cs_5_0)
# Task 7 adds fullscreen_vs (vs_5_0) and fullscreen_ps (ps_5_0) here

add_library(bc_gpu STATIC
  src/gpu/device.cpp
  src/gpu/texture_pool.cpp
  src/gpu/converter.cpp
  src/gpu/image_io.cpp
  ${BC_SHADER_HEADERS}
)
target_include_directories(bc_gpu PUBLIC src ${BC_SHADER_DIR})
target_link_libraries(bc_gpu PUBLIC bc_core d3d11 dxgi windowscodecs)
```
and change the test link line to `target_link_libraries(bc_tests PRIVATE bc_core bc_gpu doctest::doctest)`.

- [ ] **Step 4: Write the shaders**

`src/gpu/shaders/yuv_to_linear.hlsl` (NV12 and P010; the plane SRVs are R8/R8G8 or R16/R16G16):
```hlsl
Texture2D<float> gLuma : register(t0);
Texture2D<float2> gChroma : register(t1);
RWTexture2D<float4> gOut : register(u0);
SamplerState gLinear : register(s0);
cbuffer Params : register(b0) {
  float4 gRow0;
  float4 gRow1;
  float4 gRow2;
  uint2 gOutSize;
  uint2 gSrcSize;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float y = gLuma[id.xy];
  float2 uv = (float2(id.xy) + 0.5) / float2(gOutSize);
  float2 c = gChroma.SampleLevel(gLinear, uv, 0);
  float4 yuv1 = float4(y, c.x, c.y, 1.0);
  float3 rgb = float3(dot(gRow0, yuv1), dot(gRow1, yuv1), dot(gRow2, yuv1));
  gOut[id.xy] = float4(pow(saturate(rgb), 2.4), 1.0);
}
```

`src/gpu/shaders/bgra_to_linear.hlsl`:
```hlsl
Texture2D<float4> gSrc : register(t0);
RWTexture2D<float4> gOut : register(u0);
cbuffer Params : register(b0) {
  float4 gRow0;
  float4 gRow1;
  float4 gRow2;
  uint2 gOutSize;
  uint2 gSrcSize;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float4 c = gSrc[id.xy];
  gOut[id.xy] = float4(pow(saturate(c.rgb), 2.4), c.a);
}
```

`src/gpu/shaders/linear_to_bgra8.hlsl`:
```hlsl
Texture2D<float4> gSrc : register(t0);
RWTexture2D<unorm float4> gOut : register(u0);  // R8G8B8A8_UNORM, swizzled to BGRA bytes
SamplerState gLinear : register(s0);
cbuffer Params : register(b0) {
  float4 gRow0;
  float4 gRow1;
  float4 gRow2;
  uint2 gOutSize;
  uint2 gSrcSize;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float2 uv = (float2(id.xy) + 0.5) / float2(gOutSize);
  float4 c = gSrc.SampleLevel(gLinear, uv, 0);
  float3 g = pow(saturate(c.rgb), 1.0 / 2.4);
  gOut[id.xy] = float4(g.b, g.g, g.r, saturate(c.a));
}
```
ponytail: plain bilinear downscale (aliases at 4:1 for previews); add a box pre-filter if previews shimmer.

- [ ] **Step 5: Implement `utf8.h` and `GpuDevice`**

`src/core/utf8.h`:
```cpp
#pragma once
#include <windows.h>

#include <string>
#include <string_view>

namespace bc {
inline std::string toUtf8(std::wstring_view w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}
inline std::wstring toWide(std::string_view s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}
}  // namespace bc
```

`src/gpu/device.h`:
```cpp
#pragma once
#include <d3d11_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace bc {
struct GpuInfo {
  std::string adapterName;
  uint32_t vendorId = 0;
  bool warp = false;
};

// The one D3D11 device every stage shares (spec §3.1).
class GpuDevice {
 public:
  enum class Pref { Hardware, Warp, HardwareOrWarp };
  static std::unique_ptr<GpuDevice> create(Pref pref);  // throws std::runtime_error

  ID3D11Device* device() const { return device_.Get(); }
  ID3D11DeviceContext* context() const { return context_.Get(); }
  const GpuInfo& info() const { return info_; }
  bool isNvidia() const { return info_.vendorId == 0x10DE; }
  bool supportsVideoDecode() const { return videoDecode_; }
  // Hold this around any sequence of immediate-context calls (state + dispatch/draw,
  // copy + map). Single calls are already serialized by multithread protection.
  std::unique_lock<std::recursive_mutex> lock() { return std::unique_lock(mutex_); }
  std::recursive_mutex& contextMutex() { return mutex_; }

 private:
  GpuDevice() = default;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  GpuInfo info_;
  bool videoDecode_ = false;
  std::recursive_mutex mutex_;
};
}  // namespace bc
```

`src/gpu/device.cpp`:
```cpp
#include "gpu/device.h"

#include <dxgi.h>

#include <cstdlib>
#include <stdexcept>

#include "core/utf8.h"

using Microsoft::WRL::ComPtr;

namespace bc {
namespace {
bool forceWarp() {
  char* v = nullptr;
  size_t n = 0;
  _dupenv_s(&v, &n, "BC_FORCE_WARP");
  bool on = v && std::string(v) == "1";
  free(v);
  return on;
}
}  // namespace

std::unique_ptr<GpuDevice> GpuDevice::create(Pref pref) {
  if (forceWarp()) pref = Pref::Warp;
  std::unique_ptr<GpuDevice> dev(new GpuDevice());
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  const UINT base = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  auto tryCreate = [&](D3D_DRIVER_TYPE type, UINT flags) {
    dev->device_.Reset();
    dev->context_.Reset();
    return D3D11CreateDevice(nullptr, type, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                             &dev->device_, nullptr, &dev->context_);
  };
  HRESULT hr = E_FAIL;
  if (pref != Pref::Warp) {
    hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, base | D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
    if (FAILED(hr)) hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, base);
  }
  if (FAILED(hr) && pref != Pref::Hardware) hr = tryCreate(D3D_DRIVER_TYPE_WARP, base);
  if (FAILED(hr)) throw std::runtime_error("D3D11CreateDevice failed: " + std::to_string(hr));

  ComPtr<ID3D10Multithread> mt;
  dev->context_.As(&mt);
  mt->SetMultithreadProtected(TRUE);

  ComPtr<IDXGIDevice> dxgi;
  dev->device_.As(&dxgi);
  ComPtr<IDXGIAdapter> adapter;
  dxgi->GetAdapter(&adapter);
  DXGI_ADAPTER_DESC desc{};
  adapter->GetDesc(&desc);
  dev->info_.adapterName = toUtf8(desc.Description);
  dev->info_.vendorId = desc.VendorId;
  dev->info_.warp = desc.VendorId == 0x1414;  // Microsoft (WARP / Basic Render Driver)

  ComPtr<ID3D11VideoDevice> video;
  dev->videoDecode_ = !dev->info_.warp && SUCCEEDED(dev->device_.As(&video));
  return dev;
}
}  // namespace bc
```

- [ ] **Step 6: Implement `TexturePool`**

`src/gpu/texture_pool.h`:
```cpp
#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "gpu/device.h"

namespace bc {
struct GpuTexture {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;        // non-planar formats
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvLuma;    // NV12 / P010
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvChroma;  // NV12 / P010
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
};
using TextureRef = std::shared_ptr<GpuTexture>;

// Creates the views a texture's bind flags allow (plane SRVs for NV12/P010).
TextureRef wrapTexture(ID3D11Device* device, Microsoft::WRL::ComPtr<ID3D11Texture2D> tex);

// Per-camera frame textures are recycled, not re-created each tick (spec §3.2).
class TexturePool {
 public:
  explicit TexturePool(GpuDevice& gpu);
  // Throws std::invalid_argument for odd NV12/P010 sizes, std::runtime_error on failure.
  TextureRef acquire(uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind,
                     D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpuAccess = 0);
  size_t freeCount() const;

 private:
  struct Key {
    uint32_t w, h;
    DXGI_FORMAT fmt;
    UINT bind;
    D3D11_USAGE usage;
    UINT cpu;
    auto operator<=>(const Key&) const = default;
  };
  struct State {
    mutable std::mutex m;
    std::map<Key, std::vector<std::unique_ptr<GpuTexture>>> free;
  };
  GpuDevice& gpu_;
  std::shared_ptr<State> state_ = std::make_shared<State>();
};
}  // namespace bc
```

`src/gpu/texture_pool.cpp`:
```cpp
#include "gpu/texture_pool.h"

#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace bc {
namespace {
constexpr size_t kMaxFreePerKey = 8;

void makeSrv(ID3D11Device* dev, ID3D11Texture2D* tex, DXGI_FORMAT fmt, ComPtr<ID3D11ShaderResourceView>& out) {
  D3D11_SHADER_RESOURCE_VIEW_DESC d{};
  d.Format = fmt;
  d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  d.Texture2D.MipLevels = 1;
  if (FAILED(dev->CreateShaderResourceView(tex, &d, &out))) throw std::runtime_error("CreateShaderResourceView failed");
}
}  // namespace

TextureRef wrapTexture(ID3D11Device* dev, ComPtr<ID3D11Texture2D> tex) {
  D3D11_TEXTURE2D_DESC d{};
  tex->GetDesc(&d);
  auto t = std::make_shared<GpuTexture>();
  t->tex = tex;
  t->format = d.Format;
  t->width = d.Width;
  t->height = d.Height;
  if (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
    if (d.Format == DXGI_FORMAT_NV12) {
      makeSrv(dev, tex.Get(), DXGI_FORMAT_R8_UNORM, t->srvLuma);
      makeSrv(dev, tex.Get(), DXGI_FORMAT_R8G8_UNORM, t->srvChroma);
    } else if (d.Format == DXGI_FORMAT_P010) {
      makeSrv(dev, tex.Get(), DXGI_FORMAT_R16_UNORM, t->srvLuma);
      makeSrv(dev, tex.Get(), DXGI_FORMAT_R16G16_UNORM, t->srvChroma);
    } else {
      makeSrv(dev, tex.Get(), d.Format, t->srv);
    }
  }
  if (d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
    if (FAILED(dev->CreateUnorderedAccessView(tex.Get(), nullptr, &t->uav))) throw std::runtime_error("CreateUnorderedAccessView failed");
  }
  if (d.BindFlags & D3D11_BIND_RENDER_TARGET) {
    if (FAILED(dev->CreateRenderTargetView(tex.Get(), nullptr, &t->rtv))) throw std::runtime_error("CreateRenderTargetView failed");
  }
  return t;
}

TexturePool::TexturePool(GpuDevice& gpu) : gpu_(gpu) {}

TextureRef TexturePool::acquire(uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, D3D11_USAGE usage, UINT cpuAccess) {
  if ((fmt == DXGI_FORMAT_NV12 || fmt == DXGI_FORMAT_P010) && ((w | h) & 1)) {
    throw std::invalid_argument("NV12/P010 textures need even width and height");
  }
  Key key{w, h, fmt, bind, usage, cpuAccess};
  std::unique_ptr<GpuTexture> tex;
  {
    std::lock_guard l(state_->m);
    auto& list = state_->free[key];
    if (!list.empty()) {
      tex = std::move(list.back());
      list.pop_back();
    }
  }
  if (!tex) {
    D3D11_TEXTURE2D_DESC d{w, h, 1, 1, fmt, {1, 0}, usage, bind, cpuAccess, 0};
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(gpu_.device()->CreateTexture2D(&d, nullptr, &t))) throw std::runtime_error("CreateTexture2D failed");
    tex = std::make_unique<GpuTexture>(*wrapTexture(gpu_.device(), t));
  }
  std::weak_ptr<State> weak = state_;
  return TextureRef(tex.release(), [weak, key](GpuTexture* t) {
    if (auto s = weak.lock()) {
      std::lock_guard l(s->m);
      auto& list = s->free[key];
      if (list.size() < kMaxFreePerKey) {
        list.emplace_back(t);
        return;
      }
    }
    delete t;
  });
}

size_t TexturePool::freeCount() const {
  std::lock_guard l(state_->m);
  size_t n = 0;
  for (auto& [k, v] : state_->free) n += v.size();
  return n;
}
}  // namespace bc
```

- [ ] **Step 7: Implement `Converter` and `loadImageBgra`**

`src/gpu/converter.h`:
```cpp
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc {
enum class PixelLayout { NV12, P010, BGRA8 };

struct CpuImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> bgra;  // tightly packed, 4 bytes per pixel
};

// rgb' = rows * (y, cb, cr, 1) on unorm samples (spec §3.2).
struct YuvMatrix {
  std::array<std::array<float, 4>, 3> rows;
};
// BT.709 for height >= 720, else BT.601; limited range unless fullRange.
YuvMatrix yuvToRgbMatrix(uint32_t height, bool fullRange);

class Converter {
 public:
  static constexpr DXGI_FORMAT kLinearFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
  Converter(GpuDevice& gpu, TexturePool& pool);

  TextureRef toLinear(const GpuTexture& src, PixelLayout layout, bool fullRange);
  TextureRef toBgra8(const GpuTexture& linear, uint32_t outW, uint32_t outH);
  CpuImage readback(const GpuTexture& rgba8);  // blocking Map
  TextureRef solid(uint32_t w, uint32_t h, std::array<float, 4> linearRgba);
  TextureRef uploadBgra8(const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride);

 private:
  struct alignas(16) Params {
    float row0[4];
    float row1[4];
    float row2[4];
    uint32_t outW, outH, srcW, srcH;
  };
  void dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srvs, UINT nSrv,
                ID3D11UnorderedAccessView* uav, const Params& p);

  GpuDevice& gpu_;
  TexturePool& pool_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> yuvCs_, bgraCs_, outCs_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
};
}  // namespace bc
```

`src/gpu/converter.cpp`:
```cpp
#include "gpu/converter.h"

#include <cstring>
#include <stdexcept>

#include "bgra_to_linear.h"
#include "linear_to_bgra8.h"
#include "yuv_to_linear.h"

using Microsoft::WRL::ComPtr;

namespace bc {
YuvMatrix yuvToRgbMatrix(uint32_t height, bool fullRange) {
  const float kr = height >= 720 ? 0.2126f : 0.299f;
  const float kb = height >= 720 ? 0.0722f : 0.114f;
  const float kg = 1.0f - kr - kb;
  // Y' = ys*Y + yo ; Pb = cs*Cb + co ; Pr = cs*Cr + co   (inputs are unorm 0..1)
  const float ys = fullRange ? 1.0f : 255.0f / 219.0f;
  const float yo = fullRange ? 0.0f : -16.0f / 219.0f;
  const float cs = fullRange ? 1.0f : 255.0f / 224.0f;
  const float co = fullRange ? -128.0f / 255.0f : -128.0f / 224.0f;
  const float rv = 2.0f * (1.0f - kr);
  const float gu = -2.0f * kb * (1.0f - kb) / kg;
  const float gv = -2.0f * kr * (1.0f - kr) / kg;
  const float bu = 2.0f * (1.0f - kb);
  YuvMatrix m;
  m.rows[0] = {ys, 0.0f, rv * cs, yo + rv * co};
  m.rows[1] = {ys, gu * cs, gv * cs, yo + (gu + gv) * co};
  m.rows[2] = {ys, bu * cs, 0.0f, yo + bu * co};
  return m;
}

Converter::Converter(GpuDevice& gpu, TexturePool& pool) : gpu_(gpu), pool_(pool) {
  auto* dev = gpu_.device();
  if (FAILED(dev->CreateComputeShader(g_yuv_to_linear, sizeof(g_yuv_to_linear), nullptr, &yuvCs_)) ||
      FAILED(dev->CreateComputeShader(g_bgra_to_linear, sizeof(g_bgra_to_linear), nullptr, &bgraCs_)) ||
      FAILED(dev->CreateComputeShader(g_linear_to_bgra8, sizeof(g_linear_to_bgra8), nullptr, &outCs_))) {
    throw std::runtime_error("CreateComputeShader failed");
  }
  D3D11_BUFFER_DESC bd{sizeof(Params), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_))) throw std::runtime_error("CreateBuffer failed");
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(dev->CreateSamplerState(&sd, &sampler_))) throw std::runtime_error("CreateSamplerState failed");
}

void Converter::dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srvs, UINT nSrv,
                         ID3D11UnorderedAccessView* uav, const Params& p) {
  auto lock = gpu_.lock();
  auto* ctx = gpu_.context();
  D3D11_MAPPED_SUBRESOURCE m{};
  ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
  std::memcpy(m.pData, &p, sizeof(p));
  ctx->Unmap(cb_.Get(), 0);
  ctx->CSSetShader(cs, nullptr, 0);
  ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ctx->CSSetSamplers(0, 1, sampler_.GetAddressOf());
  ctx->CSSetShaderResources(0, nSrv, srvs);
  ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx->Dispatch((p.outW + 7) / 8, (p.outH + 7) / 8, 1);
  ID3D11ShaderResourceView* nullSrv[2] = {};
  ID3D11UnorderedAccessView* nullUav = nullptr;
  ctx->CSSetShaderResources(0, 2, nullSrv);
  ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
}

TextureRef Converter::toLinear(const GpuTexture& src, PixelLayout layout, bool fullRange) {
  auto out = pool_.acquire(src.width, src.height, kLinearFormat,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.outW = p.srcW = src.width;
  p.outH = p.srcH = src.height;
  if (layout == PixelLayout::BGRA8) {
    ID3D11ShaderResourceView* srvs[] = {src.srv.Get()};
    dispatch(bgraCs_.Get(), srvs, 1, out->uav.Get(), p);
  } else {
    auto m = yuvToRgbMatrix(src.height, fullRange);
    std::memcpy(p.row0, m.rows[0].data(), sizeof(p.row0));
    std::memcpy(p.row1, m.rows[1].data(), sizeof(p.row1));
    std::memcpy(p.row2, m.rows[2].data(), sizeof(p.row2));
    ID3D11ShaderResourceView* srvs[] = {src.srvLuma.Get(), src.srvChroma.Get()};
    dispatch(yuvCs_.Get(), srvs, 2, out->uav.Get(), p);
  }
  return out;
}

TextureRef Converter::toBgra8(const GpuTexture& linear, uint32_t outW, uint32_t outH) {
  auto out = pool_.acquire(outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.outW = outW;
  p.outH = outH;
  p.srcW = linear.width;
  p.srcH = linear.height;
  ID3D11ShaderResourceView* srvs[] = {linear.srv.Get()};
  dispatch(outCs_.Get(), srvs, 1, out->uav.Get(), p);
  return out;
}

CpuImage Converter::readback(const GpuTexture& rgba8) {
  auto staging = pool_.acquire(rgba8.width, rgba8.height, rgba8.format, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
  CpuImage img;
  img.width = rgba8.width;
  img.height = rgba8.height;
  img.bgra.resize(static_cast<size_t>(img.width) * img.height * 4);
  auto lock = gpu_.lock();
  auto* ctx = gpu_.context();
  ctx->CopyResource(staging->tex.Get(), rgba8.tex.Get());
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(ctx->Map(staging->tex.Get(), 0, D3D11_MAP_READ, 0, &m))) throw std::runtime_error("Map failed");
  for (uint32_t y = 0; y < img.height; ++y) {
    std::memcpy(img.bgra.data() + static_cast<size_t>(y) * img.width * 4,
                static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, img.width * 4);
  }
  ctx->Unmap(staging->tex.Get(), 0);
  return img;
}

TextureRef Converter::solid(uint32_t w, uint32_t h, std::array<float, 4> c) {
  auto out = pool_.acquire(w, h, kLinearFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  auto lock = gpu_.lock();
  gpu_.context()->ClearUnorderedAccessViewFloat(out->uav.Get(), c.data());
  return out;
}

TextureRef Converter::uploadBgra8(const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride) {
  auto out = pool_.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
  auto lock = gpu_.lock();
  gpu_.context()->UpdateSubresource(out->tex.Get(), 0, nullptr, data, stride, 0);
  return out;
}
}  // namespace bc
```
ponytail: `readback` maps synchronously under the context lock (stalls the GPU queue ~1 ms per 1080p frame); double-buffer the staging textures if the tick budget gets tight.

`src/gpu/image_io.h`:
```cpp
#pragma once
#include <filesystem>

#include "gpu/converter.h"

namespace bc {
// Loads PNG/JPEG/BMP through WIC as BGRA8. Throws std::runtime_error.
CpuImage loadImageBgra(const std::filesystem::path& path);
}  // namespace bc
```

`src/gpu/image_io.cpp`:
```cpp
#include "gpu/image_io.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace bc {
CpuImage loadImageBgra(const std::filesystem::path& path) {
  HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  struct Uninit {
    bool on;
    ~Uninit() { if (on) CoUninitialize(); }
  } uninit{SUCCEEDED(co)};
  ComPtr<IWICImagingFactory> f;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
    throw std::runtime_error("WIC is not available");
  }
  ComPtr<IWICBitmapDecoder> dec;
  if (FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
    throw std::runtime_error("cannot open image " + path.string());
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(dec->GetFrame(0, &frame))) throw std::runtime_error("cannot read image " + path.string());
  ComPtr<IWICBitmapSource> bgra;
  if (FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppBGRA, frame.Get(), &bgra))) {
    throw std::runtime_error("cannot convert image " + path.string());
  }
  CpuImage img;
  bgra->GetSize(&img.width, &img.height);
  img.bgra.resize(static_cast<size_t>(img.width) * img.height * 4);
  if (FAILED(bgra->CopyPixels(nullptr, img.width * 4, static_cast<UINT>(img.bgra.size()), img.bgra.data()))) {
    throw std::runtime_error("cannot decode image " + path.string());
  }
  return img;
}
}  // namespace bc
```

- [ ] **Step 8: Make `BC_REQUIRES(HardwareGpu)` real**

In `tests/support/requires.cpp`, add `#include "gpu/device.h"` and replace the `HardwareGpu` case:
```cpp
    case Req::HardwareGpu: {
      reason = "needs a hardware GPU (WARP or BC_FORCE_WARP=1 in use)";
      try {
        auto dev = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
        return dev->info().warp;
      } catch (...) {
        return true;
      }
    }
```

- [ ] **Step 9: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1`
Expected: every test so far passes. Then once with WARP: in PowerShell `$env:BC_FORCE_WARP='1'; .\build\release\bc_tests.exe -tc="*Converter*,*TexturePool*,*GpuDevice*"` — expected PASS (the log names the adapter "Microsoft Basic Render Driver"). Ledger both adapter names.

- [ ] **Step 10: Commit**

```bash
git add src tests CMakeLists.txt
git commit -m "feat(gpu): D3D11 device, texture pool and linear-light conversion shaders"
```

### Task 6: FFmpeg input — SRT (and any URL), hardware/software decode, audio, reconnect

**Files:**
- Create: `src/io/input.h`, `src/io/srt_params.h`, `src/io/srt_params.cpp`, `src/io/ffmpeg_input.h`, `src/io/ffmpeg_input.cpp`
- Create: `tests/support/test_media.h`, `tests/support/test_media.cpp`
- Modify: `CMakeLists.txt` (`bc_io` library; tests)
- Test: `tests/srt_params_test.cpp`, `tests/ffmpeg_input_test.cpp`

**Interfaces:**
- Consumes: `GpuDevice`, `TexturePool`, `TextureRef`, `PixelLayout` (Task 5); `SignalState`, `ErrorInfo` (Task 4); `Backoff` (Task 2).
- Produces:
  - `src/io/input.h`: `bc::VideoFrameIn { TextureRef tex; PixelLayout layout; bool fullRange; int64_t ptsUs; }`, `bc::AudioBlock { int sampleRate = 48000; int channels = 2; std::vector<float> samples; }` (interleaved), `class bc::InputSink { virtual void onVideo(VideoFrameIn) = 0; virtual void onAudio(AudioBlock) = 0; virtual void onState(SignalState, std::optional<ErrorInfo>) = 0; }`, `class bc::Input { virtual void start(InputSink&) = 0; virtual void stop() = 0; }`.
  - `bc::FfmpegInputOptions { std::string url; std::map<std::string,std::string> avOptions; bool reconnect = true; bool allowHwDecode = true; bool realtime = false; std::chrono::milliseconds readTimeout{2000}; }`, `bc::FfmpegInputOptions bc::srtOptionsFromParams(const nlohmann::json&)` (throws `std::invalid_argument` with a user-readable message).
  - `bc::FfmpegInput(GpuDevice&, TexturePool&, FfmpegInputOptions)` implementing `Input`.
  - Test helpers: `void bc::test::writeTestTs(path, int width, int height, int fps, int seconds, bool withAudio = true)`, `class bc::test::TsPusher(path file, std::string url, bool loop)` with `stop()`, `failed()`, `error()`.

`srt` input params (spec §4.2), all optional except where noted:
| key | rule | FFmpeg option |
|---|---|---|
| `url` | `srt://...`, ≤ 2048 chars; when present, `host`/`port` are ignored | used as the URL |
| `mode` | `caller` or `listener`; default `caller` with `url`, else **required** | `mode` |
| `host` | caller: **required**; listener: default `0.0.0.0` | in URL |
| `port` | integer 1..65535, **required** without `url` | in URL |
| `latencyMs` | integer 20..8000, default 120 | `latency` = ms × 1000 (µs) |
| `passphrase` | empty or 10..79 chars (SRT rule) | `passphrase` |
| `streamId` | ≤ 512 chars | `streamid` |

Secrets travel in `avOptions`, never in the URL, so error messages can quote the URL. Error messages quote the URL **without its query string**.

- [ ] **Step 1: Write the failing parameter tests**

`tests/srt_params_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "io/srt_params.h"

using nlohmann::json;

TEST_CASE("srtOptionsFromParams: listener defaults") {
  auto o = bc::srtOptionsFromParams({{"mode", "listener"}, {"port", 9000}});
  CHECK(o.url == "srt://0.0.0.0:9000");
  CHECK(o.avOptions.at("mode") == "listener");
  CHECK(o.avOptions.at("latency") == "120000");
  CHECK_FALSE(o.avOptions.contains("passphrase"));
}

TEST_CASE("srtOptionsFromParams: caller with secrets in options, not in the URL") {
  auto o = bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "10.0.0.5"}, {"port", 7001},
                                     {"latencyMs", 200}, {"passphrase", "0123456789"}, {"streamId", "feed/abc"}});
  CHECK(o.url == "srt://10.0.0.5:7001");
  CHECK(o.avOptions.at("latency") == "200000");
  CHECK(o.avOptions.at("passphrase") == "0123456789");
  CHECK(o.avOptions.at("streamid") == "feed/abc");
}

TEST_CASE("srtOptionsFromParams: a full URL (BreadCall feed) is used as-is") {
  auto o = bc::srtOptionsFromParams({{"url", "srt://media.breadcall.io.vn:9000?streamid=read:room/feed"}});
  CHECK(o.url == "srt://media.breadcall.io.vn:9000?streamid=read:room/feed");
  CHECK(o.avOptions.at("mode") == "caller");
}

TEST_CASE("srtOptionsFromParams: rejects bad input with readable messages") {
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"port", 9000}}), std::invalid_argument);  // no mode
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"mode", "caller"}, {"port", 9000}}), std::invalid_argument);  // no host
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"mode", "listener"}, {"port", 70000}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"mode", "listener"}, {"port", "9000"}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"mode", "listener"}, {"port", 9000}, {"latencyMs", 5}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"mode", "listener"}, {"port", 9000}, {"passphrase", "short"}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"mode", "rendezvous"}, {"port", 9000}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOptionsFromParams({{"url", "rtmp://x"}}), std::invalid_argument);
  CHECK_THROWS_AS(bc::srtOptionsFromParams(json::array()), std::invalid_argument);
}
```

- [ ] **Step 2: Write the test media helpers**

`tests/support/test_media.h`:
```cpp
#pragma once
#include <atomic>
#include <filesystem>
#include <string>
#include <thread>

namespace bc::test {
// MPEG-TS with a moving pattern (mpeg2video) and a 1 kHz tone (mp2, 48 kHz stereo).
// LGPL-native encoders only.
void writeTestTs(const std::filesystem::path& path, int width, int height, int fps, int seconds,
                 bool withAudio = true);

// Re-muxes a TS file to `url` (e.g. "srt://127.0.0.1:47901?mode=caller") in real time on
// its own thread. Retries the connection until the receiver is up or stop() is called.
class TsPusher {
 public:
  TsPusher(std::filesystem::path file, std::string url, bool loop);
  ~TsPusher();
  void stop();
  bool failed() const { return failed_; }
  std::string error() const { return error_; }

 private:
  void run();
  static int interrupt(void* self);
  std::filesystem::path file_;
  std::string url_;
  bool loop_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> failed_{false};
  std::string error_;
  std::thread thread_;
};
}  // namespace bc::test
```

`tests/support/test_media.cpp`:
```cpp
#include "support/test_media.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace bc::test {
namespace {
void check(int rc, const char* what) {
  if (rc < 0) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(rc, buf, sizeof(buf));
    throw std::runtime_error(std::string(what) + ": " + buf);
  }
}
struct CodecDel { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
struct FrameDel { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct PacketDel { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDel>;
using FramePtr = std::unique_ptr<AVFrame, FrameDel>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDel>;

void drain(AVFormatContext* oc, AVCodecContext* c, AVStream* st) {
  PacketPtr pkt(av_packet_alloc());
  for (;;) {
    int rc = avcodec_receive_packet(c, pkt.get());
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return;
    check(rc, "receive_packet");
    av_packet_rescale_ts(pkt.get(), c->time_base, st->time_base);
    pkt->stream_index = st->index;
    check(av_interleaved_write_frame(oc, pkt.get()), "write_frame");
  }
}
}  // namespace

void writeTestTs(const std::filesystem::path& path, int width, int height, int fps, int seconds, bool withAudio) {
  const std::string file = path.string();
  AVFormatContext* oc = nullptr;
  check(avformat_alloc_output_context2(&oc, nullptr, "mpegts", file.c_str()), "alloc_output");
  std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> ocGuard(oc, &avformat_free_context);

  const AVCodec* vc = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
  CodecPtr v(avcodec_alloc_context3(vc));
  v->width = width;
  v->height = height;
  v->time_base = {1, fps};
  v->framerate = {fps, 1};
  v->pix_fmt = AV_PIX_FMT_YUV420P;
  v->gop_size = fps / 2;
  v->max_b_frames = 0;
  v->bit_rate = 8'000'000;
  check(avcodec_open2(v.get(), vc, nullptr), "open mpeg2video");
  AVStream* vs = avformat_new_stream(oc, nullptr);
  avcodec_parameters_from_context(vs->codecpar, v.get());
  vs->time_base = v->time_base;

  CodecPtr a;
  AVStream* as = nullptr;
  if (withAudio) {
    const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_MP2);
    a.reset(avcodec_alloc_context3(ac));
    a->sample_rate = 48000;
    a->sample_fmt = AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&a->ch_layout, 2);
    a->bit_rate = 192000;
    a->time_base = {1, 48000};
    check(avcodec_open2(a.get(), ac, nullptr), "open mp2");
    as = avformat_new_stream(oc, nullptr);
    avcodec_parameters_from_context(as->codecpar, a.get());
    as->time_base = a->time_base;
  }

  check(avio_open(&oc->pb, file.c_str(), AVIO_FLAG_WRITE), "avio_open");
  check(avformat_write_header(oc, nullptr), "write_header");

  FramePtr vf(av_frame_alloc());
  vf->format = AV_PIX_FMT_YUV420P;
  vf->width = width;
  vf->height = height;
  check(av_frame_get_buffer(vf.get(), 0), "frame buffer");
  FramePtr af;
  if (a) {
    af.reset(av_frame_alloc());
    af->format = AV_SAMPLE_FMT_S16;
    av_channel_layout_copy(&af->ch_layout, &a->ch_layout);
    af->sample_rate = 48000;
    af->nb_samples = a->frame_size;
    check(av_frame_get_buffer(af.get(), 0), "audio buffer");
  }

  int64_t audioPts = 0;
  for (int i = 0; i < fps * seconds; ++i) {
    check(av_frame_make_writable(vf.get()), "writable");
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x)
        vf->data[0][y * vf->linesize[0] + x] = static_cast<uint8_t>(16 + (x + i * 4) % 220);
    for (int y = 0; y < height / 2; ++y)
      for (int x = 0; x < width / 2; ++x) {
        vf->data[1][y * vf->linesize[1] + x] = 128;
        vf->data[2][y * vf->linesize[2] + x] = static_cast<uint8_t>(64 + i % 128);
      }
    vf->pts = i;
    check(avcodec_send_frame(v.get(), vf.get()), "send video");
    drain(oc, v.get(), vs);
    while (a && audioPts < static_cast<int64_t>(i + 1) * 48000 / fps) {
      check(av_frame_make_writable(af.get()), "writable audio");
      auto* s = reinterpret_cast<int16_t*>(af->data[0]);
      for (int n = 0; n < af->nb_samples; ++n) {
        auto val = static_cast<int16_t>(std::sin(2.0 * 3.141592653589793 * 1000.0 * (audioPts + n) / 48000.0) * 8000);
        s[2 * n] = s[2 * n + 1] = val;
      }
      af->pts = audioPts;
      audioPts += af->nb_samples;
      check(avcodec_send_frame(a.get(), af.get()), "send audio");
      drain(oc, a.get(), as);
    }
  }
  avcodec_send_frame(v.get(), nullptr);
  drain(oc, v.get(), vs);
  if (a) {
    avcodec_send_frame(a.get(), nullptr);
    drain(oc, a.get(), as);
  }
  check(av_write_trailer(oc), "trailer");
  avio_closep(&oc->pb);
}

TsPusher::TsPusher(std::filesystem::path file, std::string url, bool loop)
    : file_(std::move(file)), url_(std::move(url)), loop_(loop), thread_([this] { run(); }) {}

TsPusher::~TsPusher() { stop(); }

void TsPusher::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

int TsPusher::interrupt(void* self) { return static_cast<TsPusher*>(self)->stop_ ? 1 : 0; }

void TsPusher::run() {
  using namespace std::chrono;
  try {
    while (!stop_) {
      AVFormatContext* in = nullptr;
      check(avformat_open_input(&in, file_.string().c_str(), nullptr, nullptr), "open test file");
      std::unique_ptr<AVFormatContext*, void (*)(AVFormatContext**)> inGuard(&in, avformat_close_input);
      check(avformat_find_stream_info(in, nullptr), "stream info");

      AVFormatContext* out = nullptr;
      check(avformat_alloc_output_context2(&out, nullptr, "mpegts", url_.c_str()), "alloc output");
      std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> outGuard(out, &avformat_free_context);
      for (unsigned i = 0; i < in->nb_streams; ++i) {
        AVStream* o = avformat_new_stream(out, nullptr);
        avcodec_parameters_copy(o->codecpar, in->streams[i]->codecpar);
        o->codecpar->codec_tag = 0;
        o->time_base = in->streams[i]->time_base;
      }
      AVIOInterruptCB cb{&TsPusher::interrupt, this};
      int rc = AVERROR(ECONNREFUSED);
      while (!stop_ && (rc = avio_open2(&out->pb, url_.c_str(), AVIO_FLAG_WRITE, &cb, nullptr)) < 0) {
        std::this_thread::sleep_for(200ms);  // receiver not up yet
      }
      if (stop_) return;
      check(avformat_write_header(out, nullptr), "write header");

      PacketPtr pkt(av_packet_alloc());
      const auto t0 = steady_clock::now();
      int64_t firstUs = AV_NOPTS_VALUE, offsetUs = 0, lastUs = 0;
      while (!stop_) {
        rc = av_read_frame(in, pkt.get());
        if (rc == AVERROR_EOF) {
          if (!loop_) break;
          offsetUs = lastUs + 20'000;
          av_seek_frame(in, -1, 0, AVSEEK_FLAG_BACKWARD);
          firstUs = AV_NOPTS_VALUE;
          continue;
        }
        check(rc, "read test file");
        AVStream* is = in->streams[pkt->stream_index];
        const int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        int64_t us = av_rescale_q(ts, is->time_base, {1, 1'000'000});
        if (firstUs == AV_NOPTS_VALUE) firstUs = us;
        us = us - firstUs + offsetUs;
        lastUs = std::max(lastUs, us);
        while (!stop_ && steady_clock::now() < t0 + microseconds(us)) std::this_thread::sleep_for(1ms);
        const int64_t shift = av_rescale_q(offsetUs, {1, 1'000'000}, is->time_base) -
                              av_rescale_q(firstUs, {1, 1'000'000}, is->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += shift;
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += shift;
        av_packet_rescale_ts(pkt.get(), is->time_base, out->streams[pkt->stream_index]->time_base);
        rc = av_interleaved_write_frame(out, pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0) break;  // receiver went away: reconnect from the top
      }
      av_write_trailer(out);
      avio_closep(&out->pb);
      if (!loop_) return;
    }
  } catch (const std::exception& e) {
    error_ = e.what();
    failed_ = true;
  }
}
}  // namespace bc::test
```

Note: after a seek, `firstUs` resets so the second pass restarts its clock at `offsetUs`; `shift` maps each pass onto one continuous timeline.

- [ ] **Step 3: Write the failing input tests**

`tests/ffmpeg_input_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <condition_variable>
#include <functional>
#include <mutex>

#include "gpu/device.h"
#include "io/ffmpeg_input.h"
#include "io/srt_params.h"
#include "support/requires.h"
#include "support/temp_dir.h"
#include "support/test_media.h"

using namespace std::chrono_literals;

namespace {
struct RecordingSink : bc::InputSink {
  std::mutex m;
  std::condition_variable cv;
  std::vector<bc::SignalState> states;
  std::vector<std::optional<bc::ErrorInfo>> errors;
  int video = 0;
  int64_t audioFrames = 0;
  int audioRate = 0;
  std::optional<bc::PixelLayout> layout;
  uint32_t width = 0;

  void onVideo(bc::VideoFrameIn f) override {
    std::lock_guard l(m);
    ++video;
    layout = f.layout;
    width = f.tex->width;
    cv.notify_all();
  }
  void onAudio(bc::AudioBlock a) override {
    std::lock_guard l(m);
    audioFrames += static_cast<int64_t>(a.samples.size()) / a.channels;
    audioRate = a.sampleRate;
  }
  void onState(bc::SignalState s, std::optional<bc::ErrorInfo> e) override {
    std::lock_guard l(m);
    states.push_back(s);
    errors.push_back(std::move(e));
    cv.notify_all();
  }
  bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds t) {
    std::unique_lock l(m);
    return cv.wait_for(l, t, pred);
  }
  int count(bc::SignalState s) {
    return static_cast<int>(std::count(states.begin(), states.end(), s));
  }
};

struct Fixture {
  bc::test::TempDir dir;
  std::filesystem::path ts = dir.path / "clip.ts";
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  Fixture() { bc::test::writeTestTs(ts, 320, 180, 30, 2); }
};
}  // namespace

TEST_CASE("FfmpegInput: decodes a file to frames and 48 kHz stereo audio") {
  Fixture f;
  bc::FfmpegInputOptions o;
  o.url = f.ts.string();
  o.reconnect = false;
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] { return sink.count(bc::SignalState::Offline) > 0; }, 15s));
  in.stop();
  CHECK(sink.states.front() == bc::SignalState::Connecting);
  CHECK(sink.count(bc::SignalState::Live) == 1);
  CHECK(sink.video >= 55);
  CHECK(sink.audioFrames >= 80000);
  CHECK(sink.audioRate == 48000);
  CHECK(sink.width == 320);
}

TEST_CASE("FfmpegInput: the software path delivers BGRA8") {
  Fixture f;
  bc::FfmpegInputOptions o;
  o.url = f.ts.string();
  o.reconnect = false;
  o.allowHwDecode = false;
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] { return sink.video > 5; }, 10s));
  in.stop();
  CHECK(sink.layout == bc::PixelLayout::BGRA8);
}

TEST_CASE("FfmpegInput: the hardware path delivers NV12" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Fixture f;
  bc::FfmpegInputOptions o;
  o.url = f.ts.string();
  o.reconnect = false;
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  REQUIRE(sink.waitFor([&] { return sink.video > 5; }, 10s));
  in.stop();
  CHECK(sink.layout == bc::PixelLayout::NV12);
}

TEST_CASE("FfmpegInput: an SRT listener receives a caller") {
  Fixture f;
  auto o = bc::srtOptionsFromParams({{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47901}});
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  bc::test::TsPusher pusher(f.ts, "srt://127.0.0.1:47901?mode=caller", true);
  CHECK(sink.waitFor([&] { return sink.video >= 30; }, 15s));
  auto t0 = std::chrono::steady_clock::now();
  in.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
  pusher.stop();
  CHECK_FALSE(pusher.failed());
}

TEST_CASE("FfmpegInput: stop() returns promptly while a listener waits for a caller") {
  Fixture f;
  auto o = bc::srtOptionsFromParams({{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47902}});
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  std::this_thread::sleep_for(300ms);
  auto t0 = std::chrono::steady_clock::now();
  in.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
  CHECK(sink.states == std::vector{bc::SignalState::Connecting, bc::SignalState::Offline});
}

TEST_CASE("FfmpegInput: a dead caller target reports Offline with an error, then retries") {
  Fixture f;
  auto o = bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "127.0.0.1"}, {"port", 47903}});
  o.avOptions["connect_timeout"] = "1000";
  bc::FfmpegInput in(*f.gpu, f.pool, o);
  RecordingSink sink;
  in.start(sink);
  CHECK(sink.waitFor([&] { return sink.count(bc::SignalState::Connecting) >= 2; }, 10s));
  in.stop();
  REQUIRE(sink.states.size() >= 3);
  CHECK(sink.states[1] == bc::SignalState::Offline);
  REQUIRE(sink.errors[1].has_value());
  CHECK(sink.errors[1]->code == "input-failed");
  CHECK(sink.errors[1]->message.find("srt://127.0.0.1:47903") != std::string::npos);
}
```

Add `tests/srt_params_test.cpp tests/ffmpeg_input_test.cpp tests/support/test_media.cpp` to `bc_tests`, and add to `CMakeLists.txt` after `bc_gpu`:
```cmake
# ---- io ----
add_library(bc_io STATIC
  src/io/srt_params.cpp
  src/io/ffmpeg_input.cpp
  # Task 7
  # Task 8
)
target_include_directories(bc_io PUBLIC src ${FFMPEG_INCLUDE_DIRS})
target_link_directories(bc_io PUBLIC ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(bc_io PUBLIC bc_gpu ${FFMPEG_LIBRARIES})
```
and link `bc_io` into `bc_tests`.

- [ ] **Step 4: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "*srt*"`
Expected: build FAILS — `io/srt_params.h` not found.

- [ ] **Step 5: Implement `input.h` and the SRT params**

`src/io/input.h`:
```cpp
#pragma once
#include <cstdint>
#include <optional>
#include <vector>

#include "core/signal_state.h"
#include "gpu/converter.h"
#include "gpu/texture_pool.h"

namespace bc {
struct VideoFrameIn {
  TextureRef tex;  // NV12/P010 (planar SRVs) or BGRA8 (srv)
  PixelLayout layout = PixelLayout::NV12;
  bool fullRange = false;
  int64_t ptsUs = 0;
};

struct AudioBlock {
  int sampleRate = 48000;
  int channels = 2;
  std::vector<float> samples;  // interleaved
};

// Called from the input's own thread.
class InputSink {
 public:
  virtual ~InputSink() = default;
  virtual void onVideo(VideoFrameIn frame) = 0;
  virtual void onAudio(AudioBlock audio) = 0;
  virtual void onState(SignalState state, std::optional<ErrorInfo> error) = 0;
};

// spec §4.1: every adapter runs its own thread; start()/stop() never block for long.
class Input {
 public:
  virtual ~Input() = default;
  virtual void start(InputSink& sink) = 0;
  virtual void stop() = 0;
};
}  // namespace bc
```

`src/io/srt_params.h`:
```cpp
#pragma once
#include <chrono>
#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace bc {
struct FfmpegInputOptions {
  std::string url;
  std::map<std::string, std::string> avOptions;  // protocol/format options (secrets live here)
  bool reconnect = true;
  bool allowHwDecode = true;
  bool realtime = false;  // pace reads by timestamps (file inputs in tests)
  std::chrono::milliseconds readTimeout{2000};  // no packet this long = stalled
};

// Validates show params for the "srt" input kind (spec §4.2). Throws std::invalid_argument.
FfmpegInputOptions srtOptionsFromParams(const nlohmann::json& params);
}  // namespace bc
```

`src/io/srt_params.cpp`:
```cpp
#include "io/srt_params.h"

#include <stdexcept>

namespace bc {
namespace {
using nlohmann::json;

int intIn(const json& p, const char* key, int def, int lo, int hi) {
  if (!p.contains(key)) return def;
  if (!p[key].is_number_integer()) throw std::invalid_argument(std::string(key) + " must be a whole number");
  int v = p[key].get<int>();
  if (v < lo || v > hi) {
    throw std::invalid_argument(std::string(key) + " must be between " + std::to_string(lo) + " and " + std::to_string(hi));
  }
  return v;
}

std::string strOr(const json& p, const char* key, const std::string& def) {
  if (!p.contains(key)) return def;
  if (!p[key].is_string()) throw std::invalid_argument(std::string(key) + " must be text");
  return p[key].get<std::string>();
}
}  // namespace

FfmpegInputOptions srtOptionsFromParams(const json& p) {
  if (!p.is_object()) throw std::invalid_argument("SRT settings must be an object");
  FfmpegInputOptions o;
  const std::string url = strOr(p, "url", "");
  std::string mode = strOr(p, "mode", url.empty() ? "" : "caller");
  if (mode != "caller" && mode != "listener") throw std::invalid_argument("mode must be caller or listener");
  if (!url.empty()) {
    if (url.rfind("srt://", 0) != 0 || url.size() > 2048) throw std::invalid_argument("url must start with srt://");
    o.url = url;
  } else {
    if (!p.contains("port")) throw std::invalid_argument("port is required");
    const int port = intIn(p, "port", 0, 1, 65535);
    std::string host = strOr(p, "host", mode == "listener" ? "0.0.0.0" : "");
    if (host.empty()) throw std::invalid_argument("host is required in caller mode");
    o.url = "srt://" + host + ":" + std::to_string(port);
  }
  o.avOptions["mode"] = mode;
  o.avOptions["latency"] = std::to_string(intIn(p, "latencyMs", 120, 20, 8000) * 1000);
  const std::string pass = strOr(p, "passphrase", "");
  if (!pass.empty()) {
    if (pass.size() < 10 || pass.size() > 79) throw std::invalid_argument("passphrase must be 10 to 79 characters");
    o.avOptions["passphrase"] = pass;
  }
  const std::string sid = strOr(p, "streamId", "");
  if (sid.size() > 512) throw std::invalid_argument("streamId must be at most 512 characters");
  if (!sid.empty()) o.avOptions["streamid"] = sid;
  return o;
}
}  // namespace bc
```

- [ ] **Step 6: Implement `FfmpegInput`**

`src/io/ffmpeg_input.h`:
```cpp
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"
#include "io/srt_params.h"

struct AVFrame;
struct AVStream;
struct AVBufferRef;
struct SwsContext;
struct SwrContext;

namespace bc {
// Any FFmpeg URL (SRT in the product, files in tests). Decodes on the shared D3D11 device
// when it can (d3d11va), else in software to BGRA (spec §3.2). Reconnects with backoff.
class FfmpegInput final : public Input {
 public:
  FfmpegInput(GpuDevice& gpu, TexturePool& pool, FfmpegInputOptions opts);
  ~FfmpegInput() override;
  void start(InputSink& sink) override;
  void stop() override;

 private:
  void run();
  void runOnce();
  void emitVideo(AVFrame* f, AVStream* st);
  void emitAudio(AVFrame* f);
  void pace(int64_t ptsUs);
  AVBufferRef* makeHwDevice();
  std::string safeUrl() const;
  static int interruptCb(void* self);

  GpuDevice& gpu_;
  TexturePool& pool_;
  FfmpegInputOptions opts_;
  InputSink* sink_ = nullptr;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<int64_t> deadlineMs_{INT64_MAX};
  std::mutex waitMutex_;
  std::condition_variable waitCv_;
  bool wentLive_ = false;
  SwsContext* sws_ = nullptr;
  SwrContext* swr_ = nullptr;
  std::vector<uint8_t> swBuf_;
  int64_t paceFirstUs_ = INT64_MIN;
  std::chrono::steady_clock::time_point paceStart_{};
};
}  // namespace bc
```

`src/io/ffmpeg_input.cpp`:
```cpp
#include "io/ffmpeg_input.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <stdexcept>

#include "core/backoff.h"

namespace bc {
namespace {
using namespace std::chrono;

struct FmtDel { void operator()(AVFormatContext* f) const { avformat_close_input(&f); } };
struct CodecDel { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
struct PacketDel { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct FrameDel { void operator()(AVFrame* f) const { av_frame_free(&f); } };
using FmtPtr = std::unique_ptr<AVFormatContext, FmtDel>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDel>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDel>;
using FramePtr = std::unique_ptr<AVFrame, FrameDel>;

int64_t nowMs() { return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count(); }

std::string averr(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(rc, buf, sizeof(buf));
  return buf;
}

AVPixelFormat pickFormat(AVCodecContext*, const AVPixelFormat* fmts) {
  for (auto* p = fmts; *p != AV_PIX_FMT_NONE; ++p) if (*p == AV_PIX_FMT_D3D11) return *p;
  for (auto* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
    if (!(av_pix_fmt_desc_get(*p)->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
  }
  return AV_PIX_FMT_NONE;
}

CodecPtr openDecoder(AVStream* st, AVBufferRef* hw) {
  const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!codec) throw std::runtime_error(std::string("unsupported codec ") + avcodec_get_name(st->codecpar->codec_id));
  CodecPtr ctx(avcodec_alloc_context3(codec));
  avcodec_parameters_to_context(ctx.get(), st->codecpar);
  ctx->pkt_timebase = st->time_base;
  if (hw) {
    ctx->hw_device_ctx = av_buffer_ref(hw);
    ctx->get_format = &pickFormat;
    ctx->extra_hw_frames = 8;
  }
  int rc = avcodec_open2(ctx.get(), codec, nullptr);
  if (rc < 0) throw std::runtime_error("cannot open decoder: " + averr(rc));
  return ctx;
}
}  // namespace

FfmpegInput::FfmpegInput(GpuDevice& gpu, TexturePool& pool, FfmpegInputOptions opts)
    : gpu_(gpu), pool_(pool), opts_(std::move(opts)) {
  static std::once_flag once;
  std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
}

FfmpegInput::~FfmpegInput() { stop(); }

void FfmpegInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
  thread_ = std::thread([this] { run(); });
}

void FfmpegInput::stop() {
  stopping_ = true;
  waitCv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

int FfmpegInput::interruptCb(void* self) {
  auto* in = static_cast<FfmpegInput*>(self);
  return (in->stopping_ || nowMs() > in->deadlineMs_) ? 1 : 0;
}

std::string FfmpegInput::safeUrl() const { return opts_.url.substr(0, opts_.url.find('?')); }

AVBufferRef* FfmpegInput::makeHwDevice() {
  AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  auto* dctx = reinterpret_cast<AVHWDeviceContext*>(ref->data);
  auto* d3d = static_cast<AVD3D11VADeviceContext*>(dctx->hwctx);
  d3d->device = gpu_.device();
  d3d->device->AddRef();
  // FFmpeg takes our context lock, so its decode calls never interleave with ours.
  d3d->lock_ctx = &gpu_.contextMutex();
  d3d->lock = [](void* m) { static_cast<std::recursive_mutex*>(m)->lock(); };
  d3d->unlock = [](void* m) { static_cast<std::recursive_mutex*>(m)->unlock(); };
  int rc = av_hwdevice_ctx_init(ref);
  if (rc < 0) {
    av_buffer_unref(&ref);
    throw std::runtime_error("d3d11va init failed: " + averr(rc));
  }
  return ref;
}

void FfmpegInput::run() {
  Backoff backoff;
  while (!stopping_) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    wentLive_ = false;
    std::optional<ErrorInfo> err;
    try {
      runOnce();
      err = ErrorInfo{"input-ended", "the stream ended: " + safeUrl()};
    } catch (const std::exception& e) {
      err = ErrorInfo{"input-failed", e.what()};
    }
    if (wentLive_) backoff.reset();
    if (stopping_) break;
    sink_->onState(SignalState::Offline, err);
    if (!opts_.reconnect) return;
    std::unique_lock l(waitMutex_);
    waitCv_.wait_for(l, backoff.next(), [&] { return stopping_.load(); });
  }
  sink_->onState(SignalState::Offline, std::nullopt);
}

void FfmpegInput::runOnce() {
  AVFormatContext* raw = avformat_alloc_context();
  raw->interrupt_callback = {&FfmpegInput::interruptCb, this};
  deadlineMs_ = INT64_MAX;  // a listener may wait for its caller indefinitely; stop() interrupts
  AVDictionary* dict = nullptr;
  for (auto& [k, v] : opts_.avOptions) av_dict_set(&dict, k.c_str(), v.c_str(), 0);
  av_dict_set(&dict, "analyzeduration", "1000000", 0);
  av_dict_set(&dict, "probesize", "2000000", 0);
  int rc = avformat_open_input(&raw, opts_.url.c_str(), nullptr, &dict);
  av_dict_free(&dict);
  if (rc < 0) throw std::runtime_error("cannot open " + safeUrl() + ": " + averr(rc));
  FmtPtr fmt(raw);

  deadlineMs_ = nowMs() + opts_.readTimeout.count() * 3;
  rc = avformat_find_stream_info(fmt.get(), nullptr);
  if (rc < 0) throw std::runtime_error("cannot read stream info from " + safeUrl() + ": " + averr(rc));
  const int vi = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vi < 0) throw std::runtime_error("no video stream in " + safeUrl());
  const int ai = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, vi, nullptr, 0);

  AVBufferRef* hw = nullptr;
  if (opts_.allowHwDecode && gpu_.supportsVideoDecode()) {
    try {
      hw = makeHwDevice();
    } catch (const std::exception&) {
      hw = nullptr;  // fall back to software decode
    }
  }
  std::unique_ptr<AVBufferRef*, void (*)(AVBufferRef**)> hwGuard(&hw, av_buffer_unref);
  AVStream* vst = fmt->streams[vi];
  CodecPtr vdec = openDecoder(vst, hw);
  CodecPtr adec = ai >= 0 ? openDecoder(fmt->streams[ai], nullptr) : nullptr;

  PacketPtr pkt(av_packet_alloc());
  FramePtr frame(av_frame_alloc());
  paceFirstUs_ = INT64_MIN;
  struct Cleanup {
    FfmpegInput* self;
    ~Cleanup() {
      sws_freeContext(self->sws_);
      self->sws_ = nullptr;
      swr_free(&self->swr_);
    }
  } cleanup{this};

  while (!stopping_) {
    deadlineMs_ = nowMs() + opts_.readTimeout.count();
    rc = av_read_frame(fmt.get(), pkt.get());
    if (rc == AVERROR_EOF) return;
    if (rc < 0) {
      if (stopping_) return;
      if (rc == AVERROR_EXIT) {
        throw std::runtime_error("no data from " + safeUrl() + " for " + std::to_string(opts_.readTimeout.count()) + " ms");
      }
      throw std::runtime_error("read failed on " + safeUrl() + ": " + averr(rc));
    }
    AVCodecContext* dec = pkt->stream_index == vi ? vdec.get() : (pkt->stream_index == ai ? adec.get() : nullptr);
    if (dec && avcodec_send_packet(dec, pkt.get()) >= 0) {
      while (avcodec_receive_frame(dec, frame.get()) >= 0) {
        if (dec == vdec.get()) emitVideo(frame.get(), vst);
        else emitAudio(frame.get());
        av_frame_unref(frame.get());
      }
    }
    av_packet_unref(pkt.get());
  }
}

void FfmpegInput::pace(int64_t ptsUs) {
  if (!opts_.realtime) return;
  if (paceFirstUs_ == INT64_MIN) {
    paceFirstUs_ = ptsUs;
    paceStart_ = steady_clock::now();
    return;
  }
  std::unique_lock l(waitMutex_);
  waitCv_.wait_until(l, paceStart_ + microseconds(ptsUs - paceFirstUs_), [&] { return stopping_.load(); });
}

void FfmpegInput::emitVideo(AVFrame* f, AVStream* st) {
  VideoFrameIn out;
  out.fullRange = f->color_range == AVCOL_RANGE_JPEG;
  out.ptsUs = f->best_effort_timestamp == AV_NOPTS_VALUE ? 0 : av_rescale_q(f->best_effort_timestamp, st->time_base, {1, 1'000'000});
  const uint32_t w = static_cast<uint32_t>(f->width) & ~1u;
  const uint32_t h = static_cast<uint32_t>(f->height) & ~1u;
  if (f->format == AV_PIX_FMT_D3D11) {
    auto* src = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
    const UINT index = static_cast<UINT>(reinterpret_cast<intptr_t>(f->data[1]));
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    out.layout = d.Format == DXGI_FORMAT_P010 ? PixelLayout::P010 : PixelLayout::NV12;
    out.tex = pool_.acquire(w, h, d.Format, D3D11_BIND_SHADER_RESOURCE);
    const D3D11_BOX box{0, 0, 0, w, h, 1};
    auto lock = gpu_.lock();
    gpu_.context()->CopySubresourceRegion(out.tex->tex.Get(), 0, 0, 0, 0, src, index, &box);
  } else {
    sws_ = sws_getCachedContext(sws_, f->width, f->height, static_cast<AVPixelFormat>(f->format), static_cast<int>(w),
                                static_cast<int>(h), AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) throw std::runtime_error("cannot convert pixel format");
    const int cs = f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    sws_setColorspaceDetails(sws_, sws_getCoefficients(cs), f->color_range == AVCOL_RANGE_JPEG,
                             sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    swBuf_.resize(static_cast<size_t>(w) * h * 4);
    uint8_t* dst[1] = {swBuf_.data()};
    const int dstStride[1] = {static_cast<int>(w * 4)};
    sws_scale(sws_, f->data, f->linesize, 0, f->height, dst, dstStride);
    out.tex = pool_.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    {
      auto lock = gpu_.lock();
      gpu_.context()->UpdateSubresource(out.tex->tex.Get(), 0, nullptr, swBuf_.data(), w * 4, 0);
    }
    out.layout = PixelLayout::BGRA8;
    out.fullRange = true;
  }
  pace(out.ptsUs);
  if (!wentLive_) {
    wentLive_ = true;
    sink_->onState(SignalState::Live, std::nullopt);
  }
  sink_->onVideo(std::move(out));
}

void FfmpegInput::emitAudio(AVFrame* f) {
  if (!swr_) {
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_FLT, 48000, &f->ch_layout,
                            static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr) < 0 ||
        swr_init(swr_) < 0) {
      throw std::runtime_error("cannot set up audio resampling");
    }
  }
  // ponytail: the resampler is built from the first frame; a mid-stream format change is
  // not followed. Rebuild on change if a real source does that.
  const int maxOut = swr_get_out_samples(swr_, f->nb_samples);
  AudioBlock blk;
  blk.samples.resize(static_cast<size_t>(maxOut) * 2);
  uint8_t* outPtr[1] = {reinterpret_cast<uint8_t*>(blk.samples.data())};
  const int got = swr_convert(swr_, outPtr, maxOut, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
  if (got <= 0) return;
  blk.samples.resize(static_cast<size_t>(got) * 2);
  sink_->onAudio(std::move(blk));
}
}  // namespace bc
```

- [ ] **Step 7: Run the tests to verify they pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "*srt*"` then `-Filter "FfmpegInput*"`
Expected: srtOptionsFromParams 4 cases pass; FfmpegInput 6 cases pass (the NV12 case passes on the RX 6600 and is skipped under WARP). If Windows Firewall prompts for `bc_tests.exe`, the loopback tests still work — ledger it.

- [ ] **Step 8: Commit**

```bash
git add src/io tests CMakeLists.txt
git commit -m "feat(io): FFmpeg/SRT input with d3d11va or software decode, audio and reconnect"
```

### Task 7: Output base (queue + worker + retry) and the fullscreen output

**Files:**
- Create: `src/io/output.h`, `src/io/queued_output.h`, `src/io/queued_output.cpp`, `src/io/monitors.h`, `src/io/monitors.cpp`, `src/io/fullscreen_output.h`, `src/io/fullscreen_output.cpp`
- Create: `src/gpu/shaders/fullscreen_vs.hlsl`, `src/gpu/shaders/fullscreen_ps.hlsl`
- Create: `tests/support/fake_output.h`
- Modify: `CMakeLists.txt` (`bc_shader(fullscreen_vs vs_5_0)`, `bc_shader(fullscreen_ps ps_5_0)`; add the three `.cpp` to `bc_io` under `# Task 7`; `target_link_libraries(bc_io PUBLIC user32)`; tests)
- Test: `tests/queued_output_test.cpp`, `tests/fullscreen_output_test.cpp`

**Interfaces:**
- Consumes: `BoundedQueue`, `Backoff` (Task 2); `SignalState`, `ErrorInfo` (Task 4); `GpuDevice`, `TextureRef`, `CpuImage`, `Converter` (Task 5); `Rate` (Task 1).
- Produces:
  - `bc::OutFrame { uint64_t tick; Rate rate; TextureRef linear; std::shared_ptr<const CpuImage> bgra; std::shared_ptr<const std::vector<float>> audio; int sampleRate = 48000; int channels = 2; bool hasAlpha = false; }`.
  - `bc::OutputStatus { SignalState state = Ready; std::optional<ErrorInfo> error; uint64_t dropped = 0; uint64_t delivered = 0; }`.
  - `class bc::Output { virtual bool needsCpu() const = 0; virtual void start() = 0; virtual void submit(OutFrame) = 0; virtual void stop() = 0; virtual OutputStatus status() const = 0; }`.
  - `class bc::QueuedOutput : public Output` — subclasses implement `open()`, `deliver(const OutFrame&)`, `close() noexcept`, optionally `errorCode()`; **every subclass destructor must call `stop()`**.
  - `bc::MonitorInfo { int index; std::string name; int x, y, width, height; bool primary; }`, `std::vector<MonitorInfo> bc::listMonitors()` (primary first, then left to right).
  - `bc::FullscreenOutput(GpuDevice&, int monitorIndex)`.
  - Test helper `bc::test::FakeOutput` (see Step 1).

Output state machine (spec §3.6, §6.1): `Ready` before `start()`; `Connecting` while `open()` runs and until the first frame is delivered; `Live` after the first delivered frame; on any exception from `open()`/`deliver()`: `close()`, `Offline` + `{code: errorCode(), message: what()}`, wait the backoff, then `open()` again. `stop()` ends at `Offline` with no error. A failing output never touches the queue or thread of another.

- [ ] **Step 1: Write the fake output and the failing tests**

`tests/support/fake_output.h`:
```cpp
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "io/queued_output.h"

namespace bc::test {
// Records what it receives. Can be slowed down or made to throw.
class FakeOutput final : public bc::QueuedOutput {
 public:
  struct Seen {
    uint64_t tick;
    std::chrono::steady_clock::time_point at;
    std::array<uint8_t, 4> firstPixel;  // BGRA of pixel (0,0); zeros when no CPU image
    uint32_t width;
    size_t audioSamples;
  };

  explicit FakeOutput(bool needsCpu = true) : needsCpu_(needsCpu) {}
  ~FakeOutput() override { stop(); }
  bool needsCpu() const override { return needsCpu_; }

  std::atomic<int> opens{0};
  std::atomic<int> delivered{0};
  std::atomic<int> failOnDelivery{-1};  // throw when the delivery count reaches this
  std::atomic<int> delayMs{0};

  std::vector<Seen> seen() {
    std::lock_guard l(m_);
    return seen_;
  }

 protected:
  void open() override { ++opens; }
  void deliver(const bc::OutFrame& f) override {
    if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs.load()));
    const int n = ++delivered;
    if (n == failOnDelivery) throw std::runtime_error("boom");
    Seen s{f.tick, std::chrono::steady_clock::now(), {0, 0, 0, 0}, 0, f.audio ? f.audio->size() : 0};
    if (f.bgra && !f.bgra->bgra.empty()) {
      std::copy_n(f.bgra->bgra.begin(), 4, s.firstPixel.begin());
      s.width = f.bgra->width;
    } else if (f.linear) {
      s.width = f.linear->width;
    }
    std::lock_guard l(m_);
    seen_.push_back(s);
  }
  void close() noexcept override {}

 private:
  bool needsCpu_;
  std::mutex m_;
  std::vector<Seen> seen_;
};
}  // namespace bc::test
```

`tests/queued_output_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "support/fake_output.h"

using namespace std::chrono_literals;

namespace {
bc::OutFrame frame(uint64_t tick) {
  bc::OutFrame f;
  f.tick = tick;
  return f;
}
template <class P>
bool waitUntil(P pred, std::chrono::milliseconds t) {
  auto end = std::chrono::steady_clock::now() + t;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return pred();
}
}  // namespace

TEST_CASE("QueuedOutput: Ready, then Live after the first delivered frame") {
  bc::test::FakeOutput out;
  CHECK(out.status().state == bc::SignalState::Ready);
  out.start();
  out.submit(frame(1));
  CHECK(waitUntil([&] { return out.status().state == bc::SignalState::Live; }, 1000ms));
  out.stop();
  CHECK(out.status().state == bc::SignalState::Offline);
  CHECK_FALSE(out.status().error);
}

TEST_CASE("QueuedOutput: a slow output never blocks submit and drops its own oldest frames") {
  bc::test::FakeOutput out;
  out.delayMs = 100;
  out.start();
  auto t0 = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < 10; ++i) out.submit(frame(i));
  CHECK(std::chrono::steady_clock::now() - t0 < 20ms);
  CHECK(waitUntil([&] { return out.status().dropped >= 7; }, 500ms));
  out.stop();
}

TEST_CASE("QueuedOutput: a delivery exception goes Offline with an error, then recovers") {
  bc::test::FakeOutput out;
  out.failOnDelivery = 2;
  out.start();
  out.submit(frame(1));
  CHECK(waitUntil([&] { return out.delivered >= 1; }, 1000ms));
  out.submit(frame(2));
  CHECK(waitUntil([&] { return out.status().state == bc::SignalState::Offline; }, 1000ms));
  auto st = out.status();
  REQUIRE(st.error);
  CHECK(st.error->code == "output-failed");
  CHECK(st.error->message == "boom");
  CHECK(waitUntil([&] { return out.opens >= 2; }, 2000ms));  // reopened after the 500 ms backoff
  out.submit(frame(3));
  CHECK(waitUntil([&] { return out.status().state == bc::SignalState::Live; }, 1000ms));
  CHECK_FALSE(out.status().error);
  out.stop();
}

TEST_CASE("QueuedOutput: stop joins quickly even while idle") {
  bc::test::FakeOutput out;
  out.start();
  auto t0 = std::chrono::steady_clock::now();
  out.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 300ms);
}
```

`tests/fullscreen_output_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <thread>

#include "gpu/converter.h"
#include "io/fullscreen_output.h"
#include "io/monitors.h"

using namespace std::chrono_literals;

TEST_CASE("listMonitors: at least one, primary first, indexes in order") {
  auto mons = bc::listMonitors();
  REQUIRE_FALSE(mons.empty());
  CHECK(mons[0].primary);
  for (size_t i = 0; i < mons.size(); ++i) CHECK(mons[i].index == static_cast<int>(i));
}

TEST_CASE("FullscreenOutput: a missing monitor reports an error instead of crashing") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::FullscreenOutput out(*gpu, 99);
  out.start();
  std::this_thread::sleep_for(300ms);
  auto st = out.status();
  CHECK(st.state == bc::SignalState::Offline);
  REQUIRE(st.error);
  CHECK(st.error->code == "fullscreen-failed");
  CHECK(st.error->message.find("monitor 99") != std::string::npos);
  out.stop();
}

TEST_CASE("FullscreenOutput: presents frames on monitor 0") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::FullscreenOutput out(*gpu, 0);
  out.start();
  for (uint64_t i = 0; i < 30; ++i) {
    bc::OutFrame f;
    f.tick = i;
    f.linear = conv.solid(1920, 1080, {0.2f, 0.02f, 0.02f, 1.0f});
    out.submit(std::move(f));
    std::this_thread::sleep_for(16ms);
  }
  auto st = out.status();
  CHECK(st.state == bc::SignalState::Live);
  CHECK_FALSE(st.error);
  CHECK(st.delivered > 0);
  out.stop();
}
```
(The last test flashes a dark-red window on the primary monitor for half a second. That is intended.)

Add `tests/queued_output_test.cpp tests/fullscreen_output_test.cpp` to `bc_tests`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "QueuedOutput*"`
Expected: build FAILS — `io/queued_output.h` not found.

- [ ] **Step 3: Implement `output.h` and `QueuedOutput`**

`src/io/output.h`:
```cpp
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "core/rate.h"
#include "core/signal_state.h"
#include "gpu/converter.h"
#include "gpu/texture_pool.h"

namespace bc {
// One tick's frame for one camera, shared by all of its outputs (no copies, spec §3.2).
struct OutFrame {
  uint64_t tick = 0;
  Rate rate;
  TextureRef linear;                            // RGBA16F linear
  std::shared_ptr<const CpuImage> bgra;         // set when any output needsCpu()
  std::shared_ptr<const std::vector<float>> audio;  // interleaved, this tick's samples
  int sampleRate = 48000;
  int channels = 2;
  bool hasAlpha = false;
};

struct OutputStatus {
  SignalState state = SignalState::Ready;
  std::optional<ErrorInfo> error;
  uint64_t dropped = 0;
  uint64_t delivered = 0;
};

class Output {
 public:
  virtual ~Output() = default;
  virtual bool needsCpu() const = 0;
  virtual void start() = 0;
  virtual void submit(OutFrame frame) = 0;  // never blocks
  virtual void stop() = 0;
  virtual OutputStatus status() const = 0;
};
}  // namespace bc
```

`src/io/queued_output.h`:
```cpp
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "core/bounded_queue.h"
#include "io/output.h"

namespace bc {
// Drop-oldest queue of 2 + one worker thread + retry with backoff (spec §4.1, §3.6).
// Subclass destructors MUST call stop() so the worker never calls into a destroyed subclass.
class QueuedOutput : public Output {
 public:
  ~QueuedOutput() override;
  void start() override;
  void submit(OutFrame frame) override;
  void stop() override;
  OutputStatus status() const override;

 protected:
  virtual void open() = 0;                        // on the worker thread
  virtual void deliver(const OutFrame& frame) = 0;  // on the worker thread
  virtual void close() noexcept = 0;              // on the worker thread
  virtual std::string errorCode() const { return "output-failed"; }

 private:
  void run();
  void setState(SignalState s, std::optional<ErrorInfo> e);
  BoundedQueue<OutFrame> queue_{2};
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex m_;
  OutputStatus status_;
  std::mutex waitMutex_;
  std::condition_variable waitCv_;
};
}  // namespace bc
```

`src/io/queued_output.cpp`:
```cpp
#include "io/queued_output.h"

#include <cstdlib>

#include "core/backoff.h"

namespace bc {
using namespace std::chrono_literals;

QueuedOutput::~QueuedOutput() {
  if (thread_.joinable()) std::abort();  // a subclass forgot to call stop() in its destructor
}

void QueuedOutput::start() {
  if (thread_.joinable()) return;
  stopping_ = false;
  queue_.reopen();
  thread_ = std::thread([this] { run(); });
}

void QueuedOutput::submit(OutFrame frame) { queue_.push(std::move(frame)); }

void QueuedOutput::stop() {
  stopping_ = true;
  queue_.close();
  waitCv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

OutputStatus QueuedOutput::status() const {
  std::lock_guard l(m_);
  OutputStatus s = status_;
  s.dropped = queue_.dropped();
  return s;
}

void QueuedOutput::setState(SignalState s, std::optional<ErrorInfo> e) {
  std::lock_guard l(m_);
  status_.state = s;
  status_.error = std::move(e);
}

void QueuedOutput::run() {
  Backoff backoff;
  while (!stopping_) {
    setState(SignalState::Connecting, std::nullopt);
    try {
      open();
      bool live = false;
      while (!stopping_) {
        auto f = queue_.pop(100ms);
        if (!f) continue;
        deliver(*f);
        {
          std::lock_guard l(m_);
          ++status_.delivered;
        }
        if (!live) {
          live = true;
          setState(SignalState::Live, std::nullopt);
          backoff.reset();
        }
      }
      close();
    } catch (const std::exception& e) {
      close();
      setState(SignalState::Offline, ErrorInfo{errorCode(), e.what()});
      std::unique_lock l(waitMutex_);
      waitCv_.wait_for(l, backoff.next(), [&] { return stopping_.load(); });
    }
  }
  setState(SignalState::Offline, std::nullopt);
}
}  // namespace bc
```

- [ ] **Step 4: Implement monitors**

`src/io/monitors.h`:
```cpp
#pragma once
#include <string>
#include <vector>

namespace bc {
struct MonitorInfo {
  int index = 0;
  std::string name;  // e.g. "\\\\.\\DISPLAY2"
  int x = 0, y = 0, width = 0, height = 0;
  bool primary = false;
};
// Primary first, then left to right, top to bottom. Index = position in this list.
std::vector<MonitorInfo> listMonitors();
}  // namespace bc
```

`src/io/monitors.cpp`:
```cpp
#include "io/monitors.h"

#include <windows.h>

#include <algorithm>

#include "core/utf8.h"

namespace bc {
std::vector<MonitorInfo> listMonitors() {
  std::vector<MonitorInfo> out;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
          MonitorInfo m;
          m.name = toUtf8(mi.szDevice);
          m.x = mi.rcMonitor.left;
          m.y = mi.rcMonitor.top;
          m.width = mi.rcMonitor.right - mi.rcMonitor.left;
          m.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
          m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
          reinterpret_cast<std::vector<MonitorInfo>*>(lp)->push_back(m);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
  std::stable_sort(out.begin(), out.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
    if (a.primary != b.primary) return a.primary;
    if (a.x != b.x) return a.x < b.x;
    return a.y < b.y;
  });
  for (size_t i = 0; i < out.size(); ++i) out[i].index = static_cast<int>(i);
  return out;
}
}  // namespace bc
```

- [ ] **Step 5: Implement the fullscreen shaders and output**

`src/gpu/shaders/fullscreen_vs.hlsl`:
```hlsl
struct VsOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

VsOut main(uint id : SV_VertexID) {
  VsOut o;
  float2 uv = float2((id << 1) & 2, id & 2);
  o.uv = uv;
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return o;
}
```

`src/gpu/shaders/fullscreen_ps.hlsl`:
```hlsl
Texture2D<float4> gSrc : register(t0);
SamplerState gLinear : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float4 c = gSrc.Sample(gLinear, uv);
  return float4(pow(saturate(c.rgb), 1.0 / 2.4), 1.0);
}
```

`src/io/fullscreen_output.h`:
```cpp
#pragma once
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include "gpu/device.h"
#include "io/queued_output.h"

namespace bc {
// Borderless topmost window covering one monitor; presents each tick frame (spec §4.3).
// The window lives on the output's own worker thread.
// ponytail: stretches to the monitor (no letterbox); add aspect handling if a non-16:9
// screen shows up.
class FullscreenOutput final : public QueuedOutput {
 public:
  FullscreenOutput(GpuDevice& gpu, int monitorIndex);
  ~FullscreenOutput() override { stop(); }
  bool needsCpu() const override { return false; }

 protected:
  void open() override;
  void deliver(const OutFrame& frame) override;
  void close() noexcept override;
  std::string errorCode() const override { return "fullscreen-failed"; }

 private:
  GpuDevice& gpu_;
  int monitor_;
  HWND hwnd_ = nullptr;
  UINT width_ = 0, height_ = 0;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
};
}  // namespace bc
```

`src/io/fullscreen_output.cpp`:
```cpp
#include "io/fullscreen_output.h"

#include <stdexcept>

#include "fullscreen_ps.h"
#include "fullscreen_vs.h"
#include "io/monitors.h"

using Microsoft::WRL::ComPtr;

namespace bc {
namespace {
constexpr wchar_t kClass[] = L"BreadCameraFullscreen";

void registerClassOnce() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    return RegisterClassExW(&wc);
  }();
  if (!atom) throw std::runtime_error("cannot register the fullscreen window class");
}

void check(HRESULT hr, const char* what) {
  if (FAILED(hr)) throw std::runtime_error(std::string(what) + " failed (" + std::to_string(hr) + ")");
}
}  // namespace

FullscreenOutput::FullscreenOutput(GpuDevice& gpu, int monitorIndex) : gpu_(gpu), monitor_(monitorIndex) {}

void FullscreenOutput::open() {
  auto mons = listMonitors();
  if (monitor_ < 0 || monitor_ >= static_cast<int>(mons.size())) {
    throw std::runtime_error("monitor " + std::to_string(monitor_) + " not found (" + std::to_string(mons.size()) + " connected)");
  }
  const auto& m = mons[monitor_];
  registerClassOnce();
  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"BreadCamera", WS_POPUP, m.x, m.y, m.width,
                          m.height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!hwnd_) throw std::runtime_error("cannot create the fullscreen window");
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  width_ = static_cast<UINT>(m.width);
  height_ = static_cast<UINT>(m.height);

  auto* dev = gpu_.device();
  ComPtr<IDXGIDevice> dxgiDev;
  check(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)), "QueryInterface(IDXGIDevice)");
  ComPtr<IDXGIAdapter> adapter;
  check(dxgiDev->GetAdapter(&adapter), "GetAdapter");
  ComPtr<IDXGIFactory2> factory;
  check(adapter->GetParent(IID_PPV_ARGS(&factory)), "GetParent(IDXGIFactory2)");
  DXGI_SWAP_CHAIN_DESC1 d{};
  d.Width = width_;
  d.Height = height_;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc = {1, 0};
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.BufferCount = 2;
  d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  check(factory->CreateSwapChainForHwnd(dev, hwnd_, &d, nullptr, nullptr, &swap_), "CreateSwapChainForHwnd");
  factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
  ComPtr<ID3D11Texture2D> back;
  check(swap_->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
  check(dev->CreateRenderTargetView(back.Get(), nullptr, &rtv_), "CreateRenderTargetView");
  check(dev->CreateVertexShader(g_fullscreen_vs, sizeof(g_fullscreen_vs), nullptr, &vs_), "CreateVertexShader");
  check(dev->CreatePixelShader(g_fullscreen_ps, sizeof(g_fullscreen_ps), nullptr, &ps_), "CreatePixelShader");
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  check(dev->CreateSamplerState(&sd, &sampler_), "CreateSamplerState");
}

void FullscreenOutput::deliver(const OutFrame& f) {
  MSG msg;
  while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (!f.linear) return;
  {
    auto lock = gpu_.lock();
    auto* ctx = gpu_.context();
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    ctx->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, f.linear->srv.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
  }
  // Present outside the context lock: it can wait for vsync.
  check(swap_->Present(1, 0), "Present");
}

void FullscreenOutput::close() noexcept {
  rtv_.Reset();
  swap_.Reset();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}
}  // namespace bc
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "QueuedOutput*"` then `-Filter "*Fullscreen*,listMonitors*"`
Expected: 4 + 3 cases pass.

- [ ] **Step 7: Commit**

```bash
git add src tests CMakeLists.txt
git commit -m "feat(io): queued output base with retry, monitor list and fullscreen output"
```

### Task 8: NDI output (optional SDK)

**Files:**
- Create: `src/io/ndi_output.h`, `src/io/ndi_output.cpp`
- Modify: `CMakeLists.txt` (conditional NDI sources, define and DLL copy), `tests/support/requires.cpp` (no change needed — `Ndi` already reads `BC_WITH_NDI`)
- Test: `tests/ndi_output_test.cpp`

**Interfaces:**
- Consumes: `QueuedOutput`, `OutFrame` (Task 7); `CpuImage` (Task 5).
- Produces: `bc::NdiOutput(std::string sourceName)` (only when `BC_WITH_NDI=1`); `needsCpu() == true`; error code `"ndi-failed"`. CMake function `bc_copy_runtime(<target>)` that copies the NDI DLL next to an executable (no-op without NDI).

Wire format: video `NDIlib_FourCC_type_BGRA` when `OutFrame::hasAlpha`, else `NDIlib_FourCC_type_BGRX`, frame rate = show rate, progressive, synthesized timecode; audio planar float (`NDIlib_FourCC_audio_type_FLTP`) at 48 kHz with the frame's channel count. (Spec §4.3 lists "UYVA" for alpha; BGRA carries the same alpha and saves a conversion — ledger this as an authorised deviation.)

- [ ] **Step 1: Make the CMake wiring conditional**

In `CMakeLists.txt`, under the `bc_io` block, add:
```cmake
if(BC_WITH_NDI)
  target_sources(bc_io PRIVATE src/io/ndi_output.cpp)
  target_link_libraries(bc_io PUBLIC NDI::NDI)
  target_compile_definitions(bc_io PUBLIC BC_WITH_NDI=1)
else()
  target_compile_definitions(bc_io PUBLIC BC_WITH_NDI=0)
endif()

# Copies runtime DLLs that vcpkg does not manage (NDI) next to an executable.
function(bc_copy_runtime target)
  if(BC_WITH_NDI)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NDI_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
endfunction()
```
Remove the old `target_compile_definitions(bc_tests PRIVATE BC_WITH_NDI=...)` line (the define now comes from `bc_io`), and after the `bc_tests` target add `bc_copy_runtime(bc_tests)`.

- [ ] **Step 2: Write the failing test**

`tests/ndi_output_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_NDI
#include <Processing.NDI.Lib.h>

#include <string>
#include <thread>

#include "io/ndi_output.h"

using namespace std::chrono_literals;

TEST_CASE("NdiOutput: sends video and audio a receiver can read" BC_REQUIRES(bc::test::Req::Ndi)) {
  REQUIRE(NDIlib_initialize());
  const std::string name = "BreadCamera Test " + std::to_string(GetCurrentProcessId());
  bc::NdiOutput out(name);
  out.start();

  auto img = std::make_shared<bc::CpuImage>();
  img->width = 64;
  img->height = 36;
  img->bgra.resize(64 * 36 * 4);
  for (size_t i = 0; i < img->bgra.size(); i += 4) {
    img->bgra[i] = 0;        // B
    img->bgra[i + 1] = 0;    // G
    img->bgra[i + 2] = 200;  // R
    img->bgra[i + 3] = 255;
  }
  auto audio = std::make_shared<std::vector<float>>(1600 * 2, 0.25f);  // 1600 stereo frames

  std::atomic<bool> sending{true};
  std::thread sender([&] {
    uint64_t tick = 0;
    while (sending) {
      bc::OutFrame f;
      f.tick = tick++;
      f.rate = {30, 1};
      f.bgra = img;
      f.audio = audio;
      out.submit(std::move(f));
      std::this_thread::sleep_for(33ms);
    }
  });

  NDIlib_find_create_t fc{};
  fc.show_local_sources = true;
  NDIlib_find_instance_t find = NDIlib_find_create_v2(&fc);
  REQUIRE(find);
  const NDIlib_source_t* match = nullptr;
  for (int attempt = 0; attempt < 16 && !match; ++attempt) {
    NDIlib_find_wait_for_sources(find, 500);
    uint32_t n = 0;
    const NDIlib_source_t* srcs = NDIlib_find_get_current_sources(find, &n);
    for (uint32_t i = 0; i < n; ++i) {
      if (std::string(srcs[i].p_ndi_name).find(name) != std::string::npos) match = &srcs[i];
    }
  }
  REQUIRE(match);

  NDIlib_recv_create_v3_t rc{};
  rc.source_to_connect_to = *match;
  rc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
  rc.bandwidth = NDIlib_recv_bandwidth_highest;
  NDIlib_recv_instance_t recv = NDIlib_recv_create_v3(&rc);
  REQUIRE(recv);
  bool gotVideo = false, gotAudio = false;
  int red = -1, channels = 0;
  for (int i = 0; i < 200 && !(gotVideo && gotAudio); ++i) {
    NDIlib_video_frame_v2_t v{};
    NDIlib_audio_frame_v3_t a{};
    switch (NDIlib_recv_capture_v3(recv, &v, &a, nullptr, 100)) {
      case NDIlib_frame_type_video:
        gotVideo = true;
        red = v.p_data[2];
        NDIlib_recv_free_video_v2(recv, &v);
        break;
      case NDIlib_frame_type_audio:
        gotAudio = true;
        channels = a.no_channels;
        NDIlib_recv_free_audio_v3(recv, &a);
        break;
      default:
        break;
    }
  }
  NDIlib_recv_destroy(recv);
  NDIlib_find_destroy(find);
  sending = false;
  sender.join();
  out.stop();

  CHECK(gotVideo);
  CHECK(std::abs(red - 200) <= 8);
  CHECK(gotAudio);
  CHECK(channels == 2);
  CHECK(out.status().error == std::nullopt);
}
#else
TEST_CASE("NdiOutput: sends video and audio a receiver can read" BC_REQUIRES(bc::test::Req::Ndi)) {}
#endif
```

Add `tests/ndi_output_test.cpp` to `bc_tests`.

- [ ] **Step 3: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "NdiOutput*"`
Expected with the NDI SDK installed: build FAILS — `io/ndi_output.h` not found. Without the SDK: the test is reported as **skipped** with `[skip] ... NDI SDK not built`; ledger that the SDK is missing and continue to Step 4 anyway (the code must still be written; it compiles once the SDK is installed).

- [ ] **Step 4: Implement `NdiOutput`**

`src/io/ndi_output.h`:
```cpp
#pragma once
#include <string>
#include <vector>

#include "io/queued_output.h"

struct NDIlib_send_instance_type;

namespace bc {
// NDI sender, one per camera output (spec §4.3). Needs the CPU readback.
class NdiOutput final : public QueuedOutput {
 public:
  explicit NdiOutput(std::string sourceName);
  ~NdiOutput() override { stop(); }
  bool needsCpu() const override { return true; }

 protected:
  void open() override;
  void deliver(const OutFrame& frame) override;
  void close() noexcept override;
  std::string errorCode() const override { return "ndi-failed"; }

 private:
  std::string name_;
  NDIlib_send_instance_type* send_ = nullptr;
  std::vector<float> planar_;
};
}  // namespace bc
```

`src/io/ndi_output.cpp`:
```cpp
#include "io/ndi_output.h"

#include <Processing.NDI.Lib.h>

#include <mutex>
#include <stdexcept>

namespace bc {
NdiOutput::NdiOutput(std::string sourceName) : name_(std::move(sourceName)) {}

void NdiOutput::open() {
  static const bool ok = NDIlib_initialize();
  if (!ok) throw std::runtime_error("the NDI runtime could not start on this CPU");
  NDIlib_send_create_t c{};
  c.p_ndi_name = name_.c_str();
  c.clock_video = false;  // the master tick paces us
  c.clock_audio = false;
  send_ = NDIlib_send_create(&c);
  if (!send_) throw std::runtime_error("cannot create NDI source '" + name_ + "'");
}

void NdiOutput::deliver(const OutFrame& f) {
  if (f.bgra && !f.bgra->bgra.empty()) {
    NDIlib_video_frame_v2_t v{};
    v.xres = static_cast<int>(f.bgra->width);
    v.yres = static_cast<int>(f.bgra->height);
    v.FourCC = f.hasAlpha ? NDIlib_FourCC_type_BGRA : NDIlib_FourCC_type_BGRX;
    v.frame_rate_N = f.rate.num;
    v.frame_rate_D = f.rate.den;
    v.picture_aspect_ratio = 0.0f;  // = xres / yres
    v.frame_format_type = NDIlib_frame_format_type_progressive;
    v.timecode = NDIlib_send_timecode_synthesize;
    v.p_data = const_cast<uint8_t*>(f.bgra->bgra.data());
    v.line_stride_in_bytes = v.xres * 4;
    NDIlib_send_send_video_v2(send_, &v);
  }
  if (f.audio && !f.audio->empty()) {
    const int ch = f.channels;
    const int n = static_cast<int>(f.audio->size()) / ch;
    planar_.resize(f.audio->size());
    for (int c = 0; c < ch; ++c)
      for (int i = 0; i < n; ++i) planar_[static_cast<size_t>(c) * n + i] = (*f.audio)[static_cast<size_t>(i) * ch + c];
    NDIlib_audio_frame_v3_t a{};
    a.sample_rate = f.sampleRate;
    a.no_channels = ch;
    a.no_samples = n;
    a.timecode = NDIlib_send_timecode_synthesize;
    a.FourCC = NDIlib_FourCC_audio_type_FLTP;
    a.p_data = reinterpret_cast<uint8_t*>(planar_.data());
    a.channel_stride_in_bytes = n * static_cast<int>(sizeof(float));
    NDIlib_send_send_audio_v3(send_, &a);
  }
}

void NdiOutput::close() noexcept {
  if (send_) {
    NDIlib_send_destroy(send_);
    send_ = nullptr;
  }
}
}  // namespace bc
```

Note: the NDI headers declare the instance types as `NDIlib_send_instance_type*` typedefs; if the forward declaration in the header clashes with the SDK's typedef, change the member to `void* send_` and cast at the three call sites — ledger whichever compiles.

- [ ] **Step 5: Run the test**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "NdiOutput*"`
Expected with the SDK: PASS (1 case). Without it: 1 skipped. Run the whole suite once: all previous cases still pass.

- [ ] **Step 6: Commit**

```bash
git add src/io CMakeLists.txt tests
git commit -m "feat(io): NDI output with alpha and planar audio (optional SDK)"
```

### Task 9: Engine core — adapter factory, cameras, master tick, hold/slate, audio, status

**Files:**
- Create: `src/engine/adapter_factory.h`, `src/engine/adapter_factory.cpp`, `src/engine/camera.h`, `src/engine/camera.cpp`, `src/engine/engine.h`, `src/engine/engine.cpp`
- Create: `tests/support/fake_input.h`
- Modify: `CMakeLists.txt` (`bc_engine` library; link into `bc_tests`)
- Test: `tests/engine_test.cpp`

**Interfaces:**
- Consumes: everything from Tasks 2–8: `FrameSync`, `SyncPolicy`, `FeedPhase`, `TickSchedule`, `AudioDelay`, `Show`/`CameraConfig`/`OutputConfig`/`ShowSettings`, `StateStore`, `errorToJson`, `Converter`, `TextureRef`, `loadImageBgra`, `Input`/`InputSink`/`VideoFrameIn`/`AudioBlock`, `Output`/`OutFrame`, `FfmpegInput`, `srtOptionsFromParams`, `FullscreenOutput`, `NdiOutput`.
- Produces:
  - `class bc::AdapterFactory { virtual std::unique_ptr<Input> makeInput(const InputConfig&) = 0; virtual std::unique_ptr<Output> makeOutput(const OutputConfig&, const CameraConfig&) = 0; }` (both throw `std::invalid_argument` on bad config); `bc::DefaultAdapterFactory(GpuDevice&, TexturePool&)`.
  - `bc::CameraStats { double fps; uint64_t drops, repeats; double latencyMs; FeedPhase phase; }`.
  - `bc::Camera(CameraConfig, AdapterFactory&, Converter&, StateStore&, SyncPolicy)`: `start()`, `stop()`, `reconfigure(const CameraConfig&)`, `setPolicy(SyncPolicy)`, `tick(uint64_t, const TickSchedule&, time_point, const TextureRef& slate)`, `TextureRef lastFrame() const`, `void publishStatus()`, `CameraStats stats() const`, `const CameraConfig& config() const`.
  - `bc::Engine(Converter&, StateStore&, AdapterFactory&)`: `void apply(const Show&)`, `void start()`, `void stop()`, `EngineStats stats() const` (`EngineStats { uint64_t overruns; std::map<std::string, CameraStats> cameras; }`), `void setFrameTap(FrameTap)` where `using FrameTap = std::function<void(uint64_t tick, const std::vector<std::pair<std::string, TextureRef>>& frames)>` (set before `start()`; runs on the tick thread after all cameras ticked).

Behaviour (spec §3.3, §3.5, §3.6):
- One tick thread at the show rate (high-resolution waitable timer, deadlines from `TickSchedule`). After an overrun it jumps to the next future tick and adds the skipped count to `overruns`.
- Per camera per tick: pick the newest frame; convert only when it is fresh (repeats reuse the last converted texture); `NoSignal`/`Slate` → the slate texture; the frame is never null.
- One CPU readback per camera per tick, only if an enabled output `needsCpu()`.
- Audio: `AudioDelay(48000, 2)`; each tick pulls exactly `audioFramesForTick` frames; the delay follows the measured latency (EWMA of arrival → submit), re-set when it moves by more than 10 ms, rounded to 5 ms.
- An exception from one camera's tick is caught, reported at `/cameras/<id>/tickError`, and the other cameras still tick.
- Default slate: solid linear `(0.05, 0.05, 0.06)` at 1920×1080 (dark grey, never black). A `slatePath` that fails to load → default slate + `/engine/warnings/slate` message.
- State written by this task: `/cameras/<id>/name`, `/phase`, `/input/state`, `/input/error`, `/outputs/<oid>/kind|state|error`, `/tickError`; `/engine/rate`, `/engine/overruns`, `/engine/warnings/slate`. Output state for a disabled output is `"ready"`; an output that could not be created is `"offline"` with `{code:"output-config"}`; an input that could not be created is `"offline"` with `{code:"input-config"}`.

- [ ] **Step 1: Write the fake input and factory test helpers**

`tests/support/fake_input.h`:
```cpp
#pragma once
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "engine/adapter_factory.h"
#include "gpu/converter.h"
#include "support/fake_output.h"

namespace bc::test {
// Pushes solid BGRA frames + matching audio at `fps` until paused or stopped.
class FakeInput final : public bc::Input {
 public:
  FakeInput(bc::Converter& conv, int fps, uint32_t w, uint32_t h, std::array<uint8_t, 4> bgra)
      : conv_(conv), fps_(fps), color_(bgra) {
    width = w;
    height = h;
  }
  ~FakeInput() override { stop(); }
  std::atomic<bool> paused{false};
  std::atomic<uint32_t> width{0}, height{0};

  void start(bc::InputSink& sink) override {
    running_ = true;
    thread_ = std::thread([this, &sink] {
      sink.onState(bc::SignalState::Live, std::nullopt);
      int64_t n = 0;
      auto t0 = std::chrono::steady_clock::now();
      while (running_) {
        if (!paused) {
          const uint32_t w = width, h = height;
          std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
          for (size_t i = 0; i < px.size(); i += 4) std::copy(color_.begin(), color_.end(), px.begin() + i);
          bc::VideoFrameIn f{conv_.uploadBgra8(px.data(), w, h, w * 4), bc::PixelLayout::BGRA8, true, n * 1'000'000 / fps_};
          sink.onVideo(std::move(f));
          sink.onAudio(bc::AudioBlock{48000, 2, std::vector<float>(48000 / fps_ * 2, 0.1f)});
        }
        ++n;
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(n * 1'000'000 / fps_));
      }
    });
  }
  void stop() override {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }

 private:
  bc::Converter& conv_;
  int fps_;
  std::array<uint8_t, 4> color_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

// Input kind "fake": params {tag, fps, width, height, bgra:[b,g,r,a]}.
// Output kind "fake": params {cpu (bool, default true), delayMs}. Anything else throws.
class FakeFactory final : public bc::AdapterFactory {
 public:
  explicit FakeFactory(bc::Converter& conv) : conv_(conv) {}
  std::unique_ptr<bc::Input> makeInput(const bc::InputConfig& cfg) override {
    if (cfg.kind != "fake") throw std::invalid_argument("unknown input kind " + cfg.kind);
    auto& p = cfg.params;
    auto c = p.value("bgra", std::vector<int>{0, 0, 200, 255});
    auto in = std::make_unique<FakeInput>(conv_, p.value("fps", 30), p.value("width", 64u), p.value("height", 36u),
                                          std::array<uint8_t, 4>{uint8_t(c[0]), uint8_t(c[1]), uint8_t(c[2]), uint8_t(c[3])});
    std::lock_guard l(m);
    inputs[p.value("tag", std::string("in"))] = in.get();
    return in;
  }
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig& cfg, const bc::CameraConfig&) override {
    if (cfg.kind != "fake") throw std::invalid_argument("unknown output kind " + cfg.kind);
    auto out = std::make_unique<FakeOutput>(cfg.params.value("cpu", true));
    out->delayMs = cfg.params.value("delayMs", 0);
    std::lock_guard l(m);
    outputs[cfg.id] = out.get();
    return out;
  }
  FakeInput* input(const std::string& tag) {
    std::lock_guard l(m);
    return inputs.at(tag);
  }
  FakeOutput* output(const std::string& id) {
    std::lock_guard l(m);
    return outputs.at(id);
  }

 private:
  bc::Converter& conv_;
  std::mutex m;
  std::map<std::string, FakeInput*> inputs;
  std::map<std::string, FakeOutput*> outputs;
};
}  // namespace bc::test
```

Raw pointers in the factory are only valid while the engine owns the adapters; tests read them before removing cameras.

- [ ] **Step 2: Write the failing engine tests**

`tests/engine_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <set>
#include <thread>

#include "core/state_store.h"
#include "engine/engine.h"
#include "gpu/device.h"
#include "support/fake_input.h"

using namespace std::chrono_literals;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::StateStore state;
  bc::test::FakeFactory factory{conv};
  bc::Engine engine{conv, state, factory};
  ~Rig() { engine.stop(); }
};

bc::CameraConfig cam(const std::string& id, std::vector<bc::OutputConfig> outs, json inParams = json::object()) {
  bc::CameraConfig c;
  c.id = id;
  c.name = "Cam " + id;
  c.input.kind = "fake";
  c.input.params = inParams;
  if (!c.input.params.contains("tag")) c.input.params["tag"] = id;
  c.outputs = std::move(outs);
  return c;
}

bc::OutputConfig out(const std::string& id, json params = json::object(), const std::string& kind = "fake") {
  return {id, kind, false, true, std::move(params)};
}

int countBetween(const std::vector<bc::test::FakeOutput::Seen>& seen, Clock::time_point a, Clock::time_point b) {
  int n = 0;
  for (auto& s : seen) if (s.at >= a && s.at < b) ++n;
  return n;
}

std::chrono::milliseconds maxGap(const std::vector<bc::test::FakeOutput::Seen>& seen) {
  std::chrono::milliseconds g{0};
  for (size_t i = 1; i < seen.size(); ++i) {
    g = std::max(g, std::chrono::duration_cast<std::chrono::milliseconds>(seen[i].at - seen[i - 1].at));
  }
  return g;
}

std::string phaseOf(bc::StateStore& s, const std::string& id) {
  auto snap = s.snapshot();
  auto p = json::json_pointer("/cameras/" + id + "/phase");
  return snap.contains(p) ? snap.at(p).get<std::string>() : "";
}
}  // namespace

TEST_CASE("Engine: a 30 fps input becomes a steady 60 fps output") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {out("o1")}, {{"fps", 30}, {"bgra", {10, 20, 200, 255}}}));
  r.engine.apply(show);
  r.engine.start();
  auto t0 = Clock::now();
  std::this_thread::sleep_for(1600ms);
  auto seen = r.factory.output("o1")->seen();
  int n = countBetween(seen, t0 + 500ms, t0 + 1500ms);
  CHECK(n >= 55);
  CHECK(n <= 62);
  CHECK(maxGap(seen) < 50ms);
  auto& last = seen.back();
  CHECK(std::abs(last.firstPixel[0] - 10) <= 2);
  CHECK(std::abs(last.firstPixel[2] - 200) <= 2);
  CHECK(r.engine.stats().cameras.at("a").repeats > 0);
}

TEST_CASE("Engine: each tick carries exactly its share of audio") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {out("o1")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  for (auto& s : r.factory.output("o1")->seen()) CHECK(s.audioSamples == 800 * 2);
}

TEST_CASE("Engine: a stalled input holds, then shows the slate; outputs never stop") {
  Rig r;
  bc::Show show;
  show.settings.holdMs = 500;
  show.cameras.push_back(cam("a", {out("o1")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  r.factory.input("a")->paused = true;
  std::set<std::string> phases;
  for (int i = 0; i < 75; ++i) {  // 1.5 s
    phases.insert(phaseOf(r.state, "a"));
    std::this_thread::sleep_for(20ms);
  }
  CHECK(phases.count("holding") == 1);
  CHECK(phaseOf(r.state, "a") == "slate");
  auto seen = r.factory.output("o1")->seen();
  CHECK(maxGap(seen) < 50ms);
  CHECK(std::abs(seen.back().firstPixel[0] - 79) <= 4);  // blue: 0.06 linear
  CHECK(std::abs(seen.back().firstPixel[2] - 73) <= 4);  // red: 0.05 linear
  r.factory.input("a")->paused = false;
  std::this_thread::sleep_for(500ms);
  CHECK(phaseOf(r.state, "a") == "live");
}

TEST_CASE("Engine: a slow output does not slow its neighbour") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {out("slow", {{"delayMs", 200}}), out("fast")}));
  r.engine.apply(show);
  r.engine.start();
  auto t0 = Clock::now();
  std::this_thread::sleep_for(1600ms);
  CHECK(countBetween(r.factory.output("fast")->seen(), t0 + 500ms, t0 + 1500ms) >= 55);
  CHECK(r.factory.output("slow")->status().dropped > 0);
}

TEST_CASE("Engine: apply adds, reconfigures and removes cameras") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {out("o1")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  auto* o1 = r.factory.output("o1");
  show.cameras[0].outputs = {out("o2")};
  show.cameras.push_back(cam("b", {out("o3")}));
  r.engine.apply(show);
  std::this_thread::sleep_for(300ms);
  CHECK(r.factory.output("o2")->delivered > 0);
  CHECK(r.factory.output("o3")->delivered > 0);
  auto snap = r.state.snapshot();
  CHECK_FALSE(snap["cameras"]["a"]["outputs"].contains("o1"));
  CHECK(snap["cameras"]["b"]["name"] == "Cam b");
  (void)o1;  // destroyed by the reconfigure; must not be touched
  r.engine.apply(bc::Show{});
  CHECK(r.state.snapshot()["cameras"].empty());
}

TEST_CASE("Engine: a resolution change mid-stream follows the new size") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {out("o1")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  r.factory.input("a")->width = 32;
  r.factory.input("a")->height = 18;
  std::this_thread::sleep_for(500ms);
  auto seen = r.factory.output("o1")->seen();
  CHECK(seen.front().width == 64);
  CHECK(seen.back().width == 32);
  CHECK(maxGap(seen) < 50ms);
}

TEST_CASE("Engine: a missing slate image falls back to the built-in slate with a warning") {
  Rig r;
  bc::Show show;
  show.settings.slatePath = "C:/definitely/not/here.png";
  show.cameras.push_back(cam("a", {out("o1")}));
  r.engine.apply(show);
  r.factory.input("a")->paused = true;  // never any signal
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  auto snap = r.state.snapshot();
  CHECK_FALSE(snap["engine"]["warnings"]["slate"].get<std::string>().empty());
  auto seen = r.factory.output("o1")->seen();
  REQUIRE_FALSE(seen.empty());
  CHECK(std::abs(seen.back().firstPixel[2] - 73) <= 4);
}

TEST_CASE("Engine: a bad output config is reported and the camera keeps running") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {out("bad", {}, "nope"), out("good")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1200ms);  // at least one 1 Hz status publish
  auto snap = r.state.snapshot();
  CHECK(snap["cameras"]["a"]["outputs"]["bad"]["state"] == "offline");
  CHECK(snap["cameras"]["a"]["outputs"]["bad"]["error"]["code"] == "output-config");
  CHECK(snap["cameras"]["a"]["outputs"]["good"]["state"] == "live");
  CHECK(r.factory.output("good")->delivered > 30);
}
```

Slate colour: linear 0.05 → 0.05^(1/2.4) × 255 ≈ 73 (red, green); linear 0.06 → ≈ 79 (blue).

Add `tests/engine_test.cpp` to `bc_tests` and, after `bc_io` in `CMakeLists.txt`:
```cmake
# ---- engine ----
add_library(bc_engine STATIC
  src/engine/adapter_factory.cpp
  src/engine/camera.cpp
  src/engine/engine.cpp
  # Task 10
  # Task 11
)
target_include_directories(bc_engine PUBLIC src)
target_link_libraries(bc_engine PUBLIC bc_io winmm)
```
Link `bc_engine` into `bc_tests`.

- [ ] **Step 3: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Engine*"`
Expected: build FAILS — `engine/engine.h` not found.

- [ ] **Step 4: Implement the adapter factory**

`src/engine/adapter_factory.h`:
```cpp
#pragma once
#include <memory>

#include "core/show.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"
#include "io/output.h"

namespace bc {
// Builds adapters from show config. Both methods throw std::invalid_argument with a
// user-readable message for unknown kinds or bad params.
class AdapterFactory {
 public:
  virtual ~AdapterFactory() = default;
  virtual std::unique_ptr<Input> makeInput(const InputConfig& cfg) = 0;
  virtual std::unique_ptr<Output> makeOutput(const OutputConfig& cfg, const CameraConfig& cam) = 0;
};

class DefaultAdapterFactory final : public AdapterFactory {
 public:
  DefaultAdapterFactory(GpuDevice& gpu, TexturePool& pool) : gpu_(gpu), pool_(pool) {}
  std::unique_ptr<Input> makeInput(const InputConfig& cfg) override;
  std::unique_ptr<Output> makeOutput(const OutputConfig& cfg, const CameraConfig& cam) override;

 private:
  GpuDevice& gpu_;
  TexturePool& pool_;
};
}  // namespace bc
```

`src/engine/adapter_factory.cpp`:
```cpp
#include "engine/adapter_factory.h"

#include <stdexcept>

#include "io/ffmpeg_input.h"
#include "io/fullscreen_output.h"
#include "io/srt_params.h"
#if BC_WITH_NDI
#include "io/ndi_output.h"
#endif

namespace bc {
std::unique_ptr<Input> DefaultAdapterFactory::makeInput(const InputConfig& cfg) {
  if (cfg.kind == "srt") return std::make_unique<FfmpegInput>(gpu_, pool_, srtOptionsFromParams(cfg.params));
  throw std::invalid_argument("unknown input kind '" + cfg.kind + "'");
}

std::unique_ptr<Output> DefaultAdapterFactory::makeOutput(const OutputConfig& cfg, const CameraConfig& cam) {
  if (cfg.kind == "ndi") {
#if BC_WITH_NDI
    std::string name = cfg.params.value("name", std::string("BreadCamera - ") + cam.name);
    if (name.empty() || utf8Length(name) > 200) throw std::invalid_argument("NDI name must be 1 to 200 characters");
    return std::make_unique<NdiOutput>(name);
#else
    (void)cam;
    throw std::invalid_argument("NDI support is not installed in this build");
#endif
  }
  if (cfg.kind == "fullscreen") {
    if (cfg.params.contains("monitor") && !cfg.params["monitor"].is_number_integer()) {
      throw std::invalid_argument("monitor must be a whole number");
    }
    return std::make_unique<FullscreenOutput>(gpu_, cfg.params.value("monitor", 0));
  }
  throw std::invalid_argument("unknown output kind '" + cfg.kind + "'");
}
}  // namespace bc
```

- [ ] **Step 5: Implement `Camera`**

`src/engine/camera.h`:
```cpp
#pragma once
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/audio_delay.h"
#include "core/frame_sync.h"
#include "core/show.h"
#include "core/state_store.h"
#include "core/tick_schedule.h"
#include "engine/adapter_factory.h"
#include "gpu/converter.h"

namespace bc {
struct CameraStats {
  double fps = 0;
  uint64_t drops = 0;
  uint64_t repeats = 0;
  double latencyMs = 0;
  FeedPhase phase = FeedPhase::NoSignal;
};

// One camera: input → frame sync → convert → fan-out (spec §3.2). tick() runs on the
// engine's tick thread; the InputSink callbacks run on the input thread.
class Camera final : public InputSink {
 public:
  Camera(CameraConfig cfg, AdapterFactory& factory, Converter& conv, StateStore& state, SyncPolicy policy);
  ~Camera() override;
  const CameraConfig& config() const { return cfg_; }
  void start();
  void stop();
  void reconfigure(const CameraConfig& cfg);
  void setPolicy(SyncPolicy p) { sync_.setPolicy(p); }
  void tick(uint64_t index, const TickSchedule& sched, std::chrono::steady_clock::time_point now,
            const TextureRef& slate);
  TextureRef lastFrame() const { return lastFrame_; }
  void publishStatus();
  CameraStats stats() const;

  void onVideo(VideoFrameIn frame) override;
  void onAudio(AudioBlock audio) override;
  void onState(SignalState state, std::optional<ErrorInfo> error) override;

 private:
  struct Pending {
    VideoFrameIn frame;
    std::chrono::steady_clock::time_point arrival;
  };
  struct OutputSlot {
    OutputConfig cfg;
    std::unique_ptr<Output> out;
    std::optional<ErrorInfo> createError;
  };
  void startInput();
  void startOutput(OutputSlot& slot);
  std::string path(const std::string& rest) const { return "/cameras/" + cfg_.id + rest; }

  CameraConfig cfg_;
  AdapterFactory& factory_;
  Converter& conv_;
  StateStore& state_;
  FrameSync<Pending> sync_;
  AudioDelay audio_{48000, 2};
  std::unique_ptr<Input> input_;
  std::vector<OutputSlot> outputs_;
  TextureRef lastConverted_;
  TextureRef lastFrame_;
  std::chrono::milliseconds audioDelaySet_{-1};
  mutable std::mutex statsMutex_;
  double latencyEwmaMs_ = 0;
  uint64_t freshThisSecond_ = 0;
  double fps_ = 0;
  FeedPhase phase_ = FeedPhase::NoSignal;
};
}  // namespace bc
```

`src/engine/camera.cpp`:
```cpp
#include "engine/camera.h"

#include <cmath>
#include <map>

namespace bc {
using Clock = std::chrono::steady_clock;

Camera::Camera(CameraConfig cfg, AdapterFactory& factory, Converter& conv, StateStore& state, SyncPolicy policy)
    : cfg_(std::move(cfg)), factory_(factory), conv_(conv), state_(state), sync_(policy) {}

Camera::~Camera() { stop(); }

void Camera::startInput() {
  state_.set(path("/input/state"), "connecting");
  state_.set(path("/input/error"), nullptr);
  try {
    input_ = factory_.makeInput(cfg_.input);
    input_->start(*this);
  } catch (const std::exception& e) {
    input_.reset();
    state_.set(path("/input/state"), "offline");
    state_.set(path("/input/error"), errorToJson(ErrorInfo{"input-config", e.what()}));
  }
}

void Camera::startOutput(OutputSlot& s) {
  s.out.reset();
  s.createError.reset();
  if (!s.cfg.enabled) return;
  try {
    s.out = factory_.makeOutput(s.cfg, cfg_);
    s.out->start();
  } catch (const std::exception& e) {
    s.out.reset();
    s.createError = ErrorInfo{"output-config", e.what()};
  }
}

void Camera::start() {
  state_.set(path("/name"), cfg_.name);
  state_.set(path("/phase"), toString(FeedPhase::NoSignal));
  startInput();
  for (auto& o : cfg_.outputs) {
    outputs_.push_back({o, nullptr, std::nullopt});
    startOutput(outputs_.back());
  }
  publishStatus();
}

void Camera::stop() {
  if (input_) {
    input_->stop();
    input_.reset();
  }
  for (auto& s : outputs_) {
    if (s.out) s.out->stop();
  }
  outputs_.clear();
}

void Camera::reconfigure(const CameraConfig& next) {
  if (next.name != cfg_.name) state_.set(path("/name"), next.name);
  const bool inputChanged = !(next.input == cfg_.input);
  std::map<std::string, OutputSlot> old;
  for (auto& s : outputs_) old.emplace(s.cfg.id, std::move(s));
  outputs_.clear();
  cfg_ = next;
  if (inputChanged) {
    if (input_) input_->stop();
    input_.reset();
    startInput();  // the last frame stays in sync_, so outputs hold, then slate
  }
  for (auto& o : cfg_.outputs) {
    auto it = old.find(o.id);
    if (it != old.end() && it->second.cfg == o) {
      outputs_.push_back(std::move(it->second));
      old.erase(it);
      continue;
    }
    if (it != old.end()) {
      if (it->second.out) it->second.out->stop();
      old.erase(it);
    }
    outputs_.push_back({o, nullptr, std::nullopt});
    startOutput(outputs_.back());
  }
  for (auto& [id, s] : old) {
    if (s.out) s.out->stop();
    state_.erase(path("/outputs/" + id));
  }
  publishStatus();
}

void Camera::onVideo(VideoFrameIn frame) {
  const auto now = Clock::now();
  sync_.push(Pending{std::move(frame), now}, now);
}

void Camera::onAudio(AudioBlock a) {
  if (a.channels == 2 && a.sampleRate == 48000) audio_.push(a.samples);
}

void Camera::onState(SignalState s, std::optional<ErrorInfo> e) {
  state_.set(path("/input/state"), std::string(toString(s)));
  state_.set(path("/input/error"), errorToJson(e));
}

void Camera::tick(uint64_t index, const TickSchedule& sched, Clock::time_point now, const TextureRef& slate) {
  auto pick = sync_.pick(now);
  std::optional<Clock::time_point> arrival;
  if (pick.frame) {
    if (pick.fresh || !lastConverted_) {
      const auto& f = pick.frame->frame;
      lastConverted_ = conv_.toLinear(*f.tex, f.layout, f.fullRange);
      if (pick.fresh) arrival = pick.frame->arrival;
    }
  } else {
    lastConverted_.reset();
  }
  TextureRef frame = lastConverted_ ? lastConverted_ : slate;
  lastFrame_ = frame;

  auto audio = std::make_shared<std::vector<float>>(audio_.pull(sched.audioFramesForTick(index, 48000)));
  bool anyCpu = false;
  for (auto& s : outputs_) anyCpu = anyCpu || (s.out && s.out->needsCpu());
  std::shared_ptr<const CpuImage> cpu;
  if (anyCpu) cpu = std::make_shared<CpuImage>(conv_.readback(*conv_.toBgra8(*frame, frame->width, frame->height)));

  OutFrame out{index, sched.rate(), frame, cpu, audio, 48000, 2, false};
  for (auto& s : outputs_) {
    if (s.out) s.out->submit(out);
  }

  std::lock_guard l(statsMutex_);
  phase_ = pick.phase;
  if (arrival) {
    ++freshThisSecond_;
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - *arrival).count();
    latencyEwmaMs_ = latencyEwmaMs_ == 0 ? ms : latencyEwmaMs_ * 0.9 + ms * 0.1;
  }
}

void Camera::publishStatus() {
  double latency;
  FeedPhase phase;
  {
    std::lock_guard l(statsMutex_);
    fps_ = static_cast<double>(freshThisSecond_);
    freshThisSecond_ = 0;
    latency = latencyEwmaMs_;
    phase = phase_;
  }
  state_.set(path("/phase"), toString(phase));
  const auto target = std::chrono::milliseconds(static_cast<int64_t>(std::lround(latency / 5.0) * 5));
  if (audioDelaySet_.count() < 0 || std::abs((target - audioDelaySet_).count()) > 10) {
    audio_.setDelay(target);
    audioDelaySet_ = target;
  }
  for (auto& s : outputs_) {
    const std::string p = path("/outputs/" + s.cfg.id);
    state_.set(p + "/kind", s.cfg.kind);
    if (!s.cfg.enabled) {
      state_.set(p + "/state", "ready");
      state_.set(p + "/error", nullptr);
    } else if (!s.out) {
      state_.set(p + "/state", "offline");
      state_.set(p + "/error", errorToJson(s.createError));
    } else {
      auto st = s.out->status();
      state_.set(p + "/state", std::string(toString(st.state)));
      state_.set(p + "/error", errorToJson(st.error));
    }
  }
}

CameraStats Camera::stats() const {
  std::lock_guard l(statsMutex_);
  return {fps_, sync_.drops(), sync_.repeats(), latencyEwmaMs_, phase_};
}
}  // namespace bc
```

Note: `publishStatus` runs once per second **and** the phase in the tests must change within that second — the engine also writes `/phase` every tick when it changes (Step 6), so the hold/slate test sees "holding" even though it lasts < 1 s.

- [ ] **Step 6: Implement `Engine`**

`src/engine/engine.h`:
```cpp
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/show.h"
#include "core/state_store.h"
#include "engine/adapter_factory.h"
#include "engine/camera.h"
#include "gpu/converter.h"

namespace bc {
struct EngineStats {
  uint64_t overruns = 0;
  std::map<std::string, CameraStats> cameras;
};

// Master tick + camera set (spec §3.3). apply() is thread-safe; adapters are torn down
// outside the tick lock so a slow stop never stalls the tick.
class Engine {
 public:
  using FrameTap = std::function<void(uint64_t tick, const std::vector<std::pair<std::string, TextureRef>>& frames)>;
  Engine(Converter& conv, StateStore& state, AdapterFactory& factory);
  ~Engine();
  void apply(const Show& show);
  void start();
  void stop();
  EngineStats stats() const;
  void setFrameTap(FrameTap tap) { tap_ = std::move(tap); }

 private:
  void run();
  void loadSlate();
  SyncPolicy policy() const { return {std::chrono::milliseconds(250), std::chrono::milliseconds(settings_.holdMs)}; }

  Converter& conv_;
  StateStore& state_;
  AdapterFactory& factory_;
  mutable std::mutex mutex_;
  ShowSettings settings_;
  bool settingsApplied_ = false;
  bool rateChanged_ = true;
  TextureRef slate_;
  std::vector<std::unique_ptr<Camera>> cameras_;
  std::map<std::string, std::string> lastPhase_;
  FrameTap tap_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> overruns_{0};
  std::thread thread_;
};
}  // namespace bc
```

`src/engine/engine.cpp`:
```cpp
#include "engine/engine.h"

#include <windows.h>
#include <timeapi.h>

#include <cmath>

#include "core/tick_schedule.h"
#include "gpu/image_io.h"

namespace bc {
using Clock = std::chrono::steady_clock;

Engine::Engine(Converter& conv, StateStore& state, AdapterFactory& factory)
    : conv_(conv), state_(state), factory_(factory) {
  std::lock_guard l(mutex_);
  loadSlate();
  state_.set("/engine/rate", settings_.rate.str());
  state_.set("/engine/overruns", 0);
}

Engine::~Engine() {
  stop();
  std::vector<std::unique_ptr<Camera>> gone;
  {
    std::lock_guard l(mutex_);
    gone = std::move(cameras_);
  }
}

void Engine::loadSlate() {
  state_.set("/engine/warnings/slate", "");
  if (!settings_.slatePath.empty()) {
    try {
      auto img = loadImageBgra(settings_.slatePath);
      auto up = conv_.uploadBgra8(img.bgra.data(), img.width, img.height, img.width * 4);
      slate_ = conv_.toLinear(*up, PixelLayout::BGRA8, true);
      return;
    } catch (const std::exception& e) {
      state_.set("/engine/warnings/slate", std::string("slate image could not be loaded: ") + e.what() +
                                               "; using the built-in slate");
    }
  }
  slate_ = conv_.solid(1920, 1080, {0.05f, 0.05f, 0.06f, 1.0f});
}

void Engine::apply(const Show& show) {
  std::vector<std::unique_ptr<Camera>> removed;
  {
    std::lock_guard l(mutex_);
    if (!settingsApplied_ || !(show.settings == settings_)) {
      const bool slateChanged = !settingsApplied_ || show.settings.slatePath != settings_.slatePath;
      rateChanged_ = rateChanged_ || !(show.settings.rate == settings_.rate);
      settings_ = show.settings;
      settingsApplied_ = true;
      if (slateChanged) loadSlate();
      state_.set("/engine/rate", settings_.rate.str());
      for (auto& c : cameras_) c->setPolicy(policy());
    }
    std::map<std::string, std::unique_ptr<Camera>> existing;
    for (auto& c : cameras_) existing.emplace(c->config().id, std::move(c));
    cameras_.clear();
    for (auto& cfg : show.cameras) {
      auto it = existing.find(cfg.id);
      if (it != existing.end()) {
        if (!(it->second->config() == cfg)) it->second->reconfigure(cfg);
        cameras_.push_back(std::move(it->second));
        existing.erase(it);
      } else {
        auto cam = std::make_unique<Camera>(cfg, factory_, conv_, state_, policy());
        cam->start();
        cameras_.push_back(std::move(cam));
      }
    }
    for (auto& [id, cam] : existing) {
      state_.erase("/cameras/" + id);
      lastPhase_.erase(id);
      removed.push_back(std::move(cam));
    }
  }
  removed.clear();  // joins adapter threads outside the lock
}

void Engine::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void Engine::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

EngineStats Engine::stats() const {
  std::lock_guard l(mutex_);
  EngineStats s;
  s.overruns = overruns_;
  for (auto& c : cameras_) s.cameras[c->config().id] = c->stats();
  return s;
}

void Engine::run() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  timeBeginPeriod(1);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

  TickSchedule sched(settings_.rate);
  Clock::time_point start = Clock::now();
  uint64_t index = 0;
  uint64_t ticksPerSecond = 60;
  while (running_) {
    {
      std::lock_guard l(mutex_);
      if (rateChanged_) {
        sched = TickSchedule(settings_.rate);
        ticksPerSecond = static_cast<uint64_t>(std::lround(settings_.rate.fps()));
        start = Clock::now();
        index = 0;
        rateChanged_ = false;
      }
    }
    const auto deadline = start + sched.offset(index);
    const auto remaining = deadline - Clock::now();
    if (remaining.count() > 0) {
      LARGE_INTEGER due;
      due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, INFINITE);
    }
    const auto now = Clock::now();
    std::vector<std::pair<std::string, TextureRef>> frames;
    {
      std::lock_guard l(mutex_);
      for (auto& cam : cameras_) {
        const auto& id = cam->config().id;
        try {
          cam->tick(index, sched, now, slate_);
        } catch (const std::exception& e) {
          state_.set("/cameras/" + id + "/tickError", e.what());
        }
        const std::string phase = toString(cam->stats().phase);
        auto& last = lastPhase_[id];
        if (last != phase) {
          last = phase;
          state_.set("/cameras/" + id + "/phase", phase);
        }
        frames.emplace_back(id, cam->lastFrame());
      }
      if (index % ticksPerSecond == 0) {
        for (auto& cam : cameras_) cam->publishStatus();
        state_.set("/engine/overruns", overruns_.load());
      }
    }
    if (tap_) tap_(index, frames);
    const auto elapsed = Clock::now() - start;
    uint64_t next = index + 1;
    if (sched.offset(next) < elapsed) {
      const uint64_t skipTo = sched.nextIndexAtOrAfter(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
      overruns_ += skipTo - next;
      next = skipTo;
    }
    index = next;
  }
  CloseHandle(timer);
  timeEndPeriod(1);
}
}  // namespace bc
```

Note: `Camera::stats()` reads `phase_`, which `tick()` set just before, so the per-tick phase write is current.

- [ ] **Step 7: Run the tests to verify they pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Engine*"`
Expected: 8 cases pass on the RX 6600. Then run them once with `BC_FORCE_WARP=1` and ledger the result; if a timing assertion fails **only** under WARP, ledger it as an environment finding (do not loosen the bounds).

- [ ] **Step 8: Commit**

```bash
git add src/engine tests CMakeLists.txt
git commit -m "feat(engine): master tick, cameras with hold/slate, audio delay and output fan-out"
```

### Task 10: Preview encoder, preview subscriptions and the preview service

**Files:**
- Create: `src/engine/preview_encoder.h`, `src/engine/preview_encoder.cpp`, `src/engine/preview_hub.h`, `src/engine/preview_hub.cpp`, `src/engine/preview_service.h`, `src/engine/preview_service.cpp`
- Modify: `CMakeLists.txt` (`# Task 10` sources in `bc_engine`; tests), `tests/support/requires.cpp` (real `H264Encoder` check)
- Test: `tests/preview_test.cpp`

**Interfaces:**
- Consumes: `Converter`, `CpuImage`, `TextureRef` (Task 5); `StateStore` (Task 4); `Engine::FrameTap` frame list shape (Task 9).
- Produces:
  - `bc::PreviewPacket { std::vector<uint8_t> annexB; bool keyframe; int64_t ptsUs; }`.
  - `bc::PreviewEncoder(int width, int height, int fps)` (throws `std::runtime_error` listing what was tried), `static const std::vector<std::string>& encoderOrder()` = `{"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf"}` (spec §7.2), `const std::string& encoderName() const`, `void forceKeyframe()`, `std::vector<PreviewPacket> encode(const CpuImage&, int64_t ptsUs)`.
  - `std::vector<uint8_t> bc::avccToAnnexB(std::span<const uint8_t>)` (throws `std::invalid_argument`), `bool bc::hasNalType(std::span<const uint8_t> annexB, int type)`, `std::vector<uint8_t> bc::encodePreviewMessage(std::string_view cameraId, const PreviewPacket&)`.
  - `enum class bc::PreviewSize { Small, Large }`, `bc::PreviewDims bc::dims(PreviewSize)` → 512×288 / 1280×720, `std::optional<PreviewSize> bc::parsePreviewSize(std::string_view)` (`"small"`/`"large"`).
  - `bc::PreviewHub`: `std::vector<std::pair<std::string, PreviewSize>> subscribe(const std::string& client, std::vector<std::string> cameraIds, PreviewSize)` (returns the pairs this client newly wants), `void dropClient(const std::string&)`, `std::set<std::pair<std::string, PreviewSize>> wanted() const`, `std::vector<std::string> clientsFor(const std::string& cameraId, PreviewSize) const`.
  - `bc::PreviewService(Converter&, StateStore&, PreviewHub&, SendFn)` with `using SendFn = std::function<void(const std::string& client, std::vector<uint8_t> message)>`: `void onTick(uint64_t tick, const std::vector<std::pair<std::string, TextureRef>>& frames)`, `void requestKeyframe(const std::string& cameraId, PreviewSize)`.

Preview binary message (spec §6.3), little-endian:
```
byte 0      version = 1
byte 1      flags: bit0 = keyframe
bytes 2-3   uint16 camera id length N
bytes 4..   camera id (UTF-8, N bytes)
next 8      int64 ptsUs
rest        H.264 Annex-B access unit (SPS/PPS in front of every keyframe)
```
Sizes are 512×288 and 1280×720 (16:9, multiples of 16, which every hardware encoder accepts; spec §6.3's "about 480×270" — ledger this). Previews run at half the show rate (every second tick).

- [ ] **Step 1: Write the failing tests**

`tests/preview_test.cpp`:
```cpp
#include <doctest/doctest.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "core/state_store.h"
#include "engine/preview_encoder.h"
#include "engine/preview_hub.h"
#include "engine/preview_service.h"
#include "gpu/device.h"
#include "support/requires.h"

using bc::PreviewSize;

TEST_CASE("avccToAnnexB: converts SPS/PPS and rejects truncated input") {
  const std::vector<uint8_t> avcc{1, 0x42, 0xC0, 0x1E, 0xFF, 0xE1, 0x00, 0x04, 0x67, 0x42, 0xC0, 0x1E,
                                  0x01, 0x00, 0x02, 0x68, 0xCE};
  const std::vector<uint8_t> want{0, 0, 0, 1, 0x67, 0x42, 0xC0, 0x1E, 0, 0, 0, 1, 0x68, 0xCE};
  CHECK(bc::avccToAnnexB(avcc) == want);
  CHECK_THROWS_AS(bc::avccToAnnexB(std::vector<uint8_t>(avcc.begin(), avcc.begin() + 9)), std::invalid_argument);
  CHECK(bc::hasNalType(want, 7));
  CHECK(bc::hasNalType(want, 8));
  CHECK_FALSE(bc::hasNalType(want, 5));
}

TEST_CASE("encodePreviewMessage: header layout") {
  bc::PreviewPacket p{{9, 9}, true, 1234};
  auto m = bc::encodePreviewMessage("cam1", p);
  const std::vector<uint8_t> want{1, 1, 4, 0, 'c', 'a', 'm', '1', 0xD2, 0x04, 0, 0, 0, 0, 0, 0, 9, 9};
  CHECK(m == want);
}

TEST_CASE("PreviewHub: union of subscriptions, new pairs reported, clients dropped") {
  bc::PreviewHub hub;
  auto fresh = hub.subscribe("c1", {"a", "b"}, PreviewSize::Small);
  CHECK(fresh.size() == 2);
  CHECK(hub.subscribe("c1", {"a"}, PreviewSize::Small).empty());  // narrowed, nothing new
  CHECK(hub.subscribe("c2", {"a"}, PreviewSize::Large).size() == 1);
  CHECK(hub.wanted() == std::set<std::pair<std::string, PreviewSize>>{{"a", PreviewSize::Small}, {"a", PreviewSize::Large}});
  CHECK(hub.clientsFor("a", PreviewSize::Small) == std::vector<std::string>{"c1"});
  hub.dropClient("c1");
  CHECK(hub.clientsFor("a", PreviewSize::Small).empty());
  CHECK(bc::parsePreviewSize("large") == PreviewSize::Large);
  CHECK_FALSE(bc::parsePreviewSize("huge"));
}

TEST_CASE("PreviewEncoder: decodable H.264 with in-band headers" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  bc::PreviewEncoder enc(512, 288, 30);
  MESSAGE("encoder: " << enc.encoderName());
  bc::CpuImage img{512, 288, std::vector<uint8_t>(512 * 288 * 4, 128)};
  std::vector<bc::PreviewPacket> packets;
  for (int i = 0; i < 60; ++i) {
    for (size_t p = 2; p < img.bgra.size(); p += 4) img.bgra[p] = static_cast<uint8_t>(i * 4);
    if (i == 30) enc.forceKeyframe();
    auto out = enc.encode(img, i * 33'333);
    packets.insert(packets.end(), out.begin(), out.end());
  }
  REQUIRE_FALSE(packets.empty());
  CHECK(packets.front().keyframe);
  CHECK(bc::hasNalType(packets.front().annexB, 7));
  int keys = 0;
  for (auto& p : packets) keys += p.keyframe ? 1 : 0;
  CHECK(keys >= 2);

  const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
  AVCodecContext* ctx = avcodec_alloc_context3(dec);
  REQUIRE(avcodec_open2(ctx, dec, nullptr) == 0);
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  int decoded = 0;
  for (auto& p : packets) {
    pkt->data = p.annexB.data();
    pkt->size = static_cast<int>(p.annexB.size());
    if (avcodec_send_packet(ctx, pkt) >= 0) {
      while (avcodec_receive_frame(ctx, frame) >= 0) ++decoded;
    }
  }
  avcodec_send_packet(ctx, nullptr);
  while (avcodec_receive_frame(ctx, frame) >= 0) ++decoded;
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
  CHECK(decoded >= 50);
}

TEST_CASE("PreviewService: subscribers get a keyframe first; others get nothing" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::StateStore state;
  bc::PreviewHub hub;
  std::map<std::string, std::vector<std::vector<uint8_t>>> sent;
  bc::PreviewService svc(conv, state, hub, [&](const std::string& c, std::vector<uint8_t> m) { sent[c].push_back(std::move(m)); });
  hub.subscribe("c1", {"a"}, PreviewSize::Small);
  auto frame = conv.solid(1920, 1080, {0.2f, 0.2f, 0.2f, 1});
  for (uint64_t t = 0; t < 40; ++t) svc.onTick(t, {{"a", frame}, {"b", frame}});
  REQUIRE(sent["c1"].size() >= 10);
  CHECK((sent["c1"].front()[1] & 1) == 1);
  CHECK(sent.count("c2") == 0);
}
```

Add it to `bc_tests`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "*Preview*,avcc*,encodePreview*"`
Expected: build FAILS — `engine/preview_encoder.h` not found.

- [ ] **Step 3: Implement the encoder and helpers**

`src/engine/preview_encoder.h`:
```cpp
#pragma once
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gpu/converter.h"

struct AVCodecContext;
struct AVFrame;
struct SwsContext;

namespace bc {
struct PreviewPacket {
  std::vector<uint8_t> annexB;
  bool keyframe = false;
  int64_t ptsUs = 0;
};

std::vector<uint8_t> avccToAnnexB(std::span<const uint8_t> avcc);
bool hasNalType(std::span<const uint8_t> annexB, int type);
std::vector<uint8_t> encodePreviewMessage(std::string_view cameraId, const PreviewPacket& p);

// Low-latency H.264 for UI previews. Uses the first encoder that opens (spec §7.2).
class PreviewEncoder {
 public:
  PreviewEncoder(int width, int height, int fps);
  ~PreviewEncoder();
  PreviewEncoder(const PreviewEncoder&) = delete;
  PreviewEncoder& operator=(const PreviewEncoder&) = delete;
  static const std::vector<std::string>& encoderOrder();
  const std::string& encoderName() const { return name_; }
  void forceKeyframe() { forceKey_ = true; }
  std::vector<PreviewPacket> encode(const CpuImage& bgra, int64_t ptsUs);

 private:
  int w_, h_;
  std::string name_;
  AVCodecContext* ctx_ = nullptr;
  AVFrame* frame_ = nullptr;
  SwsContext* sws_ = nullptr;
  std::vector<uint8_t> headers_;
  int64_t index_ = 0;
  std::atomic<bool> forceKey_{false};
};
}  // namespace bc
```

`src/engine/preview_encoder.cpp`:
```cpp
#include "engine/preview_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <cstring>
#include <stdexcept>

namespace bc {
namespace {
std::string averr(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(rc, buf, sizeof(buf));
  return buf;
}
bool isAnnexB(std::span<const uint8_t> d) {
  return d.size() >= 4 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || (d[2] == 0 && d[3] == 1));
}
}  // namespace

std::vector<uint8_t> avccToAnnexB(std::span<const uint8_t> a) {
  auto need = [&](size_t pos, size_t n) {
    if (pos + n > a.size()) throw std::invalid_argument("truncated avcC");
  };
  need(0, 6);
  if (a[0] != 1) throw std::invalid_argument("not avcC");
  std::vector<uint8_t> out;
  size_t pos = 5;
  auto copySets = [&](int count) {
    for (int i = 0; i < count; ++i) {
      need(pos, 2);
      const size_t len = (static_cast<size_t>(a[pos]) << 8) | a[pos + 1];
      pos += 2;
      need(pos, len);
      out.insert(out.end(), {0, 0, 0, 1});
      out.insert(out.end(), a.begin() + pos, a.begin() + pos + len);
      pos += len;
    }
  };
  const int sps = a[pos++] & 0x1F;
  copySets(sps);
  need(pos, 1);
  const int pps = a[pos++];
  copySets(pps);
  return out;
}

bool hasNalType(std::span<const uint8_t> d, int type) {
  for (size_t i = 0; i + 3 < d.size(); ++i) {
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1F) == type) return true;
  }
  return false;
}

std::vector<uint8_t> encodePreviewMessage(std::string_view id, const PreviewPacket& p) {
  std::vector<uint8_t> m;
  m.reserve(4 + id.size() + 8 + p.annexB.size());
  m.push_back(1);
  m.push_back(p.keyframe ? 1 : 0);
  m.push_back(static_cast<uint8_t>(id.size() & 0xFF));
  m.push_back(static_cast<uint8_t>((id.size() >> 8) & 0xFF));
  m.insert(m.end(), id.begin(), id.end());
  for (int i = 0; i < 8; ++i) m.push_back(static_cast<uint8_t>((static_cast<uint64_t>(p.ptsUs) >> (8 * i)) & 0xFF));
  m.insert(m.end(), p.annexB.begin(), p.annexB.end());
  return m;
}

const std::vector<std::string>& PreviewEncoder::encoderOrder() {
  static const std::vector<std::string> order{"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf"};
  return order;
}

PreviewEncoder::PreviewEncoder(int w, int h, int fps) : w_(w), h_(h) {
  std::string tried;
  for (const auto& name : encoderOrder()) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) {
      tried += name + " (not built); ";
      continue;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = w;
    c->height = h;
    c->time_base = {1, fps};
    c->framerate = {fps, 1};
    c->pix_fmt = AV_PIX_FMT_NV12;
    c->gop_size = fps;
    c->max_b_frames = 0;
    c->bit_rate = w >= 1280 ? 4'000'000 : 1'000'000;
    c->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* o = nullptr;
    if (name == "h264_nvenc") {
      av_dict_set(&o, "preset", "p1", 0);
      av_dict_set(&o, "tune", "ull", 0);
      av_dict_set(&o, "zerolatency", "1", 0);
      av_dict_set(&o, "forced-idr", "1", 0);
    } else if (name == "h264_amf") {
      av_dict_set(&o, "usage", "ultralowlatency", 0);
      av_dict_set(&o, "forced_idr", "1", 0);
    } else if (name == "h264_qsv") {
      av_dict_set(&o, "preset", "veryfast", 0);
      av_dict_set(&o, "async_depth", "1", 0);
    } else if (name == "h264_mf") {
      av_dict_set(&o, "rate_control", "cbr", 0);
      av_dict_set(&o, "scenario", "display_remoting", 0);
    }
    const int rc = avcodec_open2(c, codec, &o);
    av_dict_free(&o);
    if (rc < 0) {
      tried += name + " (" + averr(rc) + "); ";
      avcodec_free_context(&c);
      continue;
    }
    ctx_ = c;
    name_ = name;
    break;
  }
  if (!ctx_) throw std::runtime_error("no H.264 encoder could be opened: " + tried);
  if (ctx_->extradata_size > 0) {
    std::span<const uint8_t> ex(ctx_->extradata, static_cast<size_t>(ctx_->extradata_size));
    headers_ = isAnnexB(ex) ? std::vector<uint8_t>(ex.begin(), ex.end()) : avccToAnnexB(ex);
  }
  sws_ = sws_getContext(w, h, AV_PIX_FMT_BGRA, w, h, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
  frame_ = av_frame_alloc();
  frame_->format = AV_PIX_FMT_NV12;
  frame_->width = w;
  frame_->height = h;
  if (!sws_ || av_frame_get_buffer(frame_, 0) < 0) throw std::runtime_error("cannot allocate the preview frame");
  forceKey_ = true;
}

PreviewEncoder::~PreviewEncoder() {
  sws_freeContext(sws_);
  av_frame_free(&frame_);
  avcodec_free_context(&ctx_);
}

std::vector<PreviewPacket> PreviewEncoder::encode(const CpuImage& img, int64_t ptsUs) {
  if (static_cast<int>(img.width) != w_ || static_cast<int>(img.height) != h_) {
    throw std::invalid_argument("preview image has the wrong size");
  }
  av_frame_make_writable(frame_);
  const uint8_t* src[1] = {img.bgra.data()};
  const int srcStride[1] = {w_ * 4};
  sws_scale(sws_, src, srcStride, 0, h_, frame_->data, frame_->linesize);
  frame_->pts = index_++;
  frame_->pict_type = forceKey_.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
  std::vector<PreviewPacket> out;
  if (avcodec_send_frame(ctx_, frame_) < 0) return out;
  AVPacket* pkt = av_packet_alloc();
  while (avcodec_receive_packet(ctx_, pkt) >= 0) {
    PreviewPacket p;
    p.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    p.ptsUs = ptsUs;
    std::span<const uint8_t> data(pkt->data, static_cast<size_t>(pkt->size));
    if (p.keyframe && !hasNalType(data, 7)) p.annexB = headers_;
    p.annexB.insert(p.annexB.end(), data.begin(), data.end());
    out.push_back(std::move(p));
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  return out;
}
}  // namespace bc
```

- [ ] **Step 4: Implement the hub and the service**

`src/engine/preview_hub.h`:
```cpp
#pragma once
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bc {
enum class PreviewSize { Small, Large };
struct PreviewDims {
  int w, h;
};
inline PreviewDims dims(PreviewSize s) { return s == PreviewSize::Large ? PreviewDims{1280, 720} : PreviewDims{512, 288}; }
std::optional<PreviewSize> parsePreviewSize(std::string_view s);

// Which UI client watches which camera preview (spec §6.3: only visible tiles stream).
class PreviewHub {
 public:
  using Key = std::pair<std::string, PreviewSize>;
  std::vector<Key> subscribe(const std::string& client, std::vector<std::string> cameraIds, PreviewSize size);
  void dropClient(const std::string& client);
  std::set<Key> wanted() const;
  std::vector<std::string> clientsFor(const std::string& cameraId, PreviewSize size) const;

 private:
  mutable std::mutex m_;
  std::map<std::string, std::set<Key>> byClient_;
};
}  // namespace bc
```

`src/engine/preview_hub.cpp`:
```cpp
#include "engine/preview_hub.h"

namespace bc {
std::optional<PreviewSize> parsePreviewSize(std::string_view s) {
  if (s == "small") return PreviewSize::Small;
  if (s == "large") return PreviewSize::Large;
  return std::nullopt;
}

std::vector<PreviewHub::Key> PreviewHub::subscribe(const std::string& client, std::vector<std::string> ids, PreviewSize size) {
  std::lock_guard l(m_);
  std::set<Key> next;
  for (auto& id : ids) next.insert({id, size});
  std::vector<Key> fresh;
  auto& cur = byClient_[client];
  for (auto& k : next) {
    if (!cur.contains(k)) fresh.push_back(k);
  }
  cur = std::move(next);
  if (cur.empty()) byClient_.erase(client);
  return fresh;
}

void PreviewHub::dropClient(const std::string& client) {
  std::lock_guard l(m_);
  byClient_.erase(client);
}

std::set<PreviewHub::Key> PreviewHub::wanted() const {
  std::lock_guard l(m_);
  std::set<Key> all;
  for (auto& [c, keys] : byClient_) all.insert(keys.begin(), keys.end());
  return all;
}

std::vector<std::string> PreviewHub::clientsFor(const std::string& id, PreviewSize size) const {
  std::lock_guard l(m_);
  std::vector<std::string> out;
  for (auto& [c, keys] : byClient_) {
    if (keys.contains({id, size})) out.push_back(c);
  }
  return out;
}
}  // namespace bc
```

`src/engine/preview_service.h`:
```cpp
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/state_store.h"
#include "engine/preview_encoder.h"
#include "engine/preview_hub.h"
#include "gpu/converter.h"

namespace bc {
// Runs on the tick thread via Engine::setFrameTap. Encodes only what someone watches.
// ponytail: encodes on the tick thread (~1-2 ms per small preview); move to a worker if
// the tick budget gets tight with many large previews.
class PreviewService {
 public:
  using SendFn = std::function<void(const std::string& client, std::vector<uint8_t> message)>;
  PreviewService(Converter& conv, StateStore& state, PreviewHub& hub, SendFn send);
  void onTick(uint64_t tick, const std::vector<std::pair<std::string, TextureRef>>& frames);
  void requestKeyframe(const std::string& cameraId, PreviewSize size);

 private:
  Converter& conv_;
  StateStore& state_;
  PreviewHub& hub_;
  SendFn send_;
  bool broken_ = false;
  std::map<PreviewHub::Key, std::unique_ptr<PreviewEncoder>> encoders_;
  std::mutex keyMutex_;
  std::set<PreviewHub::Key> keyRequests_;
};
}  // namespace bc
```

`src/engine/preview_service.cpp`:
```cpp
#include "engine/preview_service.h"

#include <chrono>

namespace bc {
PreviewService::PreviewService(Converter& conv, StateStore& state, PreviewHub& hub, SendFn send)
    : conv_(conv), state_(state), hub_(hub), send_(std::move(send)) {}

void PreviewService::requestKeyframe(const std::string& id, PreviewSize size) {
  std::lock_guard l(keyMutex_);
  keyRequests_.insert({id, size});
}

void PreviewService::onTick(uint64_t tick, const std::vector<std::pair<std::string, TextureRef>>& frames) {
  if (broken_ || tick % 2 != 0) return;
  const auto wanted = hub_.wanted();
  for (auto it = encoders_.begin(); it != encoders_.end();) {
    it = wanted.contains(it->first) ? std::next(it) : encoders_.erase(it);
  }
  std::set<PreviewHub::Key> keys;
  {
    std::lock_guard l(keyMutex_);
    keys.swap(keyRequests_);
  }
  const int64_t ptsUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
  for (const auto& key : wanted) {
    const auto& [id, size] = key;
    TextureRef frame;
    for (auto& [fid, tex] : frames) {
      if (fid == id) frame = tex;
    }
    if (!frame) continue;
    auto& enc = encoders_[key];
    if (!enc) {
      try {
        const auto d = dims(size);
        enc = std::make_unique<PreviewEncoder>(d.w, d.h, 30);
        state_.set("/engine/previewEncoder", enc->encoderName());
      } catch (const std::exception& e) {
        encoders_.erase(key);
        broken_ = true;
        state_.set("/engine/warnings/preview", std::string("previews are unavailable: ") + e.what());
        return;
      }
    }
    if (keys.contains(key)) enc->forceKeyframe();
    const auto d = dims(size);
    const auto img = conv_.readback(*conv_.toBgra8(*frame, static_cast<uint32_t>(d.w), static_cast<uint32_t>(d.h)));
    for (auto& pkt : enc->encode(img, ptsUs)) {
      const auto msg = encodePreviewMessage(id, pkt);
      for (auto& client : hub_.clientsFor(id, size)) send_(client, msg);
    }
  }
}
}  // namespace bc
```

- [ ] **Step 5: Make `BC_REQUIRES(H264Encoder)` real**

In `tests/support/requires.cpp` add `#include "engine/preview_encoder.h"` and replace the case:
```cpp
    case Req::H264Encoder: {
      reason = "no H.264 encoder opens on this machine (h264_nvenc/amf/qsv/mf)";
      try {
        bc::PreviewEncoder probe(512, 288, 30);
        return false;
      } catch (...) {
        return true;
      }
    }
```

- [ ] **Step 6: Run the tests to verify they pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "*Preview*,avcc*,encodePreview*"`
Expected: 5 cases pass on the RX 6600 (the log names `h264_amf`). If `H264Encoder` is skipped here, ledger the encoder errors printed by the probe — that is a finding for the planner, not a pass.

- [ ] **Step 7: Commit**

```bash
git add src/engine tests CMakeLists.txt
git commit -m "feat(engine): H.264 preview encoder, subscriptions and preview service"
```

### Task 11: Control API — command router, token-protected WebSocket server, engine executable

**Files:**
- Create: `src/engine/command_router.h`, `src/engine/command_router.cpp`, `src/engine/control_server.h`, `src/engine/control_server.cpp`, `src/engine/engine_main.cpp`
- Modify: `CMakeLists.txt` (`# Task 11` sources; `target_link_libraries(bc_engine PUBLIC ixwebsocket::ixwebsocket bcrypt)`; `breadcamera-engine` executable; tests)
- Test: `tests/command_router_test.cpp`, `tests/control_server_test.cpp`

**Interfaces:**
- Consumes: `Show`, `CameraConfig`, `OutputConfig`, `showToJson`/`cameraFromJson`/`outputFromJson`, `newId`, `SecretMode`, `kSecretKeys` (Task 3); `StateStore` (Task 4); `srtOptionsFromParams` (Task 6); `listMonitors` (Task 7); `Engine`, `DefaultAdapterFactory` (Task 9); `PreviewHub`, `PreviewService`, `PreviewSize`, `parsePreviewSize` (Task 10); `ShowStore` (Task 3).
- Produces:
  - `using bc::ClientId = std::string;`
  - `bc::CommandRouter(Show initial, Effects&)`: `nlohmann::json handle(const ClientId&, std::string_view text)` (always returns a response object), `Show show() const`; nested `class Effects { virtual void showChanged(const Show&) = 0; virtual void subscribePreview(const ClientId&, std::vector<std::string>, PreviewSize) = 0; virtual void requestKeyframe(const std::string&, PreviewSize) = 0; virtual nlohmann::json listMonitors() = 0; virtual void shutdown() = 0; }`.
  - `void bc::validateInput(const InputConfig&)`, `void bc::validateOutput(const OutputConfig&)` (throw `std::invalid_argument`; M4 extends the kind lists here).
  - `bc::ControlServer(int port, std::string token)`: `void setHandlers(Handlers)` with `Handlers { std::function<void(const ClientId&)> open, close; std::function<std::string(const ClientId&, const std::string&)> text; }`, `void start()` (throws when it cannot listen), `void stop()`, `bool send(const ClientId&, const std::string&)`, `bool sendBinary(const ClientId&, const std::vector<uint8_t>&)`, `void broadcast(const std::string&)`.
  - `bool bc::tokenMatches(std::string_view expected, std::string_view given)` (constant time), `std::optional<std::string> bc::tokenFromUri(std::string_view uri)`, `std::string bc::randomToken()` (32 hex chars from `BCryptGenRandom`).
  - Executable `breadcamera-engine.exe --token <hex> [--port 47810] [--show-dir <dir>] [--discovery <file>]`; defaults: show dir `%APPDATA%\BreadCamera\shows`, discovery `%LOCALAPPDATA%\BreadCamera\engine.json`. Exit codes: 0 clean, 2 bad arguments, 3 another engine is running, 4 cannot listen.

Protocol (spec §6.1). Client → engine: `{"id": <number|string>, "cmd": "<name>", "args": {...}}`. Engine → client response: `{"id": <same or null>, "ok": true, "result": ...}` or `{"id": ..., "ok": false, "error": {"code": "...", "message": "..."}}`. Error codes: `bad-json`, `bad-request`, `too-large` (text > 65536 bytes), `unknown-command`, `not-found`, `invalid`. Pushed messages: `{"type":"snapshot","state":{...},"show":{...}}` on connect, `{"type":"patch","ops":[...]}` (≤ 20 Hz), `{"type":"show","show":{...}}` after every show change, `{"type":"stats",...}` at 1 Hz. Every `show` sent to a client is `SecretMode::Redact`.

| cmd | args | result |
|---|---|---|
| `ping` | — | `{"pong": true, "pid": n}` |
| `getShow` | — | redacted show |
| `addCamera` | `{name, input:{kind, params}}` | `{"id"}` |
| `updateCamera` | `{id, name?, input?}` — a secret given as `{"set":true}` keeps the stored value, `{"set":false}` clears it | `{}` |
| `removeCamera` | `{id}` | `{}` |
| `addOutput` | `{cameraId, output:{kind, keyed?, enabled?, params?}}` | `{"id"}` |
| `updateOutput` | `{cameraId, id, keyed?, enabled?, params?}` | `{}` |
| `removeOutput` | `{cameraId, id}` | `{}` |
| `stopAllOutputs` | — | `{}` (every output `enabled=false`) |
| `setSettings` | `{rate?, holdMs?, slatePath?}` | `{}` |
| `subscribePreview` | `{cameraIds:[...], size:"small"\|"large"}` | `{}` |
| `requestKeyframe` | `{cameraId, size}` | `{}` |
| `listMonitors` | — | `[{index,name,width,height,primary}]` |
| `shutdown` | — | `{}` then the engine exits |

Names are 1..80 characters. Camera and output ids are generated by the engine (`newId()`), never taken from the client.

- [ ] **Step 1: Write the failing router tests**

`tests/command_router_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "engine/command_router.h"

using nlohmann::json;

namespace {
struct FakeFx : bc::CommandRouter::Effects {
  int changes = 0;
  bc::Show last;
  bool quit = false;
  void showChanged(const bc::Show& s) override {
    ++changes;
    last = s;
  }
  void subscribePreview(const bc::ClientId&, std::vector<std::string>, bc::PreviewSize) override {}
  void requestKeyframe(const std::string&, bc::PreviewSize) override {}
  json listMonitors() override { return json::array({{{"index", 0}}}); }
  void shutdown() override { quit = true; }
};

json call(bc::CommandRouter& r, json req) { return r.handle("c1", req.dump()); }

const json kSrt = {{"kind", "srt"}, {"params", {{"mode", "listener"}, {"port", 9000}, {"passphrase", "0123456789"}}}};
}  // namespace

TEST_CASE("CommandRouter: addCamera validates, assigns an id and redacts secrets") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto res = call(r, {{"id", 1}, {"cmd", "addCamera"}, {"args", {{"name", "Cam 1"}, {"input", kSrt}}}});
  REQUIRE(res["ok"] == true);
  CHECK(res["id"] == 1);
  const auto id = res["result"]["id"].get<std::string>();
  CHECK(id.size() == 8);
  CHECK(fx.changes == 1);
  CHECK(fx.last.cameras[0].input.params["passphrase"] == "0123456789");
  auto shown = call(r, {{"id", 2}, {"cmd", "getShow"}});
  CHECK(shown["result"]["cameras"][0]["input"]["params"]["passphrase"] == json{{"set", true}});
}

TEST_CASE("CommandRouter: invalid input is refused and the show is unchanged") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  json bad = kSrt;
  bad["params"]["passphrase"] = "short";
  auto res = call(r, {{"id", 1}, {"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", bad}}}});
  CHECK(res["ok"] == false);
  CHECK(res["error"]["code"] == "invalid");
  CHECK(res["error"]["message"].get<std::string>().find("passphrase") != std::string::npos);
  CHECK(fx.changes == 0);
  CHECK(r.show().cameras.empty());
  auto noName = call(r, {{"id", 2}, {"cmd", "addCamera"}, {"args", {{"name", ""}, {"input", kSrt}}}});
  CHECK(noName["error"]["code"] == "invalid");
}

TEST_CASE("CommandRouter: updateCamera keeps or clears a redacted secret") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto id = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", kSrt}}}})["result"]["id"];
  json keep = kSrt;
  keep["params"]["passphrase"] = {{"set", true}};
  keep["params"]["port"] = 9001;
  REQUIRE(call(r, {{"cmd", "updateCamera"}, {"args", {{"id", id}, {"input", keep}}}})["ok"] == true);
  CHECK(r.show().cameras[0].input.params["passphrase"] == "0123456789");
  CHECK(r.show().cameras[0].input.params["port"] == 9001);
  json clear = kSrt;
  clear["params"]["passphrase"] = {{"set", false}};
  REQUIRE(call(r, {{"cmd", "updateCamera"}, {"args", {{"id", id}, {"input", clear}}}})["ok"] == true);
  CHECK(r.show().cameras[0].input.params["passphrase"] == "");
}

TEST_CASE("CommandRouter: outputs, stopAllOutputs and settings") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", kSrt}}}})["result"]["id"];
  auto add = call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "fullscreen"}, {"params", {{"monitor", 0}}}}}}}});
  REQUIRE(add["ok"] == true);
  CHECK(call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "teleport"}}}}}})["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "fullscreen"}, {"params", {{"monitor", -1}}}}}}}})["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", "nope"}, {"output", {{"kind", "ndi"}}}}}})["error"]["code"] == "not-found");
  REQUIRE(call(r, {{"cmd", "stopAllOutputs"}})["ok"] == true);
  CHECK(r.show().cameras[0].outputs[0].enabled == false);
  CHECK(call(r, {{"cmd", "setSettings"}, {"args", {{"rate", "30/1"}}}})["error"]["code"] == "invalid");
  REQUIRE(call(r, {{"cmd", "setSettings"}, {"args", {{"rate", "50/1"}, {"holdMs", 2000}}}})["ok"] == true);
  CHECK(r.show().settings.rate == bc::Rate{50, 1});
  CHECK(r.show().settings.holdMs == 2000);
  REQUIRE(call(r, {{"cmd", "removeOutput"}, {"args", {{"cameraId", cam}, {"id", add["result"]["id"]}}}})["ok"] == true);
  REQUIRE(call(r, {{"cmd", "removeCamera"}, {"args", {{"id", cam}}}})["ok"] == true);
  CHECK(call(r, {{"cmd", "removeCamera"}, {"args", {{"id", cam}}}})["error"]["code"] == "not-found");
}

TEST_CASE("CommandRouter: hostile or malformed messages get an error, never a crash") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  CHECK(r.handle("c1", "not json")["error"]["code"] == "bad-json");
  CHECK(r.handle("c1", "[]")["error"]["code"] == "bad-request");
  CHECK(r.handle("c1", R"({"cmd":5})")["error"]["code"] == "bad-request");
  CHECK(r.handle("c1", R"({"id":7,"cmd":"addCamera","args":"x"})")["error"]["code"] == "bad-request");
  CHECK(r.handle("c1", R"({"id":7,"cmd":"addCamera","args":{"name":5}})")["error"]["code"] == "bad-request");
  auto unknown = r.handle("c1", R"({"id":7,"cmd":"fly"})");
  CHECK(unknown["error"]["code"] == "unknown-command");
  CHECK(unknown["id"] == 7);
  CHECK(r.handle("c1", std::string(100'000, ' '))["error"]["code"] == "too-large");
  CHECK(fx.changes == 0);
  CHECK(call(r, {{"cmd", "listMonitors"}})["result"].size() == 1);
  CHECK(call(r, {{"cmd", "ping"}})["result"]["pong"] == true);
  call(r, {{"cmd", "shutdown"}});
  CHECK(fx.quit);
}
```

`tests/control_server_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <thread>

#include "engine/control_server.h"

using namespace std::chrono_literals;

TEST_CASE("tokenFromUri and tokenMatches") {
  CHECK(bc::tokenFromUri("/?token=abc123") == "abc123");
  CHECK(bc::tokenFromUri("/?x=1&token=abc&y=2") == "abc");
  CHECK_FALSE(bc::tokenFromUri("/"));
  CHECK(bc::tokenMatches("abc", "abc"));
  CHECK_FALSE(bc::tokenMatches("abc", "abd"));
  CHECK_FALSE(bc::tokenMatches("abc", "ab"));
  CHECK_FALSE(bc::tokenMatches("abc", ""));
  CHECK(bc::randomToken().size() == 32);
  CHECK(bc::randomToken() != bc::randomToken());
}

TEST_CASE("ControlServer: refuses a wrong token and answers a right one") {
  ix::initNetSystem();
  bc::ControlServer server(47820, "goodtoken");
  std::atomic<int> opens{0};
  server.setHandlers({[&](const bc::ClientId&) { ++opens; }, [](const bc::ClientId&) {},
                      [](const bc::ClientId&, const std::string& text) { return "echo:" + text; }});
  server.start();

  ix::WebSocket bad;
  bad.setUrl("ws://127.0.0.1:47820/?token=wrong");
  bad.disableAutomaticReconnection();
  std::atomic<int> closeCode{0};
  bad.setOnMessageCallback([&](const ix::WebSocketMessagePtr& m) {
    if (m->type == ix::WebSocketMessageType::Close) closeCode = m->closeInfo.code;
  });
  bad.start();
  for (int i = 0; i < 100 && closeCode == 0; ++i) std::this_thread::sleep_for(20ms);
  CHECK(closeCode == 4401);
  bad.stop();
  CHECK(opens == 0);

  ix::WebSocket good;
  good.setUrl("ws://127.0.0.1:47820/?token=goodtoken");
  good.disableAutomaticReconnection();
  std::atomic<bool> open{false};
  std::string reply;
  std::mutex m;
  good.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) open = true;
    if (msg->type == ix::WebSocketMessageType::Message) {
      std::lock_guard l(m);
      reply = msg->str;
    }
  });
  good.start();
  for (int i = 0; i < 100 && !open; ++i) std::this_thread::sleep_for(20ms);
  REQUIRE(open);
  good.send("hello");
  for (int i = 0; i < 100; ++i) {
    {
      std::lock_guard l(m);
      if (!reply.empty()) break;
    }
    std::this_thread::sleep_for(20ms);
  }
  {
    std::lock_guard l(m);
    CHECK(reply == "echo:hello");
  }
  CHECK(opens == 1);
  good.stop();
  server.stop();
}
```

Add both files to `bc_tests`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "CommandRouter*,ControlServer*,token*"`
Expected: build FAILS — `engine/command_router.h` not found.

- [ ] **Step 3: Implement the router**

`src/engine/command_router.h`:
```cpp
#pragma once
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/show.h"
#include "engine/preview_hub.h"

namespace bc {
using ClientId = std::string;

// Throw std::invalid_argument with a user-readable message (spec §6.1, trust boundary).
void validateInput(const InputConfig& in);
void validateOutput(const OutputConfig& out);

// Applies control commands to the show. Thread-safe: each WebSocket connection calls
// handle() from its own thread.
class CommandRouter {
 public:
  class Effects {
   public:
    virtual ~Effects() = default;
    virtual void showChanged(const Show& show) = 0;
    virtual void subscribePreview(const ClientId& client, std::vector<std::string> cameraIds, PreviewSize size) = 0;
    virtual void requestKeyframe(const std::string& cameraId, PreviewSize size) = 0;
    virtual nlohmann::json listMonitors() = 0;
    virtual void shutdown() = 0;
  };

  CommandRouter(Show initial, Effects& fx) : show_(std::move(initial)), fx_(fx) {}
  nlohmann::json handle(const ClientId& client, std::string_view text);
  Show show() const;

 private:
  nlohmann::json dispatch(const ClientId& client, const std::string& cmd, const nlohmann::json& args);
  mutable std::mutex m_;
  Show show_;
  Effects& fx_;
};
}  // namespace bc
```

`src/engine/command_router.cpp`:
```cpp
#include "engine/command_router.h"

#include <windows.h>

#include <stdexcept>

#include "io/srt_params.h"

namespace bc {
namespace {
using nlohmann::json;

struct CmdError : std::runtime_error {
  std::string code;
  CmdError(std::string c, const std::string& msg) : std::runtime_error(msg), code(std::move(c)) {}
};

const json& obj(const json& args, const char* key) {
  if (!args.contains(key) || !args[key].is_object()) throw CmdError("bad-request", std::string(key) + " must be an object");
  return args[key];
}
std::string str(const json& args, const char* key) {
  if (!args.contains(key) || !args[key].is_string()) throw CmdError("bad-request", std::string(key) + " must be text");
  return args[key].get<std::string>();
}
std::string name(const json& args) {
  auto n = str(args, "name");
  if (n.empty() || n.size() > 80) throw CmdError("invalid", "name must be 1 to 80 characters");
  return n;
}

// Replaces {"set":true} with the stored secret and {"set":false} with "".
json mergeSecrets(json incoming, const json& stored) {
  for (auto key : kSecretKeys) {
    const std::string k(key);
    if (!incoming.contains(k) || !incoming[k].is_object()) continue;
    const bool keep = incoming[k].value("set", false);
    incoming[k] = keep && stored.contains(k) ? stored[k] : json("");
  }
  return incoming;
}

InputConfig inputFrom(const json& j, const json& storedParams = json::object()) {
  InputConfig in;
  in.kind = str(j, "kind");
  in.params = j.value("params", json::object());
  if (!in.params.is_object()) throw CmdError("bad-request", "params must be an object");
  in.params = mergeSecrets(in.params, storedParams);
  try {
    validateInput(in);
  } catch (const std::invalid_argument& e) {
    throw CmdError("invalid", e.what());
  }
  return in;
}

void checkOutput(const OutputConfig& o) {
  try {
    validateOutput(o);
  } catch (const std::invalid_argument& e) {
    throw CmdError("invalid", e.what());
  }
}

PreviewSize sizeArg(const json& args) {
  auto s = parsePreviewSize(args.value("size", std::string("small")));
  if (!s) throw CmdError("invalid", "size must be small or large");
  return *s;
}
}  // namespace

void validateInput(const InputConfig& in) {
  if (in.kind == "srt") {
    srtOptionsFromParams(in.params);
    return;
  }
  throw std::invalid_argument("unknown input kind '" + in.kind + "'");
}

void validateOutput(const OutputConfig& o) {
  if (o.kind == "ndi") {
    if (o.params.contains("name")) {
      if (!o.params["name"].is_string()) throw std::invalid_argument("NDI name must be text");
      const auto n = o.params["name"].get<std::string>();
      if (n.empty() || utf8Length(n) > 200) throw std::invalid_argument("NDI name must be 1 to 200 characters");
    }
    return;
  }
  if (o.kind == "fullscreen") {
    if (o.params.contains("monitor") && (!o.params["monitor"].is_number_integer() || o.params["monitor"].get<int>() < 0)) {
      throw std::invalid_argument("monitor must be a whole number from 0");
    }
    return;
  }
  throw std::invalid_argument("unknown output kind '" + o.kind + "'");
}

Show CommandRouter::show() const {
  std::lock_guard l(m_);
  return show_;
}

json CommandRouter::handle(const ClientId& client, std::string_view text) {
  json id = nullptr;
  try {
    if (text.size() > 65536) throw CmdError("too-large", "message is larger than 64 KiB");
    json req;
    try {
      req = json::parse(text);
    } catch (const json::exception&) {
      throw CmdError("bad-json", "message is not valid JSON");
    }
    if (!req.is_object()) throw CmdError("bad-request", "message must be a JSON object");
    if (req.contains("id") && (req["id"].is_number() || req["id"].is_string())) id = req["id"];
    if (!req.contains("cmd") || !req["cmd"].is_string()) throw CmdError("bad-request", "cmd must be text");
    const json args = req.value("args", json::object());
    if (!args.is_object()) throw CmdError("bad-request", "args must be an object");
    json result = dispatch(client, req["cmd"].get<std::string>(), args);
    return {{"id", id}, {"ok", true}, {"result", result}};
  } catch (const CmdError& e) {
    return {{"id", id}, {"ok", false}, {"error", {{"code", e.code}, {"message", e.what()}}}};
  } catch (const json::exception& e) {
    return {{"id", id}, {"ok", false}, {"error", {{"code", "bad-request"}, {"message", e.what()}}}};
  } catch (const std::exception& e) {
    return {{"id", id}, {"ok", false}, {"error", {{"code", "invalid"}, {"message", e.what()}}}};
  }
}

json CommandRouter::dispatch(const ClientId& client, const std::string& cmd, const json& args) {
  if (cmd == "ping") return {{"pong", true}, {"pid", GetCurrentProcessId()}};
  if (cmd == "listMonitors") return fx_.listMonitors();
  if (cmd == "shutdown") {
    fx_.shutdown();
    return json::object();
  }
  if (cmd == "subscribePreview") {
    if (!args.contains("cameraIds") || !args["cameraIds"].is_array()) throw CmdError("bad-request", "cameraIds must be a list");
    std::vector<std::string> ids;
    for (auto& v : args["cameraIds"]) {
      if (!v.is_string()) throw CmdError("bad-request", "cameraIds must be text");
      ids.push_back(v.get<std::string>());
    }
    fx_.subscribePreview(client, std::move(ids), sizeArg(args));
    return json::object();
  }
  if (cmd == "requestKeyframe") {
    fx_.requestKeyframe(str(args, "cameraId"), sizeArg(args));
    return json::object();
  }

  std::lock_guard l(m_);
  if (cmd == "getShow") return showToJson(show_, SecretMode::Redact);

  Show next = show_;
  json result = json::object();
  auto findCam = [&](const std::string& camId) -> CameraConfig& {
    auto* c = next.findCamera(camId);
    if (!c) throw CmdError("not-found", "no camera " + camId);
    return *c;
  };
  if (cmd == "addCamera") {
    CameraConfig c;
    c.id = newId();
    c.name = name(args);
    c.input = inputFrom(obj(args, "input"));
    next.cameras.push_back(c);
    result = {{"id", c.id}};
  } else if (cmd == "updateCamera") {
    auto& c = findCam(str(args, "id"));
    if (args.contains("name")) c.name = name(args);
    if (args.contains("input")) c.input = inputFrom(obj(args, "input"), c.input.params);
  } else if (cmd == "removeCamera") {
    const auto camId = str(args, "id");
    findCam(camId);
    std::erase_if(next.cameras, [&](const CameraConfig& c) { return c.id == camId; });
  } else if (cmd == "addOutput") {
    auto& c = findCam(str(args, "cameraId"));
    const auto& o = obj(args, "output");
    OutputConfig out;
    out.id = newId();
    out.kind = str(o, "kind");
    out.keyed = o.value("keyed", false);
    out.enabled = o.value("enabled", true);
    out.params = o.value("params", json::object());
    if (!out.params.is_object()) throw CmdError("bad-request", "params must be an object");
    checkOutput(out);
    c.outputs.push_back(out);
    result = {{"id", out.id}};
  } else if (cmd == "updateOutput") {
    auto& c = findCam(str(args, "cameraId"));
    const auto outId = str(args, "id");
    auto it = std::find_if(c.outputs.begin(), c.outputs.end(), [&](const OutputConfig& o) { return o.id == outId; });
    if (it == c.outputs.end()) throw CmdError("not-found", "no output " + outId);
    if (args.contains("keyed")) it->keyed = args["keyed"].get<bool>();
    if (args.contains("enabled")) it->enabled = args["enabled"].get<bool>();
    if (args.contains("params")) it->params = obj(args, "params");
    checkOutput(*it);
  } else if (cmd == "removeOutput") {
    auto& c = findCam(str(args, "cameraId"));
    const auto outId = str(args, "id");
    const auto before = c.outputs.size();
    std::erase_if(c.outputs, [&](const OutputConfig& o) { return o.id == outId; });
    if (c.outputs.size() == before) throw CmdError("not-found", "no output " + outId);
  } else if (cmd == "stopAllOutputs") {
    for (auto& c : next.cameras)
      for (auto& o : c.outputs) o.enabled = false;
  } else if (cmd == "setSettings") {
    if (args.contains("rate")) {
      auto r = parseShowRate(str(args, "rate"));
      if (!r) throw CmdError("invalid", "rate must be 50/1, 60000/1001 or 60/1");
      next.settings.rate = *r;
    }
    if (args.contains("holdMs")) {
      if (!args["holdMs"].is_number_integer()) throw CmdError("bad-request", "holdMs must be a whole number");
      const int h = args["holdMs"].get<int>();
      if (h < 0 || h > 60000) throw CmdError("invalid", "holdMs must be 0 to 60000");
      next.settings.holdMs = h;
    }
    if (args.contains("slatePath")) next.settings.slatePath = str(args, "slatePath");
  } else {
    throw CmdError("unknown-command", "unknown command '" + cmd + "'");
  }
  show_ = std::move(next);
  fx_.showChanged(show_);
  return result;
}
}  // namespace bc
```

Note: `args["keyed"].get<bool>()` throws `json::type_error` for a non-bool — caught in `handle()` as `bad-request`.

- [ ] **Step 4: Implement the server**

`src/engine/control_server.h`:
```cpp
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ix {
class WebSocketServer;
class WebSocket;
}  // namespace ix

namespace bc {
using ClientId = std::string;

bool tokenMatches(std::string_view expected, std::string_view given);
std::optional<std::string> tokenFromUri(std::string_view uri);
std::string randomToken();

// WebSocket control API on 127.0.0.1 only; every connection must carry ?token= (spec §6.1).
class ControlServer {
 public:
  struct Handlers {
    std::function<void(const ClientId&)> open;
    std::function<void(const ClientId&)> close;
    std::function<std::string(const ClientId&, const std::string&)> text;  // returns the reply
  };
  ControlServer(int port, std::string token);
  ~ControlServer();
  void setHandlers(Handlers h) { handlers_ = std::move(h); }
  void start();
  void stop();
  bool send(const ClientId& id, const std::string& text);
  bool sendBinary(const ClientId& id, const std::vector<uint8_t>& data);
  void broadcast(const std::string& text);

 private:
  int port_;
  std::string token_;
  Handlers handlers_;
  std::unique_ptr<ix::WebSocketServer> server_;
  std::mutex m_;
  std::map<ClientId, ix::WebSocket*> clients_;
};
}  // namespace bc
```

`src/engine/control_server.cpp`:
```cpp
#include "engine/control_server.h"

#include <windows.h>
#include <bcrypt.h>

#include <ixwebsocket/IXWebSocketServer.h>

#include <stdexcept>

namespace bc {
bool tokenMatches(std::string_view expected, std::string_view given) {
  if (expected.empty() || expected.size() != given.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < expected.size(); ++i) diff |= static_cast<unsigned char>(expected[i] ^ given[i]);
  return diff == 0;
}

std::optional<std::string> tokenFromUri(std::string_view uri) {
  auto q = uri.find('?');
  if (q == std::string_view::npos) return std::nullopt;
  auto query = uri.substr(q + 1);
  while (!query.empty()) {
    auto amp = query.find('&');
    auto part = query.substr(0, amp);
    if (part.rfind("token=", 0) == 0) return std::string(part.substr(6));
    if (amp == std::string_view::npos) break;
    query = query.substr(amp + 1);
  }
  return std::nullopt;
}

std::string randomToken() {
  unsigned char bytes[16];
  if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    throw std::runtime_error("BCryptGenRandom failed");
  }
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  for (auto b : bytes) {
    s += kHex[b >> 4];
    s += kHex[b & 15];
  }
  return s;
}

ControlServer::ControlServer(int port, std::string token) : port_(port), token_(std::move(token)) {}

ControlServer::~ControlServer() { stop(); }

void ControlServer::start() {
  server_ = std::make_unique<ix::WebSocketServer>(port_, "127.0.0.1");
  server_->disablePerMessageDeflate();
  server_->setOnClientMessageCallback(
      [this](std::shared_ptr<ix::ConnectionState> state, ix::WebSocket& ws, const ix::WebSocketMessagePtr& msg) {
        const ClientId id = state->getId();
        switch (msg->type) {
          case ix::WebSocketMessageType::Open: {
            auto token = tokenFromUri(msg->openInfo.uri);
            if (!token || !tokenMatches(token_, *token)) {
              ws.close(4401, "unauthorized");
              return;
            }
            {
              std::lock_guard l(m_);
              clients_[id] = &ws;
            }
            if (handlers_.open) handlers_.open(id);
            break;
          }
          case ix::WebSocketMessageType::Message: {
            {
              std::lock_guard l(m_);
              if (!clients_.contains(id)) return;  // never authorised
            }
            if (!msg->binary && handlers_.text) ws.send(handlers_.text(id, msg->str));
            break;
          }
          case ix::WebSocketMessageType::Close: {
            bool known;
            {
              std::lock_guard l(m_);
              known = clients_.erase(id) > 0;
            }
            if (known && handlers_.close) handlers_.close(id);
            break;
          }
          default:
            break;
        }
      });
  auto res = server_->listen();
  if (!res.first) throw std::runtime_error("cannot listen on 127.0.0.1:" + std::to_string(port_) + ": " + res.second);
  server_->start();
}

void ControlServer::stop() {
  if (!server_) return;
  server_->stop();
  server_.reset();
  std::lock_guard l(m_);
  clients_.clear();
}

bool ControlServer::send(const ClientId& id, const std::string& text) {
  std::lock_guard l(m_);
  auto it = clients_.find(id);
  if (it == clients_.end()) return false;
  return it->second->send(text).success;
}

bool ControlServer::sendBinary(const ClientId& id, const std::vector<uint8_t>& data) {
  std::lock_guard l(m_);
  auto it = clients_.find(id);
  if (it == clients_.end()) return false;
  return it->second->sendBinary(std::string(data.begin(), data.end())).success;
}

void ControlServer::broadcast(const std::string& text) {
  std::lock_guard l(m_);
  for (auto& [id, ws] : clients_) ws->send(text);
}
}  // namespace bc
```

ponytail: sends happen under the client-map lock, so one slow client briefly delays the others; per-client send queues if previews to many viewers stutter.

- [ ] **Step 5: Write the engine executable**

`src/engine/engine_main.cpp`:
```cpp
#include <windows.h>
#include <shlobj.h>

#include <ixwebsocket/IXNetSystem.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "core/show_store.h"
#include "core/state_store.h"
#include "core/utf8.h"
#include "engine/adapter_factory.h"
#include "engine/command_router.h"
#include "engine/control_server.h"
#include "engine/engine.h"
#include "engine/preview_service.h"
#include "gpu/converter.h"
#include "gpu/device.h"
#include "io/monitors.h"

namespace {
using nlohmann::json;
std::atomic<bool> g_quit{false};

std::filesystem::path knownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  SHGetKnownFolderPath(id, 0, nullptr, &p);
  std::filesystem::path out(p);
  CoTaskMemFree(p);
  return out;
}

void writeDiscovery(const std::filesystem::path& file, int port, const std::string& token) {
  std::filesystem::create_directories(file.parent_path());
  const auto tmp = file.wstring() + L".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    f << json{{"pid", GetCurrentProcessId()}, {"port", port}, {"token", token}}.dump();
  }
  MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  int port = 47810;
  std::string token;
  std::filesystem::path showDir = knownFolder(FOLDERID_RoamingAppData) / L"BreadCamera" / L"shows";
  std::filesystem::path discovery = knownFolder(FOLDERID_LocalAppData) / L"BreadCamera" / L"engine.json";
  for (int i = 1; i + 1 < argc; i += 2) {
    std::wstring k = argv[i], v = argv[i + 1];
    if (k == L"--port") port = std::stoi(v);
    else if (k == L"--token") token = bc::toUtf8(v);
    else if (k == L"--show-dir") showDir = v;
    else if (k == L"--discovery") discovery = v;
    else {
      std::wcerr << L"unknown argument " << k << L"\n";
      return 2;
    }
  }
  if (token.size() < 16 || port <= 0 || port > 65535) {
    std::cerr << "usage: breadcamera-engine --token <at least 16 chars> [--port N] [--show-dir D] [--discovery F]\n";
    return 2;
  }
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\BreadCameraEngine");
  if (GetLastError() == ERROR_ALREADY_EXISTS) return 3;
  SetConsoleCtrlHandler([](DWORD) -> BOOL { g_quit = true; return TRUE; }, TRUE);
  ix::initNetSystem();

  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::StateStore state;
  bc::DefaultAdapterFactory factory(*gpu, pool);
  bc::ShowStore store(showDir);
  auto loaded = store.load();
  state.set("/engine/gpu", gpu->info().adapterName);
  state.set("/engine/warnings/show", loaded.warning);

  bc::Engine engine(conv, state, factory);
  engine.apply(loaded.show);
  bc::ControlServer server(port, token);
  bc::PreviewHub hub;
  bc::PreviewService previews(conv, state, hub, [&](const bc::ClientId& c, std::vector<uint8_t> m) { server.sendBinary(c, m); });
  engine.setFrameTap([&](uint64_t tick, const auto& frames) { previews.onTick(tick, frames); });

  struct Fx : bc::CommandRouter::Effects {
    bc::Engine& engine;
    bc::ShowStore& store;
    bc::ControlServer& server;
    bc::PreviewHub& hub;
    bc::PreviewService& previews;
    Fx(bc::Engine& e, bc::ShowStore& s, bc::ControlServer& sv, bc::PreviewHub& h, bc::PreviewService& p)
        : engine(e), store(s), server(sv), hub(h), previews(p) {}
    void showChanged(const bc::Show& s) override {
      engine.apply(s);
      store.scheduleSave(s);
      server.broadcast(json{{"type", "show"}, {"show", bc::showToJson(s, bc::SecretMode::Redact)}}.dump());
    }
    void subscribePreview(const bc::ClientId& c, std::vector<std::string> ids, bc::PreviewSize size) override {
      for (auto& [id, sz] : hub.subscribe(c, std::move(ids), size)) previews.requestKeyframe(id, sz);
    }
    void requestKeyframe(const std::string& id, bc::PreviewSize size) override { previews.requestKeyframe(id, size); }
    json listMonitors() override {
      json out = json::array();
      for (auto& m : bc::listMonitors()) {
        out.push_back({{"index", m.index}, {"name", m.name}, {"width", m.width}, {"height", m.height}, {"primary", m.primary}});
      }
      return out;
    }
    void shutdown() override { g_quit = true; }
  } fx(engine, store, server, hub, previews);
  bc::CommandRouter router(loaded.show, fx);

  server.setHandlers({
      [&](const bc::ClientId& id) {
        server.send(id, json{{"type", "snapshot"}, {"state", state.snapshot()},
                             {"show", bc::showToJson(router.show(), bc::SecretMode::Redact)}}.dump());
      },
      [&](const bc::ClientId& id) { hub.dropClient(id); },
      [&](const bc::ClientId& id, const std::string& text) { return router.handle(id, text).dump(); },
  });
  try {
    server.start();
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 4;
  }
  engine.start();
  writeDiscovery(discovery, port, token);

  int ticks = 0;
  while (!g_quit) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto ops = state.takePatch();
    if (!ops.empty()) server.broadcast(json{{"type", "patch"}, {"ops", ops}}.dump());
    if (++ticks % 20 == 0) {
      auto st = engine.stats();
      json cams = json::object();
      for (auto& [id, c] : st.cameras) {
        cams[id] = {{"fps", c.fps}, {"drops", c.drops}, {"repeats", c.repeats}, {"latencyMs", c.latencyMs}};
      }
      server.broadcast(json{{"type", "stats"}, {"engine", {{"overruns", st.overruns}}}, {"cameras", cams}}.dump());
      const auto err = store.lastSaveError();
      state.set("/engine/warnings/save", err.empty() ? "" : "the show could not be saved: " + err);
    }
  }
  engine.stop();
  server.stop();
  store.flush();
  std::error_code ec;
  std::filesystem::remove(discovery, ec);
  CloseHandle(single);
  return 0;
}
```

Add to `CMakeLists.txt`:
```cmake
add_executable(breadcamera-engine src/engine/engine_main.cpp)
target_link_libraries(breadcamera-engine PRIVATE bc_engine shell32 ole32)
bc_copy_runtime(breadcamera-engine)
```

- [ ] **Step 6: Run the unit tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "CommandRouter*,ControlServer*,token*"`
Expected: 7 cases pass.

- [ ] **Step 7: Smoke-test the executable**

In bash:
```bash
SD="$(mktemp -d)"
./build/release/breadcamera-engine.exe --token 0123456789abcdef0123 --port 47830 --show-dir "$(cygpath -w "$SD")" --discovery "$(cygpath -w "$SD")\\engine.json" &
sleep 3
"/c/Program Files/nodejs/node" -e '
const ws = new WebSocket("ws://127.0.0.1:47830/?token=0123456789abcdef0123");
ws.onmessage = (e) => { const m = JSON.parse(e.data); console.log(m.type, JSON.stringify(m.state ?? m.result ?? m).slice(0, 200));
  if (m.type === "snapshot") ws.send(JSON.stringify({id: 1, cmd: "shutdown"})); };
ws.onclose = () => process.exit(0);'
wait
cat "$SD/engine.json" 2>/dev/null || echo "discovery removed on exit (expected)"
```
Expected: a `snapshot` line naming the GPU (`/engine/gpu`), then the engine exits 0 and the discovery file is gone. Paste the output into the task report.

- [ ] **Step 8: Commit**

```bash
git add src/engine tests CMakeLists.txt
git commit -m "feat(engine): token-protected control API, command router and engine executable"
```

### Task 11A: Show format, per-output format, input health and Unicode names

Added 2026-10-08 after the UI mockup review (spec §3.2 "To RGBA", §4.3 "Output format", §6.1 input health, §6.4 "Names are Unicode end to end"). It changes code from Tasks 3, 5, 9 and 11; the excerpts below are the only regions it touches.

**Files:**
- Create: `src/core/input_health.h`, `src/gpu/shaders/fit_linear.hlsl`
- Modify: `src/core/show.h`, `src/core/show.cpp` (Task 3), `src/core/utf8.h` (Task 5), `src/gpu/converter.h`, `src/gpu/converter.cpp` (Task 5), `src/engine/camera.h`, `src/engine/camera.cpp`, `src/engine/engine.h`, `src/engine/engine.cpp` (Task 9), `src/engine/command_router.cpp` (Task 11), `CMakeLists.txt` (`bc_shader(fit_linear cs_5_0)`)
- Test: `tests/show_test.cpp`, `tests/gpu_test.cpp`, `tests/engine_test.cpp`, `tests/command_router_test.cpp`, new `tests/input_health_test.cpp`

**Interfaces:**
- Consumes: `Show`, `ShowSettings`, `OutputConfig`, `showToJson`/`showFromJson` (Task 3); `Converter`, `TextureRef`, `toUtf8`/`toWide` (Task 5); `Camera`, `Engine`, `FeedPhase`, `SignalState` (Tasks 2, 4, 9); `CommandRouter`, `validateOutput` (Task 11).
- Produces:
  - `ShowSettings` gains `int width = 1920; int height = 1080;` (JSON `settings.resolution`: `"1280x720"` or `"1920x1080"`; missing → `"1920x1080"`).
  - `OutputConfig` gains, **after** `params`, `int width = 0; int height = 0; bool halfRate = false;` (0×0 = same as show). JSON `outputs[].format`: `{"width": 0, "height": 0, "halfRate": false}`; missing → those defaults. Allowed sizes: 0×0, 1280×720, 960×540.
  - `std::optional<std::pair<int, int>> bc::parseShowResolution(std::string_view)`, `std::string bc::resolutionStr(int w, int h)` (`"1920x1080"`), `bool bc::isOutputSize(int w, int h)`.
  - `size_t bc::utf8Length(std::string_view)` (code points) in `core/utf8.h`.
  - `TextureRef bc::Converter::fit(const GpuTexture& linear, uint32_t w, uint32_t h)` — scales a linear texture into w×h, keeping its aspect, with black bars.
  - `void bc::Camera::setFormat(uint32_t w, uint32_t h)`.
  - `std::string_view bc::inputHealth(SignalState input, FeedPhase phase, bool stutteredInLast10s)` → `"receiving" | "degraded" | "down" | "offline"`.
  - State: `/cameras/<id>/input/health`, `/engine/resolution`.
  - Commands: `setSettings` accepts `resolution`; `addOutput.output.format` and `updateOutput.format` accept `{width, height, halfRate}`.

Rules:
- Every camera's frame is fitted to the show size right after conversion; the slate is made at the show size. A 4:3 or 1:1 input is letterboxed, never stretched.
- Per output: frames of the output's size are made once per tick per distinct size (shared by outputs of that size); the CPU readback is made once per size, only if an output of that size `needsCpu()`. A fullscreen output ignores the size (it scales to its monitor).
- A half-rate output gets the even ticks only, with `rate = {num, den * 2}`, `tick = index / 2`, and both ticks' audio (the odd tick's samples first), so its audio never has a gap.
- Health (spec §6.1): `Holding`/`Slate` → `down`; `NoSignal` → `offline`; `Live` with the input `Connecting`/`Offline` or a stutter (a gap > 100 ms between fresh frames) in the last 10 s → `degraded`; otherwise `receiving`.
- Names (camera, show, NDI) are UTF-8 everywhere; the 1..80 limit counts code points, not bytes.

- [ ] **Step 1: Write the failing tests**

`tests/input_health_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "core/input_health.h"

using bc::FeedPhase;
using bc::SignalState;

TEST_CASE("inputHealth: phases and stutter map to the four operator states") {
  CHECK(bc::inputHealth(SignalState::Live, FeedPhase::Live, false) == "receiving");
  CHECK(bc::inputHealth(SignalState::Live, FeedPhase::Live, true) == "degraded");
  CHECK(bc::inputHealth(SignalState::Connecting, FeedPhase::Live, false) == "degraded");
  CHECK(bc::inputHealth(SignalState::Live, FeedPhase::Holding, false) == "down");
  CHECK(bc::inputHealth(SignalState::Offline, FeedPhase::Slate, false) == "down");
  CHECK(bc::inputHealth(SignalState::Connecting, FeedPhase::NoSignal, false) == "offline");
}
```

Append to `tests/show_test.cpp`:
```cpp
TEST_CASE("Show: resolution and output format round-trip, and old files load with defaults") {
  auto s = sampleShow();
  s.settings.width = 1280;
  s.settings.height = 720;
  s.cameras[0].outputs[0].width = 960;
  s.cameras[0].outputs[0].height = 540;
  s.cameras[0].outputs[0].halfRate = true;
  auto j = bc::showToJson(s, bc::SecretMode::Plain);
  CHECK(j["settings"]["resolution"] == "1280x720");
  CHECK(j["cameras"][0]["outputs"][0]["format"] == nlohmann::json{{"width", 960}, {"height", 540}, {"halfRate", true}});
  CHECK(bc::showFromJson(j) == s);

  j["settings"].erase("resolution");
  j["cameras"][0]["outputs"][0].erase("format");
  auto old = bc::showFromJson(j);
  CHECK(old.settings.width == 1920);
  CHECK(old.settings.height == 1080);
  CHECK(old.cameras[0].outputs[0].width == 0);
  CHECK_FALSE(old.cameras[0].outputs[0].halfRate);
}

TEST_CASE("Show: unsupported sizes are rejected") {
  auto j = bc::showToJson(sampleShow(), bc::SecretMode::Plain);
  j["settings"]["resolution"] = "3840x2160";
  CHECK_THROWS_AS(bc::showFromJson(j), bc::ShowError);
  j["settings"]["resolution"] = "1920x1080";
  j["cameras"][0]["outputs"][0]["format"] = {{"width", 800}, {"height", 600}, {"halfRate", false}};
  CHECK_THROWS_AS(bc::showFromJson(j), bc::ShowError);
}

TEST_CASE("Show: Unicode names survive the file unchanged") {
  auto s = sampleShow();
  s.cameras[0].name = "Sân khấu trái – Cam 2 📷";
  auto back = bc::showFromJson(nlohmann::json::parse(bc::showToJson(s, bc::SecretMode::Plain).dump()));
  CHECK(back.cameras[0].name == "Sân khấu trái – Cam 2 📷");
  CHECK(bc::toUtf8(bc::toWide(back.cameras[0].name)) == back.cameras[0].name);
  CHECK(bc::utf8Length("Sân khấu") == 8);
}
```
(Add `#include "core/utf8.h"` and `#include <nlohmann/json.hpp>` at the top of `show_test.cpp` if not already there.)

Append to `tests/gpu_test.cpp`:
```cpp
TEST_CASE("Converter: fit scales to the target size and letterboxes another aspect") {
  Gpu g;
  std::vector<uint8_t> red(36 * 36 * 4);
  for (size_t i = 0; i < red.size(); i += 4) {
    red[i] = 0;
    red[i + 1] = 0;
    red[i + 2] = 255;
    red[i + 3] = 255;
  }
  auto sq = g.conv.toLinear(*g.conv.uploadBgra8(red.data(), 36, 36, 36 * 4), bc::PixelLayout::BGRA8, true);
  auto fitted = g.conv.fit(*sq, 64, 36);
  CHECK(fitted->width == 64);
  CHECK(fitted->height == 36);
  auto img = g.conv.readback(*g.conv.toBgra8(*fitted, 64, 36));
  auto px = [&](int x, int y) { return &img.bgra[(static_cast<size_t>(y) * 64 + x) * 4]; };
  CHECK(px(2, 18)[2] < 5);     // left bar: black
  CHECK(px(61, 18)[2] < 5);    // right bar: black
  CHECK(px(32, 18)[2] > 250);  // centre: the red picture
  CHECK(px(32, 18)[3] == 255);
}
```

In `tests/engine_test.cpp`:
- Add to the anonymous namespace:
```cpp
// Small show format so the tests stay cheap; the engine fits every frame to it.
bc::Show testShow() {
  bc::Show s;
  s.settings.width = 64;
  s.settings.height = 36;
  return s;
}
```
- Replace every `bc::Show show;` in this file with `bc::Show show = testShow();` (the `bc::Show{}` used to clear the cameras stays as it is).
- Replace the test `"Engine: a resolution change mid-stream follows the new size"` with:
```cpp
TEST_CASE("Engine: an input changing size mid-stream still leaves at the show size, without a gap") {
  Rig r;
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("o1")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  r.factory.input("a")->width = 32;
  r.factory.input("a")->height = 18;
  std::this_thread::sleep_for(500ms);
  auto seen = r.factory.output("o1")->seen();
  CHECK(seen.front().width == 64);
  CHECK(seen.back().width == 64);
  CHECK(maxGap(seen) < 50ms);
}
```
- Append:
```cpp
TEST_CASE("Engine: an output can send a smaller size, and a half-rate output gets every second tick with all the audio") {
  Rig r;
  bc::Show show = testShow();
  auto small = out("small");
  small.width = 32;
  small.height = 18;
  auto half = out("half");
  half.halfRate = true;
  show.cameras.push_back(cam("a", {out("full"), small, half}));
  r.engine.apply(show);
  r.engine.start();
  auto t0 = Clock::now();
  std::this_thread::sleep_for(1600ms);
  CHECK(r.factory.output("full")->seen().back().width == 64);
  CHECK(r.factory.output("small")->seen().back().width == 32);
  auto halfSeen = r.factory.output("half")->seen();
  const int n = countBetween(halfSeen, t0 + 500ms, t0 + 1500ms);
  CHECK(n >= 27);
  CHECK(n <= 31);
  for (size_t i = 1; i < halfSeen.size(); ++i) CHECK(halfSeen[i].audioSamples == 2 * 800 * 2);
}

TEST_CASE("Engine: input health is published") {
  Rig r;
  bc::Show show = testShow();
  show.settings.holdMs = 3000;
  show.cameras.push_back(cam("a", {out("o1")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1200ms);
  auto health = [&] { return r.state.snapshot()["cameras"]["a"]["input"]["health"].get<std::string>(); };
  CHECK(health() == "receiving");
  r.factory.input("a")->paused = true;
  std::this_thread::sleep_for(1200ms);
  CHECK(health() == "down");
  r.factory.input("a")->paused = false;
  std::this_thread::sleep_for(1200ms);
  CHECK(health() == "degraded");  // the pause was a stutter less than 10 s ago
  CHECK(r.state.snapshot()["engine"]["resolution"] == "64x36");
}
```

Append to `tests/command_router_test.cpp`:
```cpp
TEST_CASE("CommandRouter: show resolution and output format are validated") {
  FakeFx fx;
  bc::CommandRouter r(bc::Show{}, fx);
  REQUIRE(call(r, {{"cmd", "setSettings"}, {"args", {{"resolution", "1280x720"}}}})["ok"] == true);
  CHECK(r.show().settings.width == 1280);
  CHECK(call(r, {{"cmd", "setSettings"}, {"args", {{"resolution", "4k"}}}})["error"]["code"] == "invalid");
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", kSrt}}}})["result"]["id"];
  auto ok = call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "ndi"}, {"format", {{"width", 960}, {"height", 540}, {"halfRate", true}}}}}}}});
  REQUIRE(ok["ok"] == true);
  auto& o = r.show().cameras[0].outputs[0];
  CHECK(o.width == 960);
  CHECK(o.halfRate);
  CHECK(call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "ndi"}, {"format", {{"width", 1000}, {"height", 500}}}}}}}})["error"]["code"] == "invalid");
  REQUIRE(call(r, {{"cmd", "updateOutput"}, {"args", {{"cameraId", cam}, {"id", ok["result"]["id"]}, {"format", {{"width", 0}, {"height", 0}, {"halfRate", false}}}}}})["ok"] == true);
  CHECK(r.show().cameras[0].outputs[0].width == 0);
}

TEST_CASE("CommandRouter: names are counted in characters, not bytes") {
  FakeFx fx;
  bc::CommandRouter r(bc::Show{}, fx);
  std::string eighty;
  for (int i = 0; i < 80; ++i) eighty += "ấ";  // 3 bytes each
  CHECK(call(r, {{"cmd", "addCamera"}, {"args", {{"name", eighty}, {"input", kSrt}}}})["ok"] == true);
  CHECK(call(r, {{"cmd", "addCamera"}, {"args", {{"name", eighty + "a"}, {"input", kSrt}}}})["error"]["code"] == "invalid");
  CHECK(r.show().cameras[0].name == eighty);
}
```
(If the existing router tests build the router differently, build it the same way here.)

Add `tests/input_health_test.cpp` to `bc_tests`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "*Show*,*fit*,*Engine*,*CommandRouter*,*inputHealth*"`
Expected: build FAILS — `core/input_health.h` not found, `ShowSettings::width` and `Converter::fit` unknown.

- [ ] **Step 3: Model and helpers**

`src/core/input_health.h`:
```cpp
#pragma once
#include <string_view>

#include "core/frame_sync.h"
#include "core/signal_state.h"

namespace bc {
// What the operator sees on the input chip (spec §6.1).
inline std::string_view inputHealth(SignalState input, FeedPhase phase, bool stutteredInLast10s) {
  if (phase == FeedPhase::Holding || phase == FeedPhase::Slate) return "down";
  if (phase == FeedPhase::NoSignal) return "offline";
  if (input != SignalState::Live || stutteredInLast10s) return "degraded";
  return "receiving";
}
}  // namespace bc
```

`src/core/utf8.h` — add:
```cpp
// Number of code points in valid UTF-8 (continuation bytes are 10xxxxxx).
inline size_t utf8Length(std::string_view s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}
```

`src/core/show.h` — in `OutputConfig`, after `params`:
```cpp
  int width = 0;           // 0 x 0 = same as the show (spec §4.3)
  int height = 0;
  bool halfRate = false;   // every second tick
```
In `ShowSettings`, after `rate`:
```cpp
  int width = 1920;   // show format (spec §1.3): 1280x720 or 1920x1080
  int height = 1080;
```
And declare:
```cpp
std::optional<std::pair<int, int>> parseShowResolution(std::string_view s);  // "1280x720" | "1920x1080"
std::string resolutionStr(int w, int h);
bool isOutputSize(int w, int h);  // 0x0, 1280x720 or 960x540
```
(`#include <optional>` and `<utility>`.)

`src/core/show.cpp`:
```cpp
std::optional<std::pair<int, int>> parseShowResolution(std::string_view s) {
  if (s == "1280x720") return std::pair{1280, 720};
  if (s == "1920x1080") return std::pair{1920, 1080};
  return std::nullopt;
}

std::string resolutionStr(int w, int h) { return std::to_string(w) + "x" + std::to_string(h); }

bool isOutputSize(int w, int h) {
  return (w == 0 && h == 0) || (w == 1280 && h == 720) || (w == 960 && h == 540);
}
```
`outputToJson` adds `{"format", {{"width", o.width}, {"height", o.height}, {"halfRate", o.halfRate}}}`. `outputFromJson` reads it:
```cpp
  auto f = j.value("format", json::object());
  if (!f.is_object()) throw ShowError("output format must be an object");
  o.width = f.value("width", 0);
  o.height = f.value("height", 0);
  o.halfRate = f.value("halfRate", false);
  if (!isOutputSize(o.width, o.height)) throw ShowError("output size must be same as show, 1280x720 or 960x540");
```
`showToJson` adds `{"resolution", resolutionStr(s.settings.width, s.settings.height)}` to `settings`. `showFromJson`:
```cpp
  auto res = parseShowResolution(settings.value("resolution", std::string("1920x1080")));
  if (!res) throw ShowError("settings.resolution must be 1280x720 or 1920x1080");
  s.settings.width = res->first;
  s.settings.height = res->second;
```
The schema version stays 1: the new keys are optional with defaults, so older files load unchanged.

- [ ] **Step 4: `Converter::fit`**

`src/gpu/shaders/fit_linear.hlsl`:
```hlsl
Texture2D<float4> gSrc : register(t0);
RWTexture2D<float4> gOut : register(u0);
SamplerState gLinear : register(s0);
cbuffer Params : register(b0) {
  float4 gRow0;
  float4 gRow1;
  float4 gRow2;
  uint2 gOutSize;
  uint2 gSrcSize;
};

// Fits the source inside the output keeping its aspect (spec §3.2); bars are black.
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float s = min(float(gOutSize.x) / gSrcSize.x, float(gOutSize.y) / gSrcSize.y);
  float2 size = float2(gSrcSize) * s;
  float2 p = float2(id.xy) + 0.5 - (float2(gOutSize) - size) * 0.5;
  if (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y) {
    gOut[id.xy] = float4(0, 0, 0, 1);
    return;
  }
  gOut[id.xy] = gSrc.SampleLevel(gLinear, p / size, 0);
}
```
ponytail: plain bilinear, like `linear_to_bgra8`; add a box pre-filter if 1080p → 540p outputs shimmer.

`CMakeLists.txt`: `bc_shader(fit_linear cs_5_0)` after the other three.

`src/gpu/converter.h`: declare `TextureRef fit(const GpuTexture& linear, uint32_t w, uint32_t h);` and add `fitCs_` to the `ComPtr<ID3D11ComputeShader>` members. `src/gpu/converter.cpp`: `#include "fit_linear.h"`, create `fitCs_` from `g_fit_linear` next to the other three, and:
```cpp
TextureRef Converter::fit(const GpuTexture& linear, uint32_t w, uint32_t h) {
  auto out = pool_.acquire(w, h, kLinearFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.outW = w;
  p.outH = h;
  p.srcW = linear.width;
  p.srcH = linear.height;
  ID3D11ShaderResourceView* srvs[] = {linear.srv.Get()};
  dispatch(fitCs_.Get(), srvs, 1, out->uav.Get(), p);
  return out;
}
```

- [ ] **Step 5: Camera — fit to the show, per-output format, health**

`src/engine/camera.h`:
- `#include "core/input_health.h"`.
- Public: `void setFormat(uint32_t w, uint32_t h) { width_ = w; height_ = h; lastConverted_.reset(); }` — called from the tick thread's lock (Engine holds `mutex_`), so no extra locking.
- `OutputSlot` gains `std::vector<float> carryAudio;` (half-rate audio from the odd tick).
- Private members: `uint32_t width_ = 1920, height_ = 1080;`, `std::atomic<SignalState> inputState_{SignalState::Connecting};`, `std::optional<std::chrono::steady_clock::time_point> lastFresh_;`, `std::chrono::steady_clock::time_point lastStutter_{};` (guarded by `statsMutex_`).

`src/engine/camera.cpp`:
- `onState`: add `inputState_ = s;` as the first line. `startInput`: add `inputState_ = SignalState::Connecting;` as the first line.
- In `tick`, replace the conversion line `lastConverted_ = conv_.toLinear(*f.tex, f.layout, f.fullRange);` with:
```cpp
      auto lin = conv_.toLinear(*f.tex, f.layout, f.fullRange);
      lastConverted_ = (lin->width == width_ && lin->height == height_) ? lin : conv_.fit(*lin, width_, height_);
```
- Replace everything in `tick` from `bool anyCpu = false;` up to and including the `for (auto& s : outputs_) { if (s.out) s.out->submit(out); }` loop with:
```cpp
  // One frame (and one readback) per distinct output size this tick (spec §4.3).
  struct Variant {
    TextureRef tex;
    std::shared_ptr<const CpuImage> cpu;
  };
  std::map<std::pair<uint32_t, uint32_t>, Variant> variants;
  auto variant = [&](const OutputConfig& c, bool needsCpu) -> Variant& {
    const uint32_t w = c.width > 0 ? static_cast<uint32_t>(c.width) : frame->width;
    const uint32_t h = c.height > 0 ? static_cast<uint32_t>(c.height) : frame->height;
    auto& v = variants[{w, h}];
    if (!v.tex) v.tex = (w == frame->width && h == frame->height) ? frame : conv_.fit(*frame, w, h);
    if (needsCpu && !v.cpu) v.cpu = std::make_shared<CpuImage>(conv_.readback(*conv_.toBgra8(*v.tex, w, h)));
    return v;
  };
  for (auto& s : outputs_) {
    if (!s.out) continue;
    if (!s.cfg.halfRate) {
      auto& v = variant(s.cfg, s.out->needsCpu());
      s.out->submit(OutFrame{index, sched.rate(), v.tex, v.cpu, audio, 48000, 2, false});
      continue;
    }
    // Half rate: even ticks only, carrying the odd tick's audio so nothing is lost.
    if (index % 2 == 1) {
      s.carryAudio = *audio;
      continue;
    }
    auto both = std::make_shared<std::vector<float>>(std::move(s.carryAudio));
    s.carryAudio.clear();
    both->insert(both->end(), audio->begin(), audio->end());
    auto& v = variant(s.cfg, s.out->needsCpu());
    const Rate half{sched.rate().num, sched.rate().den * 2};
    s.out->submit(OutFrame{index / 2, half, v.tex, v.cpu, both, 48000, 2, false});
  }
```
ponytail: after a tick overrun the index can skip an even tick; the half-rate output then waits one more tick. Fine at the measured overrun rate; revisit if the soak shows half-rate stutter.
- In the stats block at the end of `tick`, after `++freshThisSecond_;` add the stutter check (it uses the arrival of the fresh frame):
```cpp
    if (lastFresh_ && *arrival - *lastFresh_ > std::chrono::milliseconds(100)) lastStutter_ = Clock::now();
    lastFresh_ = *arrival;
```
- In `publishStatus`, read `const bool stutter = lastStutter_.time_since_epoch().count() != 0 && Clock::now() - lastStutter_ < std::chrono::seconds(10);` inside the `statsMutex_` block, and after `state_.set(path("/phase"), ...)` add:
```cpp
  state_.set(path("/input/health"), std::string(inputHealth(inputState_.load(), phase, stutter)));
```

- [ ] **Step 6: Engine — show size, slate at the show size**

`src/engine/engine.cpp`:
- `loadSlate()`: the default becomes `slate_ = conv_.solid(settings_.width, settings_.height, {0.05f, 0.05f, 0.06f, 1.0f});` and a loaded image becomes `slate_ = conv_.fit(*conv_.toLinear(*up, PixelLayout::BGRA8, true), settings_.width, settings_.height);`.
- `apply()`: next to `slateChanged`, add `const bool sizeChanged = !settingsApplied_ || show.settings.width != settings_.width || show.settings.height != settings_.height;`; after `settings_ = show.settings;` call `if (slateChanged || sizeChanged) loadSlate();` (replacing `if (slateChanged) loadSlate();`), set `state_.set("/engine/resolution", resolutionStr(settings_.width, settings_.height));`, and `for (auto& c : cameras_) c->setFormat(settings_.width, settings_.height);` when `sizeChanged`. A camera created in `apply()` gets `cam->setFormat(settings_.width, settings_.height);` before `cam->start();`.
- The constructor also sets `/engine/resolution`.

- [ ] **Step 7: Router — resolution, output format, names in characters**

`src/engine/command_router.cpp`:
- `#include "core/utf8.h"`. In `name()`: `if (n.empty() || utf8Length(n) > 80) throw CmdError("invalid", "name must be 1 to 80 characters");`.
- Add to the anonymous namespace:
```cpp
void formatFrom(const json& f, OutputConfig& o) {
  if (!f.is_object()) throw CmdError("bad-request", "format must be an object");
  o.width = f.value("width", 0);
  o.height = f.value("height", 0);
  o.halfRate = f.value("halfRate", false);
  if (!isOutputSize(o.width, o.height)) throw CmdError("invalid", "output size must be same as show, 1280x720 or 960x540");
}
```
- `addOutput`: after `out.params = ...` add `if (o.contains("format")) formatFrom(o["format"], out);`. `updateOutput`: add `if (args.contains("format")) formatFrom(args["format"], *it);`.
- `setSettings`: add
```cpp
    if (args.contains("resolution")) {
      auto res = parseShowResolution(str(args, "resolution"));
      if (!res) throw CmdError("invalid", "resolution must be 1280x720 or 1920x1080");
      next.settings.width = res->first;
      next.settings.height = res->second;
    }
```
- Protocol table: `setSettings` args become `{rate?, resolution?, holdMs?, slatePath?}`; `addOutput.output` and `updateOutput` take `format?: {width, height, halfRate}`.

- [ ] **Step 8: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "*Show*,*fit*,*Engine*,*CommandRouter*,*inputHealth*"` — Expected: all pass. Then the whole C++ suite once, and the engine and gpu cases once more with `BC_FORCE_WARP=1` (ledger the result; never loosen a bound).

- [ ] **Step 9: Commit**

```bash
git add src tests CMakeLists.txt
git commit -m "feat(engine): show resolution, per-output size and half rate, input health, Unicode name limits"
```

### Task 12: Shell — supervisor, process launcher, WebView2 window and tray

**Files:**
- Create: `src/shell/supervisor.h`, `src/shell/supervisor.cpp`, `src/shell/process_launcher.h`, `src/shell/process_launcher.cpp`, `src/shell/shell_main.cpp`
- Modify: `CMakeLists.txt` (`bc_shell` library, `BreadCamera` WIN32 executable, tests)
- Test: `tests/supervisor_test.cpp`

(The File Structure's `shell.rc`/`app.ico` are dropped: M1 uses the stock application icon; the installer milestone adds branding.)

**Interfaces:**
- Consumes: `randomToken` (Task 11), `toUtf8`/`toWide` (Task 5), ixwebsocket.
- Produces:
  - `class bc::EngineLauncher { virtual bool isRunning() = 0; virtual void launch() = 0; virtual void kill() = 0; }`.
  - `bc::Supervisor(EngineLauncher&, Policy = {})` with `Policy { milliseconds heartbeatTimeout{3000}; milliseconds startupGrace{8000}; int quickRestarts = 5; milliseconds quickWindow{60000}; milliseconds slowInterval{10000}; }`: `void heartbeat(time_point)`, `void poll(time_point)`, `int launches() const`, `bool crashLooping(time_point) const`, `void stopSupervising()`. Thread-safe.
  - `bc::ProcessLauncher(std::filesystem::path exe, std::wstring args)` implementing `EngineLauncher`, plus `void adopt(DWORD pid)`, `DWORD pid() const`.
  - `bc::Discovery { DWORD pid; int port; std::string token; }`, `std::optional<Discovery> bc::readDiscovery(const std::filesystem::path&)`.
  - Executable `BreadCamera.exe` (WIN32).

Shell behaviour (spec §2):
- One shell per user session (`Local\BreadCameraShell`); a second launch brings the first window forward and exits.
- On start it reads `%LOCALAPPDATA%\BreadCamera\engine.json`; if that engine process is alive it **adopts** it (same port and token), else it starts a new engine with a fresh `randomToken()` on port 47810. The engine is **not** in a job object: if the shell dies, the outputs keep running.
- The supervisor restarts the engine when it exits or misses heartbeats for 3 s (8 s grace after each launch); at most 5 quick restarts per 60 s, then one every 10 s.
- Heartbeat = `{"id":"hb","cmd":"ping"}` every second over the control WebSocket; a successful reply counts.
- The UI loads from `<exe dir>\ui\` through the virtual host `https://app.breadcamera/index.html#port=<p>&token=<t>`.
- Closing the window hides it (outputs keep running). Tray menu: "Open BreadCamera" (show) and "Quit…" (show the window and post `{"type":"confirmQuit"}` to the page). When the page posts the string `{"type":"quitConfirmed"}`, the shell stops supervising, sends `shutdown`, waits up to 3 s, kills the engine if needed, removes the tray icon and exits.
- Missing WebView2 runtime: an English message box; the engine keeps running. All shell text is English (spec §6.4).
- **Custom title bar (spec §6.4).** The window has no system caption but keeps its side and bottom resize borders, Snap and the dark frame: `WM_NCCALCSIZE` (wParam TRUE) lets `DefWindowProc` compute the borders and then restores the top edge; `DwmSetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE, TRUE)`. The page draws the title bar; `ICoreWebView2Settings9::put_IsNonClientRegionSupportEnabled(TRUE)` lets its CSS `app-region: drag` move the window. The page posts the strings `{"type":"window","action":"minimize"|"maximize"|"close"}` (maximize toggles; close hides to the tray like `WM_CLOSE`); the shell posts `{"type":"windowState","maximized":true|false}` on every `WM_SIZE`.

- [ ] **Step 1: Write the failing tests**

`tests/supervisor_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <fstream>
#include <thread>

#include "shell/process_launcher.h"
#include "shell/supervisor.h"
#include "support/temp_dir.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
struct FakeLauncher : bc::EngineLauncher {
  bool running = false;
  int launchCount = 0, killCount = 0;
  bool isRunning() override { return running; }
  void launch() override {
    ++launchCount;
    running = true;
  }
  void kill() override {
    ++killCount;
    running = false;
  }
};
}  // namespace

TEST_CASE("Supervisor: launches when nothing runs and relaunches after an exit") {
  FakeLauncher l;
  bc::Supervisor s(l);
  auto t = Clock::now();
  s.poll(t);
  CHECK(l.launchCount == 1);
  l.running = false;  // engine exited
  s.poll(t + 250ms);
  CHECK(l.launchCount == 2);
}

TEST_CASE("Supervisor: missing heartbeats kill and relaunch, but not during the startup grace") {
  FakeLauncher l;
  bc::Supervisor s(l);
  auto t = Clock::now();
  s.poll(t);
  s.poll(t + 5s);  // inside the 8 s grace, no heartbeat yet
  CHECK(l.killCount == 0);
  s.heartbeat(t + 9s);
  s.poll(t + 11s);
  CHECK(l.killCount == 0);
  s.poll(t + 12500ms);  // 3.5 s since the last heartbeat
  CHECK(l.killCount == 1);
  CHECK(l.launchCount == 2);
}

TEST_CASE("Supervisor: a crash loop slows down to one restart every 10 s") {
  FakeLauncher l;
  bc::Supervisor s(l);
  auto t = Clock::now();
  for (int i = 0; i < 5; ++i) {
    s.poll(t + std::chrono::seconds(i));
    l.running = false;
  }
  CHECK(l.launchCount == 5);
  CHECK(s.crashLooping(t + 5s));
  s.poll(t + 6s);
  CHECK(l.launchCount == 5);  // waits
  s.poll(t + 14s);            // 10 s after the last launch
  CHECK(l.launchCount == 6);
}

TEST_CASE("Supervisor: stopSupervising means no more launches") {
  FakeLauncher l;
  bc::Supervisor s(l);
  s.stopSupervising();
  s.poll(Clock::now());
  CHECK(l.launchCount == 0);
}

TEST_CASE("ProcessLauncher: launches, sees, kills and adopts a real process") {
  bc::ProcessLauncher l("C:\\Windows\\System32\\PING.EXE", L"-n 30 127.0.0.1");
  CHECK_FALSE(l.isRunning());
  l.launch();
  CHECK(l.isRunning());
  bc::ProcessLauncher other("C:\\Windows\\System32\\PING.EXE", L"");
  other.adopt(l.pid());
  CHECK(other.isRunning());
  l.kill();
  CHECK_FALSE(l.isRunning());
  std::this_thread::sleep_for(100ms);
  CHECK_FALSE(other.isRunning());
}

TEST_CASE("readDiscovery: parses the engine file and rejects junk") {
  bc::test::TempDir dir;
  auto f = dir.path / "engine.json";
  std::ofstream(f) << R"({"pid":1234,"port":47810,"token":"abcdef0123456789"})";
  auto d = bc::readDiscovery(f);
  REQUIRE(d);
  CHECK(d->pid == 1234);
  CHECK(d->port == 47810);
  CHECK(d->token == "abcdef0123456789");
  std::ofstream(f, std::ios::trunc) << "junk";
  CHECK_FALSE(bc::readDiscovery(f));
  CHECK_FALSE(bc::readDiscovery(dir.path / "missing.json"));
}
```

Add `tests/supervisor_test.cpp` to `bc_tests`, and to `CMakeLists.txt`:
```cmake
# ---- shell ----
add_library(bc_shell STATIC src/shell/supervisor.cpp src/shell/process_launcher.cpp)
target_include_directories(bc_shell PUBLIC src)
target_link_libraries(bc_shell PUBLIC bc_core)

add_executable(BreadCamera WIN32 src/shell/shell_main.cpp)
target_link_libraries(BreadCamera PRIVATE bc_shell bc_engine unofficial::webview2::webview2 WIL::WIL shell32 ole32 user32 dwmapi)
```
Link `bc_shell` into `bc_tests`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Supervisor*,ProcessLauncher*,readDiscovery*"`
Expected: build FAILS — `shell/supervisor.h` not found.

- [ ] **Step 3: Implement the supervisor**

`src/shell/supervisor.h`:
```cpp
#pragma once
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>

namespace bc {
class EngineLauncher {
 public:
  virtual ~EngineLauncher() = default;
  virtual bool isRunning() = 0;
  virtual void launch() = 0;
  virtual void kill() = 0;
};

// Engine restart policy (spec §2). poll() every ~250 ms; heartbeat() on every ping reply.
class Supervisor {
 public:
  using Clock = std::chrono::steady_clock;
  struct Policy {
    std::chrono::milliseconds heartbeatTimeout{3000};
    std::chrono::milliseconds startupGrace{8000};
    int quickRestarts = 5;
    std::chrono::milliseconds quickWindow{60000};
    std::chrono::milliseconds slowInterval{10000};
  };
  explicit Supervisor(EngineLauncher& launcher) : Supervisor(launcher, Policy{}) {}
  Supervisor(EngineLauncher& launcher, Policy policy) : launcher_(launcher), policy_(policy) {}
  void heartbeat(Clock::time_point now);
  void poll(Clock::time_point now);
  int launches() const;
  bool crashLooping(Clock::time_point now) const;
  void stopSupervising();

 private:
  bool canLaunch(Clock::time_point now) const;
  int recentLaunches(Clock::time_point now) const;
  EngineLauncher& launcher_;
  Policy policy_;
  mutable std::mutex m_;
  bool stopped_ = false;
  bool baselineSet_ = false;
  std::optional<Clock::time_point> lastBeat_;
  std::optional<Clock::time_point> lastLaunch_;
  std::deque<Clock::time_point> launchTimes_;
  int launchCount_ = 0;
};
}  // namespace bc
```

`src/shell/supervisor.cpp`:
```cpp
#include "shell/supervisor.h"

namespace bc {
void Supervisor::heartbeat(Clock::time_point now) {
  std::lock_guard l(m_);
  lastBeat_ = now;
}

int Supervisor::recentLaunches(Clock::time_point now) const {
  int n = 0;
  for (auto t : launchTimes_) n += (now - t < policy_.quickWindow) ? 1 : 0;
  return n;
}

bool Supervisor::crashLooping(Clock::time_point now) const {
  std::lock_guard l(m_);
  return recentLaunches(now) >= policy_.quickRestarts;
}

bool Supervisor::canLaunch(Clock::time_point now) const {
  if (recentLaunches(now) < policy_.quickRestarts) return true;
  return !lastLaunch_ || now - *lastLaunch_ >= policy_.slowInterval;
}

void Supervisor::poll(Clock::time_point now) {
  std::lock_guard l(m_);
  if (stopped_) return;
  if (!baselineSet_) {  // an adopted engine gets the same grace as a fresh one
    baselineSet_ = true;
    lastBeat_ = now;
    if (!lastLaunch_) lastLaunch_ = now;
  }
  bool running = launcher_.isRunning();
  const bool inGrace = lastLaunch_ && now - *lastLaunch_ < policy_.startupGrace;
  if (running && !inGrace && lastBeat_ && now - *lastBeat_ > policy_.heartbeatTimeout) {
    launcher_.kill();
    running = false;
  }
  if (running || !canLaunch(now)) return;
  launcher_.launch();
  ++launchCount_;
  lastLaunch_ = now;
  lastBeat_ = now;
  launchTimes_.push_back(now);
  while (launchTimes_.size() > 32) launchTimes_.pop_front();
}

int Supervisor::launches() const {
  std::lock_guard l(m_);
  return launchCount_;
}

void Supervisor::stopSupervising() {
  std::lock_guard l(m_);
  stopped_ = true;
}
}  // namespace bc
```

Trace of the grace test: launch at t (lastLaunch = t); at t+5s in grace; heartbeat t+9s; at t+11s 2 s since beat → fine; at t+12.5s 3.5 s > 3 s and out of grace → kill + relaunch.

- [ ] **Step 4: Implement the process launcher and discovery reader**

`src/shell/process_launcher.h`:
```cpp
#pragma once
#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

#include "shell/supervisor.h"

namespace bc {
struct Discovery {
  DWORD pid = 0;
  int port = 0;
  std::string token;
};
// %LOCALAPPDATA%\BreadCamera\engine.json written by the engine (Task 11).
std::optional<Discovery> readDiscovery(const std::filesystem::path& file);

class ProcessLauncher final : public EngineLauncher {
 public:
  ProcessLauncher(std::filesystem::path exe, std::wstring args) : exe_(std::move(exe)), args_(std::move(args)) {}
  ~ProcessLauncher() override;
  bool isRunning() override;
  void launch() override;
  void kill() override;
  void adopt(DWORD pid);
  DWORD pid() const { return pid_; }

 private:
  std::filesystem::path exe_;
  std::wstring args_;
  HANDLE process_ = nullptr;
  DWORD pid_ = 0;
};
}  // namespace bc
```

`src/shell/process_launcher.cpp`:
```cpp
#include "shell/process_launcher.h"

#include <fstream>

#include <nlohmann/json.hpp>

namespace bc {
std::optional<Discovery> readDiscovery(const std::filesystem::path& file) {
  std::ifstream f(file);
  if (!f) return std::nullopt;
  try {
    auto j = nlohmann::json::parse(f);
    Discovery d{j.at("pid").get<DWORD>(), j.at("port").get<int>(), j.at("token").get<std::string>()};
    if (d.port <= 0 || d.port > 65535 || d.token.size() < 16) return std::nullopt;
    return d;
  } catch (...) {
    return std::nullopt;
  }
}

ProcessLauncher::~ProcessLauncher() {
  if (process_) CloseHandle(process_);  // never kills: the engine outlives the shell
}

bool ProcessLauncher::isRunning() {
  return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

void ProcessLauncher::launch() {
  if (process_) {
    CloseHandle(process_);
    process_ = nullptr;
  }
  std::wstring cmd = L"\"" + exe_.wstring() + L"\" " + args_;
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe_.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      exe_.parent_path().c_str(), &si, &pi)) {
    return;  // stays not-running; the supervisor retries with its policy
  }
  CloseHandle(pi.hThread);
  process_ = pi.hProcess;
  pid_ = pi.dwProcessId;
}

void ProcessLauncher::kill() {
  if (!process_) return;
  TerminateProcess(process_, 1);
  WaitForSingleObject(process_, 2000);
}

void ProcessLauncher::adopt(DWORD pid) {
  HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return;
  if (process_) CloseHandle(process_);
  process_ = h;
  pid_ = pid;
}
}  // namespace bc
```

- [ ] **Step 5: Run the unit tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Supervisor*,ProcessLauncher*,readDiscovery*"`
Expected: 6 cases pass.

- [ ] **Step 6: Write the shell executable**

`src/shell/shell_main.cpp`:
```cpp
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>

#include <WebView2.h>
#include <dwmapi.h>
#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>
#include <wil/com.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/utf8.h"
#include "engine/control_server.h"
#include "shell/process_launcher.h"
#include "shell/supervisor.h"

using Microsoft::WRL::Callback;

namespace {
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_QUIT_CONFIRMED = WM_APP + 2;
constexpr UINT ID_OPEN = 1;
constexpr UINT ID_QUIT = 2;
constexpr wchar_t kClass[] = L"BreadCameraShell";

struct App {
  HWND hwnd = nullptr;
  wil::com_ptr<ICoreWebView2Controller> controller;
  wil::com_ptr<ICoreWebView2> webview;
  NOTIFYICONDATAW tray{};
  std::unique_ptr<bc::ProcessLauncher> launcher;
  std::unique_ptr<bc::Supervisor> supervisor;
  ix::WebSocket ws;
  std::atomic<bool> quitting{false};
  std::thread worker;
  int port = 47810;
  std::string token;
  std::filesystem::path exeDir;
} g;

std::filesystem::path localAppData() {
  PWSTR p = nullptr;
  SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p);
  std::filesystem::path out(p);
  CoTaskMemFree(p);
  return out / L"BreadCamera";
}

void showWindow() {
  ShowWindow(g.hwnd, SW_SHOW);
  ShowWindow(g.hwnd, SW_RESTORE);
  SetForegroundWindow(g.hwnd);
}

void quitAll() {
  g.quitting = true;
  g.supervisor->stopSupervising();
  g.ws.send(R"({"id":"quit","cmd":"shutdown"})");
  for (int i = 0; i < 30 && g.launcher->isRunning(); ++i) Sleep(100);
  if (g.launcher->isRunning()) g.launcher->kill();
  Shell_NotifyIconW(NIM_DELETE, &g.tray);
  DestroyWindow(g.hwnd);
}

void showTrayMenu() {
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, ID_OPEN, L"Open BreadCamera");
  AppendMenuW(menu, MF_STRING, ID_QUIT, L"Quit…");
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g.hwnd);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g.hwnd, nullptr);
  DestroyMenu(menu);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_NCCALCSIZE:
      if (wp == TRUE) {
        // No system caption: the page draws the title bar (spec §6.4). Keep the side and
        // bottom resize borders; when maximised, keep the top inside the screen.
        auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
        const LONG top = p->rgrc[0].top;
        const LRESULT res = DefWindowProcW(hwnd, msg, wp, lp);
        p->rgrc[0].top = top + (IsZoomed(hwnd) ? GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER) : 0);
        return res;
      }
      break;
    case WM_SIZE:
      if (g.controller) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        g.controller->put_Bounds(rc);
      }
      if (g.webview) {
        g.webview->PostWebMessageAsJson(IsZoomed(hwnd) ? L"{\"type\":\"windowState\",\"maximized\":true}"
                                                       : L"{\"type\":\"windowState\",\"maximized\":false}");
      }
      return 0;
    case WM_CLOSE:
      ShowWindow(hwnd, SW_HIDE);  // outputs keep running; quit is explicit (spec §2)
      return 0;
    case WM_TRAY:
      if (LOWORD(lp) == WM_RBUTTONUP) showTrayMenu();
      if (LOWORD(lp) == WM_LBUTTONDBLCLK) showWindow();
      return 0;
    case WM_COMMAND:
      if (LOWORD(wp) == ID_OPEN) showWindow();
      if (LOWORD(wp) == ID_QUIT) {
        showWindow();
        if (g.webview) g.webview->PostWebMessageAsJson(L"{\"type\":\"confirmQuit\"}");
        else quitAll();  // no UI to confirm in
      }
      return 0;
    case WM_QUIT_CONFIRMED:
      quitAll();
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

void runtimeMissing() {
  MessageBoxW(g.hwnd,
              L"The Microsoft Edge WebView2 Runtime is missing, so the window cannot open. "
              L"The engine is still running and every output is unaffected.",
              L"BreadCamera", MB_ICONWARNING);
}

void startWebView() {
  const auto userData = localAppData() / L"WebView2";
  const auto uiDir = g.exeDir / L"ui";
  const std::wstring url = L"https://app.breadcamera/index.html#port=" + std::to_wstring(g.port) +
                           L"&token=" + bc::toWide(g.token);
  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, userData.c_str(), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [uiDir, url](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr) || !env) {
              runtimeMissing();
              return S_OK;
            }
            env->CreateCoreWebView2Controller(
                g.hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [uiDir, url](HRESULT hr2, ICoreWebView2Controller* c) -> HRESULT {
                              if (FAILED(hr2) || !c) {
                                runtimeMissing();
                                return S_OK;
                              }
                              g.controller = c;
                              g.controller->get_CoreWebView2(&g.webview);
                              wil::com_ptr<ICoreWebView2Settings> settings;
                              g.webview->get_Settings(&settings);
                              if (auto s9 = settings.try_query<ICoreWebView2Settings9>()) {
                                s9->put_IsNonClientRegionSupportEnabled(TRUE);  // CSS app-region: drag
                              }
                              RECT rc;
                              GetClientRect(g.hwnd, &rc);
                              g.controller->put_Bounds(rc);
                              if (auto wv3 = g.webview.try_query<ICoreWebView2_3>()) {
                                wv3->SetVirtualHostNameToFolderMapping(L"app.breadcamera", uiDir.c_str(),
                                                                       COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                              }
                              EventRegistrationToken tok;
                              g.webview->add_WebMessageReceived(
                                  Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                      [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        wil::unique_cotaskmem_string msg;
                                        if (FAILED(args->TryGetWebMessageAsString(&msg))) return S_OK;
                                        const std::wstring text(msg.get());
                                        if (text.find(L"quitConfirmed") != std::wstring::npos) {
                                          PostMessageW(g.hwnd, WM_QUIT_CONFIRMED, 0, 0);
                                        } else if (text.find(L"\"window\"") != std::wstring::npos) {
                                          if (text.find(L"minimize") != std::wstring::npos) ShowWindow(g.hwnd, SW_MINIMIZE);
                                          else if (text.find(L"maximize") != std::wstring::npos)
                                            ShowWindow(g.hwnd, IsZoomed(g.hwnd) ? SW_RESTORE : SW_MAXIMIZE);
                                          else if (text.find(L"close") != std::wstring::npos)
                                            PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
                                        }
                                        return S_OK;
                                      })
                                      .Get(),
                                  &tok);
                              g.webview->Navigate(url.c_str());
                              return S_OK;
                            })
                            .Get());
            return S_OK;
          })
          .Get());
}

void startEngineSupervision() {
  const auto discovery = bc::readDiscovery(localAppData() / L"engine.json");
  g.launcher = std::make_unique<bc::ProcessLauncher>(g.exeDir / L"breadcamera-engine.exe", L"");
  if (discovery) g.launcher->adopt(discovery->pid);
  if (discovery && g.launcher->isRunning()) {
    g.port = discovery->port;
    g.token = discovery->token;
  } else {
    g.token = bc::randomToken();
  }
  g.launcher = std::make_unique<bc::ProcessLauncher>(
      g.exeDir / L"breadcamera-engine.exe",
      L"--port " + std::to_wstring(g.port) + L" --token " + bc::toWide(g.token));
  if (discovery && discovery->token == g.token) g.launcher->adopt(discovery->pid);
  g.supervisor = std::make_unique<bc::Supervisor>(*g.launcher);

  g.ws.setUrl("ws://127.0.0.1:" + std::to_string(g.port) + "/?token=" + g.token);
  g.ws.setMinWaitBetweenReconnectionRetries(500);
  g.ws.setMaxWaitBetweenReconnectionRetries(1000);
  g.ws.setOnMessageCallback([](const ix::WebSocketMessagePtr& m) {
    if (m->type != ix::WebSocketMessageType::Message) return;
    try {
      auto j = nlohmann::json::parse(m->str);
      if (j.value("id", nlohmann::json()) == "hb" && j.value("ok", false)) {
        g.supervisor->heartbeat(std::chrono::steady_clock::now());
      }
    } catch (...) {
    }
  });
  g.ws.start();
  g.worker = std::thread([] {
    int n = 0;
    while (!g.quitting) {
      g.supervisor->poll(std::chrono::steady_clock::now());
      if (++n % 4 == 0) g.ws.send(R"({"id":"hb","cmd":"ping"})");
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  });
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\BreadCameraShell");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND other = FindWindowW(kClass, nullptr)) {
      ShowWindow(other, SW_SHOW);
      SetForegroundWindow(other);
    }
    return 0;
  }
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ix::initNetSystem();
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  g.exeDir = std::filesystem::path(path).parent_path();

  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = inst;
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
  g.hwnd = CreateWindowExW(0, kClass, L"BreadCamera", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1600, 940,
                           nullptr, nullptr, inst, nullptr);
  const BOOL dark = TRUE;
  DwmSetWindowAttribute(g.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
  SetWindowPos(g.hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);  // apply WM_NCCALCSIZE
  ShowWindow(g.hwnd, show);

  g.tray.cbSize = sizeof(g.tray);
  g.tray.hWnd = g.hwnd;
  g.tray.uID = 1;
  g.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  g.tray.uCallbackMessage = WM_TRAY;
  g.tray.hIcon = wc.hIcon;
  wcscpy_s(g.tray.szTip, L"BreadCamera");
  Shell_NotifyIconW(NIM_ADD, &g.tray);

  startEngineSupervision();
  startWebView();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  g.quitting = true;
  if (g.worker.joinable()) g.worker.join();
  g.ws.stop();
  CloseHandle(single);
  CoUninitialize();
  return 0;
}
```

Note on `startEngineSupervision`: the first `ProcessLauncher` only probes whether the discovered PID is alive; the second is the real launcher with the right arguments and re-adopts the same PID. Keep both steps.

- [ ] **Step 7: Build and run the shell smoke test**

Run `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1`, then in PowerShell:
```powershell
$exe = Resolve-Path .\build\release\BreadCamera.exe
Start-Process $exe
Start-Sleep 4
$e1 = Get-Process breadcamera-engine -ErrorAction Stop
Stop-Process -Id $e1.Id -Force
Start-Sleep 3
$e2 = Get-Process breadcamera-engine -ErrorAction Stop
"engine restarted: $($e1.Id) -> $($e2.Id)"
Get-Process BreadCamera | Stop-Process -Force   # shell gone; the engine must survive
Start-Sleep 2
"engine still alive after shell death: $([bool](Get-Process -Id $e2.Id -ErrorAction SilentlyContinue))"
Start-Process $exe                                # a new shell adopts the running engine
Start-Sleep 4
"adopted (same pid): $((Get-Process breadcamera-engine).Id -eq $e2.Id)"
Get-Process BreadCamera, breadcamera-engine | Stop-Process -Force
```
Expected: three lines saying a different PID after the kill, `True`, `True`. The window shows a WebView2 "not found" page because `ui\` does not exist until Task 13 — that is expected. Paste the output into the report.

- [ ] **Step 8: Commit**

```bash
git add src/shell tests CMakeLists.txt
git commit -m "feat(shell): WebView2 shell with tray, engine adoption and restart supervisor"
```

### Task 13: UI — native window shell: title bar, toolbar, Multiview, Inspector, status bar

Rewritten 2026-10-08 after the UI mockup review. The reference is `docs/design/mockups/index.html` (approved) and `docs/design/multiview.md`; spec §6.4 is binding. English only. Effects, calibration and Settings arrive in M2, M3 and M5; this task builds the window they plug into.

**Files:**
- Create: `ui/package.json`, `ui/.npmrc`, `ui/tsconfig.json`, `ui/vite.config.ts`, `ui/index.html`, `ui/src/main.tsx`, `ui/src/app.css`, `ui/src/App.tsx`, `ui/src/bridge.ts`
- Create: `ui/src/api/protocol.ts`, `ui/src/api/patch.ts`, `ui/src/api/client.ts`, `ui/src/api/useEngine.ts`, `ui/src/preview/h264.ts`, `ui/src/preview/PreviewCanvas.tsx`
- Create: `ui/src/state/marks.ts`, `ui/src/state/labels.ts`
- Create: `ui/src/components/Icon.tsx`, `ui/src/components/ErrorBlock.tsx`, `ui/src/components/Modal.tsx`, `ui/src/components/Menu.tsx`, `ui/src/components/Prop.tsx`
- Create: `ui/src/shell/TitleBar.tsx`, `ui/src/shell/Toolbar.tsx`, `ui/src/shell/StatusBar.tsx`
- Create: `ui/src/multiview/Strip.tsx`, `ui/src/multiview/Multiview.tsx`, `ui/src/inspector/Inspector.tsx`
- Create: `ui/src/dialogs/AddCameraDialog.tsx`, `ui/src/dialogs/AddOutputDialog.tsx`, `ui/src/dialogs/ConfirmDialog.tsx`, `ui/src/dialogs/QuitDialog.tsx`
- Create: `cmake/CopyUi.cmake`
- Modify: `CMakeLists.txt` (copy `ui/dist` next to `BreadCamera.exe`)
- Test: `ui/src/test/setup.ts`, `ui/src/test/fakeWs.ts`, `ui/src/api/patch.test.ts`, `ui/src/api/protocol.test.ts`, `ui/src/preview/h264.test.ts`, `ui/src/api/client.test.ts`, `ui/src/state/marks.test.ts`, `ui/src/Workspace.test.tsx`

**Interfaces:**
- Consumes: the wire protocol of Tasks 11 and 11A (responses, `snapshot`/`patch`/`show`/`stats`, commands incl. `format` and `resolution`); Task 10's binary preview message; state paths of Tasks 4, 9 and 11A (`/engine/gpu|rate|resolution|overruns|warnings/*`, `/cameras/<id>/name|phase|tickError|input/state|input/error|input/health|outputs/<oid>/kind|state|error`); shell messages of Task 12 (`{"type":"confirmQuit"}` and `{"type":"windowState","maximized":bool}` in; the strings `{"type":"quitConfirmed"}` and `{"type":"window","action":...}` out).
- Produces (later milestones extend these): `applyPatch`, `parsePreviewMessage`, `codecFromSps`, `class EngineClient`, `useEngine(client)`; `Workspace({client})` (the whole window below the engine connection); `Strip`, `Inspector({client, cameraId, onRemoved})` with a `children` slot between Input and Outputs where M2 puts Effects; `Modal`, `ConfirmDialog`, `Menu`, `Prop`, `Icon`, `ErrorBlock`; `inputCode(kind)`, `outputCode(kind)`, `outputName(kind)`, `rateLabel(rate)`; `nextMarks`, `holdLeft`, `clock`.

Commands (bash, in `ui/`): `PATH="/c/Program Files/nodejs:$PATH" NODE_AUTH_TOKEN="$(gh auth token)" npm install` once (commit `package-lock.json`), then `PATH="/c/Program Files/nodejs:$PATH" npm test` and `npm run build`.

UI rules (spec §6.4): native pro-tool look — Segoe UI Variable 12 px, JetBrains Mono for numbers, 26 px controls, label-left property rows, the `--bm-*` tokens from `@bmsolutionsvn/design/bm.css`. English only; names as given (Unicode). Output signal states are Ready/Connecting/Live/Offline pills; input health is the filled chip (green receiving, yellow degraded, red down, grey offline). Errors are persistent blocks (`role="alert"`), never toasts. Every destructive action goes through `ConfirmDialog` whose button names the action; buttons: secondary left, primary right. Nothing on a Multiview tile changes an output; a click selects. No MUI: plain React and CSS.

- [ ] **Step 1: Create the package**

`ui/package.json`:
```json
{
  "name": "breadcamera-ui",
  "private": true,
  "version": "0.1.0",
  "type": "module",
  "scripts": {
    "dev": "vite",
    "build": "tsc --noEmit && vite build",
    "test": "vitest run"
  },
  "dependencies": {
    "@bmsolutionsvn/design": "^1.2.0",
    "react": "^18.3.1",
    "react-dom": "^18.3.1"
  },
  "devDependencies": {
    "@testing-library/dom": "^10.4.0",
    "@testing-library/jest-dom": "^6.6.3",
    "@testing-library/react": "^16.1.0",
    "@types/react": "^18.3.12",
    "@types/react-dom": "^18.3.1",
    "@vitejs/plugin-react": "^4.3.4",
    "jsdom": "^25.0.1",
    "typescript": "^5.6.3",
    "vite": "^6.0.3",
    "vitest": "^2.1.8"
  }
}
```

`ui/.npmrc`:
```
@bmsolutionsvn:registry=https://npm.pkg.github.com
//npm.pkg.github.com/:_authToken=${NODE_AUTH_TOKEN}
```

`ui/tsconfig.json`:
```json
{
  "compilerOptions": {
    "target": "ES2022",
    "lib": ["ES2022", "DOM", "DOM.Iterable"],
    "module": "ESNext",
    "moduleResolution": "Bundler",
    "jsx": "react-jsx",
    "strict": true,
    "noUnusedLocals": true,
    "skipLibCheck": true,
    "types": ["vitest/globals"]
  },
  "include": ["src"]
}
```

`ui/vite.config.ts`:
```ts
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  base: './',
  plugins: [react()],
  build: { outDir: 'dist', emptyOutDir: true },
  test: { environment: 'jsdom', globals: true, setupFiles: ['./src/test/setup.ts'] },
});
```


`ui/index.html`:
```html
<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>BreadCamera</title>
  </head>
  <body>
    <div id="root"></div>
    <script type="module" src="/src/main.tsx"></script>
  </body>
</html>
```

Run the install command above. Expected: `node_modules/` and `package-lock.json` created; `@bmsolutionsvn/design` resolves from GitHub Packages.

- [ ] **Step 2: Write the failing tests**

`ui/src/test/fakeWs.ts`:
```ts
// Minimal stand-in for the browser WebSocket used by EngineClient tests.
export class FakeWs {
  static all: FakeWs[] = [];
  static get last(): FakeWs {
    return FakeWs.all[FakeWs.all.length - 1];
  }
  url: string;
  binaryType = 'blob';
  readyState = 0;
  sent: string[] = [];
  onopen: ((e: unknown) => void) | null = null;
  onmessage: ((e: { data: unknown }) => void) | null = null;
  onclose: ((e: unknown) => void) | null = null;
  constructor(url: string) {
    this.url = url;
    FakeWs.all.push(this);
  }
  send(d: string) {
    this.sent.push(d);
  }
  close() {
    this.readyState = 3;
    this.onclose?.({});
  }
  open() {
    this.readyState = 1;
    this.onopen?.({});
  }
  receive(obj: unknown) {
    this.onmessage?.({ data: JSON.stringify(obj) });
  }
  receiveBinary(bytes: number[]) {
    this.onmessage?.({ data: new Uint8Array(bytes).buffer });
  }
  lastRequest(): { id: number; cmd: string; args: Record<string, unknown> } {
    return JSON.parse(this.sent[this.sent.length - 1]);
  }
}
```

`ui/src/api/patch.test.ts`:
```ts
import { applyPatch } from './patch';

test('replace creates missing parents', () => {
  const out = applyPatch({}, [{ op: 'replace', path: '/cameras/abc/input/state', value: 'live' }]);
  expect(out).toEqual({ cameras: { abc: { input: { state: 'live' } } } });
});

test('remove deletes, and removing a missing path is a no-op', () => {
  const s = { cameras: { a: { name: 'A' }, b: { name: 'B' } } };
  const out = applyPatch(s, [
    { op: 'remove', path: '/cameras/a' },
    { op: 'remove', path: '/cameras/zzz' },
  ]);
  expect(out).toEqual({ cameras: { b: { name: 'B' } } });
  expect(s.cameras.a).toBeDefined(); // input not mutated
});

test('escaped pointer tokens', () => {
  const out = applyPatch({}, [{ op: 'replace', path: '/a~1b/c~0d', value: 1 }]);
  expect(out).toEqual({ 'a/b': { 'c~d': 1 } });
});
```

`ui/src/api/protocol.test.ts`:
```ts
import { parsePreviewMessage } from './protocol';

test('parses the preview header written by the engine', () => {
  const bytes = [1, 1, 4, 0, 99, 97, 109, 49, 0xd2, 0x04, 0, 0, 0, 0, 0, 0, 9, 9];
  const m = parsePreviewMessage(new Uint8Array(bytes).buffer);
  expect(m.cameraId).toBe('cam1');
  expect(m.keyframe).toBe(true);
  expect(m.ptsUs).toBe(1234);
  expect(Array.from(m.data)).toEqual([9, 9]);
});

test('rejects unknown versions and short buffers', () => {
  expect(() => parsePreviewMessage(new Uint8Array([2, 0, 0, 0]).buffer)).toThrow();
  expect(() => parsePreviewMessage(new Uint8Array([1, 0, 9, 0, 1]).buffer)).toThrow();
});
```

`ui/src/preview/h264.test.ts`:
```ts
import { codecFromSps } from './h264';

test('builds the avc1 codec string from the SPS', () => {
  const au = new Uint8Array([0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1e, 0xaa, 0, 0, 0, 1, 0x68, 0xce]);
  expect(codecFromSps(au)).toBe('avc1.42C01E');
});

test('returns null without an SPS', () => {
  expect(codecFromSps(new Uint8Array([0, 0, 0, 1, 0x65, 0x88]))).toBeNull();
});
```

`ui/src/api/client.test.ts`:
```ts
import { FakeWs } from '../test/fakeWs';
import { EngineClient, EngineError } from './client';

function make() {
  FakeWs.all = [];
  const c = new EngineClient('ws://x/?token=t', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  return c;
}

afterEach(() => vi.useRealTimers());

test('snapshot then patch build the state', () => {
  const c = make();
  FakeWs.last.receive({ type: 'snapshot', state: { cameras: { a: { name: 'A' } } }, show: { cameras: [] } });
  FakeWs.last.receive({ type: 'patch', ops: [{ op: 'replace', path: '/cameras/a/phase', value: 'live' }] });
  expect(c.getSnapshot().state.cameras?.a.phase).toBe('live');
});

test('requests resolve by id and server errors reject with the code', async () => {
  const c = make();
  const p = c.request('addCamera', { name: 'x' });
  const req = FakeWs.last.lastRequest();
  expect(req.cmd).toBe('addCamera');
  FakeWs.last.receive({ id: req.id, ok: true, result: { id: 'abc' } });
  await expect(p).resolves.toEqual({ id: 'abc' });
  const bad = c.request('removeCamera', { id: 'nope' });
  FakeWs.last.receive({ id: FakeWs.last.lastRequest().id, ok: false, error: { code: 'not-found', message: 'no camera nope' } });
  await expect(bad).rejects.toBeInstanceOf(EngineError);
  await expect(bad).rejects.toMatchObject({ code: 'not-found' });
});

test('after an engine restart the new snapshot replaces the old state, and previews resubscribe', async () => {
  vi.useFakeTimers();
  const c = make();
  c.setPreviewSubscription(['a'], 'small');
  FakeWs.last.receive({ type: 'snapshot', state: { cameras: { a: { name: 'A' }, b: { name: 'B' } } }, show: { cameras: [] } });
  const pending = c.request('ping');
  FakeWs.last.close();
  await expect(pending).rejects.toThrow();
  expect(c.getSnapshot().status).toBe('closed');
  vi.advanceTimersByTime(1000);
  const second = FakeWs.last;
  expect(FakeWs.all.length).toBe(2);
  second.open();
  expect(second.sent.some((s) => JSON.parse(s).cmd === 'subscribePreview')).toBe(true);
  second.receive({ type: 'snapshot', state: { cameras: { a: { name: 'A' } } }, show: { cameras: [] } });
  expect(Object.keys(c.getSnapshot().state.cameras ?? {})).toEqual(['a']);
  c.close();
});

test('binary preview messages reach the camera listener', () => {
  const c = make();
  const got: string[] = [];
  c.onPreview('cam1', (m) => got.push(m.cameraId));
  FakeWs.last.receiveBinary([1, 0, 4, 0, 99, 97, 109, 49, 0, 0, 0, 0, 0, 0, 0, 0, 1]);
  expect(got).toEqual(['cam1']);
});
```


`ui/src/state/marks.test.ts`:
```ts
import { clock, holdLeft, nextMarks } from './marks';

test('holding starts a countdown that clears when the picture returns', () => {
  let m = nextMarks(undefined, { name: 'A', phase: 'holding' }, 1000);
  expect(holdLeft(m, 5000, 1000)).toBe('00:05');
  expect(holdLeft(m, 5000, 2600)).toBe('00:04');
  m = nextMarks(m, { name: 'A', phase: 'live', input: { state: 'live', error: null, health: 'receiving' } }, 3000);
  expect(holdLeft(m, 5000, 3000)).toBeNull();
});

test('a fault that recovers on its own leaves a mark with the time', () => {
  let m = nextMarks(undefined, { name: 'A', phase: 'live', outputs: { o: { kind: 'ndi', state: 'offline', error: { code: 'x', message: 'y' } } } }, 10);
  expect(m.recoveredAt).toBeUndefined();
  m = nextMarks(m, { name: 'A', phase: 'live', outputs: { o: { kind: 'ndi', state: 'live', error: null } } }, 20);
  expect(m.recoveredAt).toBe(20);
  expect(clock(new Date(2026, 9, 8, 20, 14).getTime())).toBe('20:14');
});
```

`ui/src/Workspace.test.tsx`:
```tsx
import { fireEvent, render, screen, within } from '@testing-library/react';

import { EngineClient } from './api/client';
import { Workspace } from './App';
import { FakeWs } from './test/fakeWs';

const show = {
  schemaVersion: 1,
  settings: { rate: '60000/1001', resolution: '1920x1080', holdMs: 5000, slatePath: '' },
  cameras: [
    {
      id: 'a',
      name: 'Sân khấu trái',
      input: { kind: 'srt', params: { mode: 'caller', url: 'srt://1.2.3.4:9000', latencyMs: 120 } },
      outputs: [
        { id: 'o1', kind: 'ndi', keyed: false, enabled: true, params: { name: 'BreadCamera - Sân khấu trái' }, format: { width: 0, height: 0, halfRate: false } },
        { id: 'o2', kind: 'fullscreen', keyed: false, enabled: true, params: { monitor: 1 }, format: { width: 0, height: 0, halfRate: false } },
      ],
    },
    { id: 'b', name: 'Khách mời', input: { kind: 'srt', params: { mode: 'listener', port: 9001 } }, outputs: [] },
  ],
};
const state = {
  engine: { gpu: 'NVIDIA GeForce RTX 4070', rate: '60000/1001', resolution: '1920x1080', overruns: 0 },
  cameras: {
    a: {
      name: 'Sân khấu trái',
      phase: 'live',
      input: { state: 'live', error: null, health: 'receiving' },
      outputs: {
        o1: { kind: 'ndi', state: 'live', error: null },
        o2: { kind: 'fullscreen', state: 'offline', error: { code: 'output-failed', message: 'monitor 2 is not connected' } },
      },
    },
    b: { name: 'Khách mời', phase: 'slate', input: { state: 'offline', error: { code: 'input-failed', message: 'cannot open srt://:9001' }, health: 'down' } },
  },
};

function client(s: unknown = state, sh: unknown = show) {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({ type: 'snapshot', state: s, show: sh });
  return c;
}

test('strips show input health, names and every output state', () => {
  render(<Workspace client={client()} />);
  expect(screen.getByTitle('SRT input: receiving')).toHaveTextContent('SRT');
  expect(screen.getByTitle('NDI output: Live')).toHaveTextContent('NDI');
  expect(screen.getByTitle('Fullscreen: failed, monitor 2 is not connected')).toBeInTheDocument();
  expect(screen.getByTitle('SRT input: down, showing the slate')).toHaveTextContent('SLATE');
  expect(screen.getAllByText('Khách mời').length).toBeGreaterThan(0);
});

test('the first camera is selected; arrow keys and clicks move the Inspector', () => {
  render(<Workspace client={client()} />);
  const insp = screen.getByRole('complementary', { name: 'Inspector' });
  expect(within(insp).getByText('Sân khấu trái')).toBeInTheDocument();
  expect(within(insp).getByRole('alert')).toHaveTextContent('monitor 2 is not connected');
  fireEvent.keyDown(screen.getByRole('listbox', { name: 'Multiview' }), { key: 'ArrowRight' });
  expect(within(insp).getByText('Khách mời')).toBeInTheDocument();
  expect(within(insp).getByRole('alert')).toHaveTextContent('cannot open srt://:9001');
  fireEvent.click(screen.getByRole('option', { name: /CAM 1/ }));
  expect(within(insp).getByText('Sân khấu trái')).toBeInTheDocument();
});

test('an empty show points to Add camera', () => {
  render(<Workspace client={client({}, { ...show, cameras: [] })} />);
  expect(screen.getByText('No cameras yet')).toBeInTheDocument();
});

test('Add output sends the chosen size and half rate', () => {
  render(<Workspace client={client()} />);
  fireEvent.click(within(screen.getByRole('complementary', { name: 'Inspector' })).getByRole('button', { name: 'Add output' }));
  const dlg = screen.getByRole('dialog', { name: 'Add output to CAM 1' });
  fireEvent.change(within(dlg).getByLabelText('Resolution'), { target: { value: '1280x720' } });
  fireEvent.change(within(dlg).getByLabelText('Frame rate'), { target: { value: 'half' } });
  fireEvent.click(within(dlg).getByRole('button', { name: 'Add output' }));
  const req = FakeWs.last.lastRequest();
  expect(req.cmd).toBe('addOutput');
  expect((req.args.output as { format: unknown }).format).toEqual({ width: 1280, height: 720, halfRate: true });
});

test('Stop all outputs asks first, with a verb button', () => {
  render(<Workspace client={client()} />);
  fireEvent.click(screen.getByRole('button', { name: 'Stop all outputs…' }));
  const dlg = screen.getByRole('dialog', { name: 'Stop all outputs?' });
  fireEvent.click(within(dlg).getByRole('button', { name: 'Stop all outputs' }));
  expect(FakeWs.last.lastRequest().cmd).toBe('stopAllOutputs');
});

test('window buttons post to the shell', () => {
  const postMessage = vi.fn();
  (window as unknown as { chrome: unknown }).chrome = { webview: { postMessage, addEventListener() {}, removeEventListener() {} } };
  render(<Workspace client={client()} />);
  fireEvent.click(screen.getByRole('button', { name: 'Minimise' }));
  expect(postMessage).toHaveBeenCalledWith('{"type":"window","action":"minimize"}');
  delete (window as unknown as { chrome?: unknown }).chrome;
});

test('losing the engine shows the restart block and every pill goes Offline', () => {
  const c = client();
  render(<Workspace client={c} />);
  FakeWs.last.close();
  expect(screen.getByRole('status', { name: 'Engine restarting' })).toBeInTheDocument();
  expect(screen.getByTitle('NDI output: Offline')).toBeInTheDocument();
  c.close();
});
```

`ui/src/test/setup.ts`:
```ts
import '@testing-library/jest-dom/vitest';
```

Run: `PATH="/c/Program Files/nodejs:$PATH" npm test` — Expected: FAIL, modules not found.

- [ ] **Step 3: Engine protocol, patch, H.264 and the client**

`ui/src/api/protocol.ts`:
```ts
export type SignalState = 'ready' | 'connecting' | 'live' | 'offline';
export type Phase = 'nosignal' | 'live' | 'holding' | 'slate';
export type InputHealth = 'receiving' | 'degraded' | 'down' | 'offline';
export interface ErrorInfo {
  code: string;
  message: string;
}
export interface OutputState {
  kind: string;
  state: SignalState;
  error: ErrorInfo | null;
}
export interface CameraState {
  name: string;
  phase?: Phase;
  input?: { state: SignalState; error: ErrorInfo | null; health?: InputHealth };
  outputs?: Record<string, OutputState>;
  tickError?: string;
}
export interface StateTree {
  engine?: {
    gpu?: string;
    rate?: string;
    resolution?: string;
    overruns?: number;
    previewEncoder?: string;
    warnings?: Record<string, string>;
  };
  cameras?: Record<string, CameraState>;
}
export interface OutputFormat {
  width: number;
  height: number;
  halfRate: boolean;
}
export interface OutputConfig {
  id: string;
  kind: string;
  keyed: boolean;
  enabled: boolean;
  params: Record<string, unknown>;
  format?: OutputFormat;
}
export interface CameraConfig {
  id: string;
  name: string;
  input: { kind: string; params: Record<string, unknown> };
  outputs: OutputConfig[];
}
export interface ShowJson {
  schemaVersion?: number;
  settings?: { rate: string; resolution?: string; holdMs: number; slatePath: string };
  cameras: CameraConfig[];
}
export interface PatchOp {
  op: 'replace' | 'remove';
  path: string;
  value?: unknown;
}
export interface Stats {
  engine: { overruns: number };
  cameras: Record<string, { fps: number; drops: number; repeats: number; latencyMs: number }>;
}
export interface PreviewMessage {
  cameraId: string;
  keyframe: boolean;
  ptsUs: number;
  data: Uint8Array;
}
export type PreviewSize = 'small' | 'large';

// Binary layout from the engine (spec §6.3).
export function parsePreviewMessage(buf: ArrayBuffer): PreviewMessage {
  const v = new DataView(buf);
  if (buf.byteLength < 4 || v.getUint8(0) !== 1) throw new Error('unknown preview message');
  const idLen = v.getUint16(2, true);
  const ptsAt = 4 + idLen;
  if (buf.byteLength < ptsAt + 8) throw new Error('short preview message');
  return {
    cameraId: new TextDecoder().decode(new Uint8Array(buf, 4, idLen)),
    keyframe: (v.getUint8(1) & 1) === 1,
    ptsUs: Number(v.getBigInt64(ptsAt, true)),
    data: new Uint8Array(buf, ptsAt + 8),
  };
}
```

`ui/src/api/patch.ts`:
```ts
import type { PatchOp } from './protocol';

// Our protocol: "replace" creates missing parents; removing a missing path is a no-op.
export function applyPatch<T extends object>(state: T, ops: PatchOp[]): T {
  const out = structuredClone(state) as Record<string, unknown>;
  for (const op of ops) {
    const keys = op.path.split('/').slice(1).map((k) => k.replace(/~1/g, '/').replace(/~0/g, '~'));
    if (keys.length === 0) continue;
    let node = out;
    let ok = true;
    for (const k of keys.slice(0, -1)) {
      const next = node[k];
      if (typeof next !== 'object' || next === null) {
        if (op.op === 'remove') {
          ok = false;
          break;
        }
        node[k] = {};
      }
      node = node[k] as Record<string, unknown>;
    }
    if (!ok) continue;
    const last = keys[keys.length - 1];
    if (op.op === 'replace') node[last] = op.value;
    else delete node[last];
  }
  return out as T;
}
```

`ui/src/preview/h264.ts`:
```ts
const hex = (n: number) => n.toString(16).toUpperCase().padStart(2, '0');

// WebCodecs codec string ("avc1.PPCCLL") from the SPS inside an Annex-B access unit.
export function codecFromSps(au: Uint8Array): string | null {
  for (let i = 0; i + 6 < au.length; i++) {
    if (au[i] === 0 && au[i + 1] === 0 && au[i + 2] === 1 && (au[i + 3] & 0x1f) === 7) {
      return `avc1.${hex(au[i + 4])}${hex(au[i + 5])}${hex(au[i + 6])}`;
    }
  }
  return null;
}
```

`ui/src/api/client.ts`:
```ts
import { applyPatch } from './patch';
import {
  parsePreviewMessage,
  type ErrorInfo,
  type PreviewMessage,
  type PreviewSize,
  type ShowJson,
  type StateTree,
  type Stats,
} from './protocol';

export class EngineError extends Error {
  constructor(public code: string, message: string) {
    super(message);
  }
}

export interface EngineSnapshot {
  status: 'connecting' | 'open' | 'closed';
  state: StateTree;
  show: ShowJson | null;
  stats: Stats | null;
}

type Pending = { resolve: (v: unknown) => void; reject: (e: Error) => void };

// One WebSocket to the engine; reconnects every second while the engine restarts.
export class EngineClient {
  private ws: WebSocket | null = null;
  private nextId = 1;
  private pending = new Map<number, Pending>();
  private listeners = new Set<() => void>();
  private previewListeners = new Map<string, Set<(m: PreviewMessage) => void>>();
  private previewSub: { ids: string[]; size: PreviewSize } | null = null;
  private closed = false;
  private retry: ReturnType<typeof setTimeout> | undefined;
  private snap: EngineSnapshot = { status: 'connecting', state: {}, show: null, stats: null };

  constructor(
    private url: string,
    private wsFactory: (url: string) => WebSocket = (u) => new WebSocket(u),
  ) {}

  connect() {
    const ws = this.wsFactory(this.url);
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => {
      this.update({ status: 'open' });
      if (this.previewSub) this.send('subscribePreview', { cameraIds: this.previewSub.ids, size: this.previewSub.size });
    };
    ws.onmessage = (e) => this.onMessage(e.data);
    ws.onclose = () => {
      for (const p of this.pending.values()) p.reject(new Error('disconnected from the engine'));
      this.pending.clear();
      this.update({ status: 'closed' });
      if (!this.closed) this.retry = setTimeout(() => this.connect(), 1000);
    };
    this.ws = ws;
  }

  close() {
    this.closed = true;
    clearTimeout(this.retry);
    this.ws?.close();
  }

  getSnapshot = (): EngineSnapshot => this.snap;

  subscribe = (fn: () => void) => {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  };

  onPreview(cameraId: string, fn: (m: PreviewMessage) => void) {
    let set = this.previewListeners.get(cameraId);
    if (!set) this.previewListeners.set(cameraId, (set = new Set()));
    set.add(fn);
    return () => set!.delete(fn);
  }

  setPreviewSubscription(ids: string[], size: PreviewSize) {
    this.previewSub = { ids, size };
    if (this.ws?.readyState === 1) this.send('subscribePreview', { cameraIds: ids, size });
  }

  request<T = unknown>(cmd: string, args: Record<string, unknown> = {}): Promise<T> {
    if (!this.ws || this.ws.readyState !== 1) return Promise.reject(new Error('not connected to the engine'));
    const id = this.send(cmd, args);
    return new Promise<T>((resolve, reject) => this.pending.set(id, { resolve: resolve as (v: unknown) => void, reject }));
  }

  private send(cmd: string, args: Record<string, unknown>) {
    const id = this.nextId++;
    this.ws!.send(JSON.stringify({ id, cmd, args }));
    return id;
  }

  private update(part: Partial<EngineSnapshot>) {
    this.snap = { ...this.snap, ...part };
    for (const fn of this.listeners) fn();
  }

  private onMessage(data: unknown) {
    if (data instanceof ArrayBuffer) {
      let m: PreviewMessage;
      try {
        m = parsePreviewMessage(data);
      } catch {
        return;
      }
      for (const fn of this.previewListeners.get(m.cameraId) ?? []) fn(m);
      return;
    }
    let msg: Record<string, unknown>;
    try {
      msg = JSON.parse(String(data));
    } catch {
      return;
    }
    switch (msg.type) {
      case 'snapshot':
        this.update({ state: msg.state as StateTree, show: msg.show as ShowJson });
        return;
      case 'patch':
        this.update({ state: applyPatch(this.snap.state, msg.ops as never) });
        return;
      case 'show':
        this.update({ show: msg.show as ShowJson });
        return;
      case 'stats':
        this.update({ stats: msg as unknown as Stats });
        return;
    }
    const p = typeof msg.id === 'number' ? this.pending.get(msg.id) : undefined;
    if (!p) return;
    this.pending.delete(msg.id as number);
    if (msg.ok) p.resolve(msg.result);
    else {
      const e = msg.error as ErrorInfo;
      p.reject(new EngineError(e?.code ?? 'unknown', e?.message ?? 'unknown error'));
    }
  }
}
```

`ui/src/api/useEngine.ts`:
```ts
import { useSyncExternalStore } from 'react';

import type { EngineClient, EngineSnapshot } from './client';

export function useEngine(client: EngineClient): EngineSnapshot {
  return useSyncExternalStore(client.subscribe, client.getSnapshot);
}
```


- [ ] **Step 4: Labels, marks and the shell bridge**

`ui/src/state/labels.ts`:
```ts
// Short codes on the strip; full names in tooltips (docs/design/multiview.md).
const IN: Record<string, [string, string]> = {
  srt: ['SRT', 'SRT input'],
  ndi: ['NDI', 'NDI input'],
  decklink: ['SDI', 'DeckLink input'],
  aja: ['AJA', 'AJA input'],
  uvc: ['USB', 'USB or built-in camera'],
};
const OUT: Record<string, [string, string]> = {
  ndi: ['NDI', 'NDI output'],
  fullscreen: ['FS', 'Fullscreen'],
  srt: ['SRT', 'SRT output'],
  omt: ['OMT', 'OMT output'],
  vcam: ['VCAM', 'Virtual camera'],
  decklink: ['SDI', 'SDI output'],
  aja: ['SDI', 'AJA SDI output'],
};
export const inputCode = (k: string) => IN[k]?.[0] ?? k.toUpperCase();
export const inputName = (k: string) => IN[k]?.[1] ?? `${k} input`;
export const outputCode = (k: string) => OUT[k]?.[0] ?? k.toUpperCase();
export const outputName = (k: string) => OUT[k]?.[1] ?? `${k} output`;

const RATES: Record<string, string> = { '50/1': '50', '60000/1001': '59.94', '60/1': '60' };
export const rateLabel = (r: string | undefined) => (r ? RATES[r] ?? r : '—');
export const sizeLabel = (r: string | undefined) => (r ? r.replace('x', '×') : '—');
export const signalLabel = { ready: 'Ready', connecting: 'Connecting', live: 'Live', offline: 'Offline' } as const;
```

`ui/src/state/marks.ts`:
```ts
import type { CameraState } from '../api/protocol';

// Per camera: when holding started (the strip's countdown) and when the last fault
// recovered on its own (the strip's mark), worked out from successive states.
export interface Marks {
  holdingSince?: number;
  recoveredAt?: number;
  faulted: boolean;
}

export function nextMarks(prev: Marks | undefined, cam: CameraState | undefined, now: number): Marks {
  const m: Marks = { ...(prev ?? { faulted: false }) };
  if (cam?.phase === 'holding') m.holdingSince ??= now;
  else m.holdingSince = undefined;
  const faulty =
    cam?.phase === 'holding' ||
    cam?.phase === 'slate' ||
    cam?.input?.health === 'degraded' ||
    Object.values(cam?.outputs ?? {}).some((o) => !!o.error);
  if (faulty) m.faulted = true;
  else if (m.faulted) {
    m.faulted = false;
    m.recoveredAt = now;
  }
  return m;
}

const two = (n: number) => String(n).padStart(2, '0');

export function holdLeft(m: Marks | undefined, holdMs: number, now: number): string | null {
  if (m?.holdingSince === undefined) return null;
  const s = Math.max(0, Math.ceil((holdMs - (now - m.holdingSince)) / 1000));
  return `${two(Math.floor(s / 60))}:${two(s % 60)}`;
}

export function clock(t: number): string {
  const d = new Date(t);
  return `${two(d.getHours())}:${two(d.getMinutes())}`;
}
```

`ui/src/bridge.ts`:
```ts
interface WebViewBridge {
  postMessage(m: unknown): void;
  addEventListener(type: 'message', fn: (e: { data: unknown }) => void): void;
  removeEventListener(type: 'message', fn: (e: { data: unknown }) => void): void;
}
const bridge = (): WebViewBridge | undefined => (window as unknown as { chrome?: { webview?: WebViewBridge } }).chrome?.webview;

// The page sends strings (the shell reads them with TryGetWebMessageAsString).
export function postToShell(msg: Record<string, unknown>) {
  bridge()?.postMessage(JSON.stringify(msg));
}

// The shell sends JSON (PostWebMessageAsJson), so e.data is already an object.
export function onShellMessage(fn: (m: { type?: string; [k: string]: unknown }) => void): () => void {
  const b = bridge();
  if (!b) return () => {};
  const h = (e: { data: unknown }) => fn((e.data ?? {}) as { type?: string });
  b.addEventListener('message', h);
  return () => b.removeEventListener('message', h);
}
```

- [ ] **Step 5: Components**

`ui/src/components/Icon.tsx`:
```tsx
const PATHS = {
  matte: '<path d="M8 8.2a2.4 2.4 0 1 0 0-4.8 2.4 2.4 0 0 0 0 4.8Z"/><path d="M3.5 13.5c.6-2.3 2.3-3.6 4.5-3.6s3.9 1.3 4.5 3.6"/><path d="M1.5 4V2.5a1 1 0 0 1 1-1H4M12 1.5h1.5a1 1 0 0 1 1 1V4M14.5 12v1.5a1 1 0 0 1-1 1H12M4 14.5H2.5a1 1 0 0 1-1-1V12"/>',
  bg: '<rect x="1.5" y="3" width="13" height="10" rx="1.5"/><circle cx="5.5" cy="6.5" r="1.2"/><path d="m14.5 10.5-3.5-3.5-6 6"/>',
  color: '<circle cx="8" cy="8" r="6.5"/><path d="M8 1.5v13"/><path d="M8 4.5h4.6M8 8h6.5M8 11.5h4.6"/>',
  arrow: '<path d="M2.5 8h11M9.5 4l4 4-4 4"/>',
  seen: '<path d="M2.5 8a5.5 5.5 0 1 0 1.6-3.9"/><path d="M2.5 2.5v2.8h2.8"/><path d="M8 5v3.2l2 1.3"/>',
  chart: '<rect x="1.5" y="3" width="13" height="10" rx="1"/><path d="M5.8 3v10M10.2 3v10M1.5 8h13"/>',
  settings: '<path d="M2 4.5h7M12 4.5h2M2 11.5h2M7 11.5h7"/><circle cx="10.5" cy="4.5" r="1.5"/><circle cx="5.5" cy="11.5" r="1.5"/>',
  plus: '<path d="M8 3v10M3 8h10"/>',
  chev: '<path d="m6 4 4 4-4 4"/>',
  warn: '<path d="M8 2 1.5 13.5h13L8 2Z"/><path d="M8 6.5v3M8 11.6v.1"/>',
  err: '<circle cx="8" cy="8" r="6.5"/><path d="M8 4.8v3.6M8 10.9v.1"/>',
  retry: '<path d="M13.5 8a5.5 5.5 0 1 1-1.6-3.9"/><path d="M13.5 2.5v2.8h-2.8"/>',
  check: '<path d="m3.5 8.5 3 3 6-7"/>',
  cam: '<rect x="1.5" y="4" width="9.5" height="8" rx="1.5"/><path d="m11 7 3.5-2v6L11 9"/>',
  power: '<path d="M8 1.5v6"/><path d="M4.4 4a5.5 5.5 0 1 0 7.2 0"/>',
  trash: '<path d="M2.5 4h11M6 4V2.5h4V4M4 4l.7 9.5h6.6L12 4"/>',
  min: '<path d="M3 8h10"/>',
  max: '<rect x="3.5" y="3.5" width="9" height="9"/>',
  restore: '<rect x="3.5" y="5.5" width="7" height="7"/><path d="M5.5 5.5v-2h7v7h-2"/>',
  close: '<path d="m3.5 3.5 9 9M12.5 3.5l-9 9"/>',
} as const;
export type IconName = keyof typeof PATHS;

// One stroke weight for every icon (craft floor: no glyphs standing in for icons).
export function Icon({ name, size = 16 }: { name: IconName; size?: number }) {
  return <svg className="i" width={size} height={size} viewBox="0 0 16 16" aria-hidden="true" dangerouslySetInnerHTML={{ __html: PATHS[name] }} />;
}
```

`ui/src/components/ErrorBlock.tsx`:
```tsx
import { Icon } from './Icon';

// Persistent block at the top of the affected panel; never a toast (spec §6.1).
export function ErrorBlock({ title, message, severity = 'danger', label }: { title?: string; message: string; severity?: 'danger' | 'warning' | 'info'; label?: string }) {
  return (
    <div className={`block ${severity}`} role={severity === 'danger' ? 'alert' : 'status'} aria-label={label}>
      <Icon name={severity === 'danger' ? 'err' : severity === 'warning' ? 'warn' : 'chart'} size={14} />
      <div>
        {title && <b>{title} </b>}
        <span className="d">{message}</span>
      </div>
    </div>
  );
}
```

`ui/src/components/Modal.tsx`:
```tsx
import { useEffect, type ReactNode } from 'react';

import { Icon } from './Icon';

// Native-style dialog: title bar with close, body, footer band. Escape closes.
export function Modal({ title, onClose, footer, children, wide }: { title: string; onClose: () => void; footer: ReactNode; children: ReactNode; wide?: boolean }) {
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => e.key === 'Escape' && onClose();
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [onClose]);
  return (
    <div className="scrim">
      <div className={`dlg${wide ? ' wide' : ''}`} role="dialog" aria-modal="true" aria-label={title}>
        <div className="dlg-h">
          {title}
          <button className="x" aria-label="Close" onClick={onClose}>
            <Icon name="close" size={12} />
          </button>
        </div>
        <div className="dlg-b">{children}</div>
        <div className="dlg-f">{footer}</div>
      </div>
    </div>
  );
}
```

`ui/src/components/Prop.tsx`:
```tsx
import { useId, type ReactNode } from 'react';

// Label-left property row (spec §6.4). The render prop receives the id to label the control.
export function Prop({ label, children, hint }: { label: string; children: (id: string) => ReactNode; hint?: string }) {
  const id = useId();
  return (
    <div className="prop">
      <label htmlFor={id}>{label}</label>
      <div className="prop-c">
        {children(id)}
        {hint && <span className="hint">{hint}</span>}
      </div>
    </div>
  );
}
```

`ui/src/components/Menu.tsx`:
```tsx
import { useEffect, useRef, useState } from 'react';

export interface MenuItem {
  label: string;
  shortcut?: string;
  danger?: boolean;
  disabled?: boolean;
  onSelect: () => void;
}

// A title-bar menu: click opens, click outside or Escape closes.
export function Menu({ label, items }: { label: string; items: (MenuItem | 'sep')[] }) {
  const [open, setOpen] = useState(false);
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!open) return;
    const close = (e: MouseEvent | KeyboardEvent) => {
      if (e instanceof KeyboardEvent ? e.key === 'Escape' : !ref.current?.contains(e.target as Node)) setOpen(false);
    };
    window.addEventListener('mousedown', close);
    window.addEventListener('keydown', close);
    return () => {
      window.removeEventListener('mousedown', close);
      window.removeEventListener('keydown', close);
    };
  }, [open]);
  return (
    <div className="menu-root" ref={ref}>
      <button className={`menu-btn${open ? ' open' : ''}`} aria-haspopup="menu" aria-expanded={open} onClick={() => setOpen(!open)}>
        {label}
      </button>
      {open && (
        <div className="menu" role="menu" aria-label={label}>
          {items.map((it, i) =>
            it === 'sep' ? (
              <hr key={i} />
            ) : (
              <button
                key={it.label}
                role="menuitem"
                className={`it${it.danger ? ' danger' : ''}`}
                disabled={it.disabled}
                onClick={() => {
                  setOpen(false);
                  it.onSelect();
                }}
              >
                <span>{it.label}</span>
                <span className="k">{it.shortcut}</span>
              </button>
            ),
          )}
        </div>
      )}
    </div>
  );
}
```

`ui/src/preview/PreviewCanvas.tsx`:
```tsx
import { useEffect, useRef } from 'react';

import type { EngineClient } from '../api/client';
import type { PreviewSize } from '../api/protocol';
import { codecFromSps } from './h264';

// Decodes the engine's H.264 preview with WebCodecs (hardware decode in WebView2).
export function PreviewCanvas({ client, cameraId, size = 'small' }: { client: EngineClient; cameraId: string; size?: PreviewSize }) {
  const canvas = useRef<HTMLCanvasElement>(null);
  useEffect(() => {
    if (typeof VideoDecoder === 'undefined') return; // jsdom / very old runtime
    let configured = false;
    const decoder = new VideoDecoder({
      output: (frame) => {
        const c = canvas.current;
        if (c) {
          c.width = frame.displayWidth;
          c.height = frame.displayHeight;
          c.getContext('2d')?.drawImage(frame, 0, 0);
        }
        frame.close();
      },
      error: () => {
        configured = false;
        client.request('requestKeyframe', { cameraId, size }).catch(() => {});
      },
    });
    const off = client.onPreview(cameraId, (m) => {
      if (!configured) {
        if (!m.keyframe) return;
        const codec = codecFromSps(m.data);
        if (!codec) return;
        decoder.configure({ codec, optimizeForLatency: true });
        configured = true;
      }
      if (decoder.state !== 'configured') return;
      decoder.decode(new EncodedVideoChunk({ type: m.keyframe ? 'key' : 'delta', timestamp: m.ptsUs, data: m.data }));
    });
    return () => {
      off();
      if (decoder.state !== 'closed') decoder.close();
    };
  }, [client, cameraId, size]);
  return <canvas ref={canvas} aria-hidden="true" />;
}
```


- [ ] **Step 6: Window shell — title bar, toolbar, status bar**

`ui/src/shell/TitleBar.tsx`:
```tsx
import { useEffect, useState } from 'react';

import { onShellMessage, postToShell } from '../bridge';
import { Icon } from '../components/Icon';
import { Menu, type MenuItem } from '../components/Menu';

export interface TitleMenus {
  file: (MenuItem | 'sep')[];
  camera: (MenuItem | 'sep')[];
  view: (MenuItem | 'sep')[];
  help: (MenuItem | 'sep')[];
}

// Custom title bar (spec §6.4): the shell removed the system caption; CSS app-region drags.
export function TitleBar({ menus }: { menus: TitleMenus }) {
  const [maximized, setMaximized] = useState(false);
  useEffect(() => onShellMessage((m) => m.type === 'windowState' && setMaximized(!!m.maximized)), []);
  const win = (action: string) => postToShell({ type: 'window', action });
  return (
    <header className="titlebar">
      <span className="app-ic">
        <Icon name="cam" />
      </span>
      <nav className="menus" aria-label="Menu bar">
        <Menu label="File" items={menus.file} />
        <Menu label="Camera" items={menus.camera} />
        <Menu label="View" items={menus.view} />
        <Menu label="Help" items={menus.help} />
      </nav>
      <div className="wintitle">BreadCamera</div>
      <div className="caption">
        <button aria-label="Minimise" onClick={() => win('minimize')}>
          <Icon name="min" size={12} />
        </button>
        <button aria-label={maximized ? 'Restore' : 'Maximise'} onClick={() => win('maximize')}>
          <Icon name={maximized ? 'restore' : 'max'} size={12} />
        </button>
        <button className="close" aria-label="Close to tray (outputs keep running)" onClick={() => win('close')}>
          <Icon name="close" size={12} />
        </button>
      </div>
    </header>
  );
}
```
(M5 adds the Show menu with Settings…; M2 adds Add effect… to the Camera menu.)

`ui/src/shell/Toolbar.tsx`:
```tsx
import type { ReactNode } from 'react';

import { Icon } from '../components/Icon';

// Add camera is the only "add" here: outputs and effects belong to a camera (spec §6.4).
export function Toolbar({ onAddCamera, onStopAll, extra }: { onAddCamera: () => void; onStopAll: () => void; extra?: ReactNode }) {
  return (
    <div className="toolbar" role="toolbar" aria-label="Toolbar">
      <button className="tb accent" title="Add camera (Ctrl+Shift+N)" onClick={onAddCamera}>
        <Icon name="plus" />
        Add camera
      </button>
      <span className="tspace" />
      {extra}
      <span className="tsep" />
      <button className="tb danger" onClick={onStopAll}>
        <Icon name="power" />
        Stop all outputs…
      </button>
    </div>
  );
}
```

`ui/src/shell/StatusBar.tsx`:
```tsx
import type { EngineSnapshot } from '../api/client';
import { Icon } from '../components/Icon';
import { rateLabel, sizeLabel } from '../state/labels';

export function StatusBar({ snap }: { snap: EngineSnapshot }) {
  const e = snap.state.engine ?? {};
  const open = snap.status === 'open';
  const outs = Object.values(snap.state.cameras ?? {}).flatMap((c) => Object.values(c.outputs ?? {}));
  const live = open ? outs.filter((o) => o.state === 'live').length : 0;
  const failing = open ? outs.filter((o) => o.error).length : 0;
  return (
    <footer className="status" aria-label="Status bar">
      <span>
        <i className={`dot${open ? '' : ' warn'}`} />
        {open ? 'Engine running' : <span className="w">Engine restarting…</span>}
      </span>
      <span>
        <b>{rateLabel(e.rate)}</b> fps · <b>{sizeLabel(e.resolution)}</b>
      </span>
      <span>GPU {e.gpu ?? '—'}</span>
      <span className={e.overruns ? 'w' : ''}>
        {e.overruns ? (
          <>
            <Icon name="warn" size={12} />
            {e.overruns} tick overruns
          </>
        ) : (
          'Ticks on time'
        )}
      </span>
      <span className="r">
        Outputs <b>{live} live</b>
        {failing > 0 && <span className="e"> · {failing} retrying</span>}
      </span>
    </footer>
  );
}
```

- [ ] **Step 7: Multiview and the strip**

`ui/src/multiview/Strip.tsx`:
```tsx
import type { CameraConfig, CameraState } from '../api/protocol';
import { Icon } from '../components/Icon';
import { inputCode, inputName, outputCode, outputName, signalLabel } from '../state/labels';
import { clock, holdLeft, type Marks } from '../state/marks';

// The one-row state strip (docs/design/multiview.md): input chip · CAM n · name · → · outputs · mark.
export function Strip({ n, cfg, cam, marks, holdMs, now, dense, offline }: {
  n: number;
  cfg: CameraConfig;
  cam: CameraState | undefined;
  marks: Marks | undefined;
  holdMs: number;
  now: number;
  dense: boolean;
  offline: boolean;
}) {
  const health = offline ? 'offline' : cam?.input?.health ?? 'offline';
  const left = holdLeft(marks, holdMs, now);
  const slate = !offline && cam?.phase === 'slate';
  const holding = !offline && left !== null;
  const chipCls = slate || holding || health === 'down' ? 'err' : health === 'receiving' ? 'ready' : health === 'degraded' ? 'conn' : 'off';
  const name = inputName(cfg.input.kind);
  const chipTitle = slate
    ? `${name}: down, showing the slate`
    : holding
      ? `${name}: down, holding the last frame (${left} left)`
      : `${name}: ${health}`;
  const max = dense ? 2 : 4;
  const shown = cfg.outputs.slice(0, max);
  const stripCls = slate || holding ? ' err' : '';
  return (
    <div className={`strip${stripCls}`}>
      <span className={`in-chip ${chipCls}`} title={chipTitle}>
        {slate ? 'SLATE' : holding ? left : inputCode(cfg.input.kind)}
      </span>
      {!dense && <span className="cam-no">CAM {n}</span>}
      <span className="cam-name" title={cfg.name}>
        {cfg.name}
      </span>
      <span className="flow">
        <Icon name="arrow" size={12} />
      </span>
      <span className="outs">
        {shown.map((o) => {
          const os = cam?.outputs?.[o.id];
          const st = offline ? 'offline' : os?.state ?? 'ready';
          if (!offline && os?.error) {
            return (
              <span key={o.id} className="err-chip" title={`${outputName(o.kind)}: failed, ${os.error.message}`}>
                <Icon name="retry" size={11} />
                {outputCode(o.kind)}
              </span>
            );
          }
          return (
            <span key={o.id} className={`pill ${st}`} title={`${outputName(o.kind)}: ${signalLabel[st]}`}>
              <i />
              {outputCode(o.kind)}
            </span>
          );
        })}
        {cfg.outputs.length > max && (
          <span className="more" title={cfg.outputs.slice(max).map((o) => outputName(o.kind)).join(', ')}>
            +{cfg.outputs.length - max}
          </span>
        )}
      </span>
      {!offline && marks?.recoveredAt !== undefined && (
        <span className="seen" title="A fault recovered on its own. Select the camera to clear this mark.">
          <Icon name="seen" size={11} />
          {clock(marks.recoveredAt)}
        </span>
      )}
    </div>
  );
}
```
M2 adds the effect icons between the name and the arrow.

`ui/src/multiview/Multiview.tsx`:
```tsx
import { useEffect, useLayoutEffect, useMemo, useRef, useState, type KeyboardEvent } from 'react';

import type { EngineClient } from '../api/client';
import type { CameraConfig, StateTree } from '../api/protocol';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon } from '../components/Icon';
import { PreviewCanvas } from '../preview/PreviewCanvas';
import type { Marks } from '../state/marks';
import { Strip } from './Strip';

export type Layout = 'auto' | '2x2' | '3x2' | '4x2';
const AUTO: Record<number, [number, number]> = { 1: [1, 1], 2: [2, 1], 3: [2, 2], 4: [2, 2], 5: [3, 2], 6: [3, 2], 7: [4, 2], 8: [4, 2] };
export function gridOf(layout: Layout, n: number): [number, number] {
  if (layout === 'auto') return AUTO[Math.min(Math.max(n, 1), 8)];
  const [c, r] = layout.split('x').map(Number);
  return [c, r];
}

const STRIP = 29;
const GAP = 3;

// Tiles keep 16:9 and the whole wall fits the area (docs/design/multiview.md §6).
export function Multiview({ client, cameras, state, marks, holdMs, now, selected, onSelect, layout, onLayout, offline }: {
  client: EngineClient;
  cameras: CameraConfig[];
  state: StateTree;
  marks: Record<string, Marks>;
  holdMs: number;
  now: number;
  selected: string | null;
  onSelect: (id: string) => void;
  layout: Layout;
  onLayout: (l: Layout) => void;
  offline: boolean;
}) {
  const area = useRef<HTMLDivElement>(null);
  const [tileW, setTileW] = useState(320);
  const [cols, rows] = gridOf(layout, cameras.length);
  const ids = useMemo(() => cameras.map((c) => c.id), [cameras]);

  useEffect(() => {
    // Only stream previews while the window is visible (spec §6.3).
    const sync = () => client.setPreviewSubscription(document.visibilityState === 'hidden' ? [] : ids, 'small');
    sync();
    document.addEventListener('visibilitychange', sync);
    return () => document.removeEventListener('visibilitychange', sync);
  }, [client, ids]);

  useLayoutEffect(() => {
    const el = area.current;
    if (!el || typeof ResizeObserver === 'undefined') return;
    const fit = () => {
      const W = el.clientWidth - 8;
      const H = el.clientHeight - 8;
      setTileW(Math.max(160, Math.floor(Math.min((W - GAP * (cols - 1)) / cols, ((H - GAP * (rows - 1)) / rows - STRIP) * (16 / 9)))));
    };
    fit();
    const ro = new ResizeObserver(fit);
    ro.observe(el);
    return () => ro.disconnect();
  }, [cols, rows]);

  function onKey(e: KeyboardEvent) {
    const i = Math.max(0, ids.indexOf(selected ?? ''));
    const step = { ArrowRight: 1, ArrowLeft: -1, ArrowDown: cols, ArrowUp: -cols }[e.key];
    if (step === undefined || ids.length === 0) return;
    e.preventDefault();
    onSelect(ids[Math.min(ids.length - 1, Math.max(0, i + step))]);
  }

  const warnings = Object.values(state.engine?.warnings ?? {}).filter((w) => w);
  return (
    <section className="center" aria-label="Multiview area">
      <div className="subbar">
        <b>Multiview</b>
        <span>{cameras.length} cameras</span>
        <span className="right">
          Layout
          <span className="seg" role="radiogroup" aria-label="Layout">
            {(['auto', '2x2', '3x2', '4x2'] as Layout[]).map((l) => (
              <button key={l} role="radio" aria-checked={layout === l} className={layout === l ? 'on' : ''} onClick={() => onLayout(l)}>
                {l === 'auto' ? 'Auto' : l.replace('x', '×')}
              </button>
            ))}
          </span>
        </span>
      </div>
      <div className="wall-wrap">
        {offline && (
          <ErrorBlock
            severity="warning"
            label="Engine restarting"
            title="Engine restarting."
            message="The connection to the engine dropped; the shell restarts it with this show. Outputs come back in about 3 seconds. The pictures are the last frames received."
          />
        )}
        {warnings.map((w) => (
          <ErrorBlock key={w} severity="warning" title="Warning." message={w} />
        ))}
        {cameras.length === 0 ? (
          <div className="empty">
            <div>
              <div className="frame">
                <Icon name="cam" />
              </div>
              <span className="h">No cameras yet</span>
              <span>Use Add camera in the toolbar (Ctrl+Shift+N). Each camera can send to several outputs.</span>
            </div>
          </div>
        ) : (
          <div className="wall-area" ref={area}>
            <div className="wall" role="listbox" aria-label="Multiview" tabIndex={0} onKeyDown={onKey} style={{ gridTemplateColumns: `repeat(${cols}, ${tileW}px)` }}>
              {cameras.map((cfg, i) => (
                <div
                  key={cfg.id}
                  role="option"
                  aria-selected={selected === cfg.id}
                  aria-label={`CAM ${i + 1} ${cfg.name}`}
                  className={`tile${selected === cfg.id ? ' sel' : ''}`}
                  onClick={() => onSelect(cfg.id)}
                >
                  <div className="pic">
                    <PreviewCanvas client={client} cameraId={cfg.id} />
                  </div>
                  <Strip n={i + 1} cfg={cfg} cam={state.cameras?.[cfg.id]} marks={marks[cfg.id]} holdMs={holdMs} now={now} dense={cameras.length > 4} offline={offline} />
                </div>
              ))}
            </div>
          </div>
        )}
      </div>
    </section>
  );
}
```

- [ ] **Step 8: Inspector and dialogs**

`ui/src/inspector/Inspector.tsx`:
```tsx
import { useEffect, useState, type ReactNode } from 'react';

import type { EngineClient, EngineSnapshot } from '../api/client';
import type { CameraConfig } from '../api/protocol';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon } from '../components/Icon';
import { Prop } from '../components/Prop';
import { ConfirmDialog } from '../dialogs/ConfirmDialog';
import { inputCode, outputCode, outputName, signalLabel } from '../state/labels';

function Section({ title, right, children }: { title: string; right?: ReactNode; children: ReactNode }) {
  return (
    <details className="sec" open>
      <summary>
        <span className="chev">
          <Icon name="chev" size={12} />
        </span>
        {title}
        <span className="right" onClick={(e) => e.preventDefault()}>
          {right}
        </span>
      </summary>
      <div className="sec-b">{children}</div>
    </details>
  );
}

function formatLabel(o: CameraConfig['outputs'][number]) {
  const f = o.format;
  const size = f && f.width ? `${f.width}×${f.height}` : 'same as show';
  return `${o.keyed ? 'Keyed' : 'Composited'} · ${size}${f?.halfRate ? ' · half rate' : ''}`;
}

// The selected camera: Input, (M2: Effects via children), Outputs (spec §6.4).
export function Inspector({ client, snap, cam, n, onAddOutput, onRemoved, children }: {
  client: EngineClient;
  snap: EngineSnapshot;
  cam: CameraConfig | undefined;
  n: number;
  onAddOutput: () => void;
  onRemoved: () => void;
  children?: ReactNode;
}) {
  const [confirm, setConfirm] = useState<null | { kind: 'camera' } | { kind: 'output'; id: string }>(null);
  const p = cam?.input.params ?? {};
  const [draft, setDraft] = useState({ mode: '', url: '', host: '', port: '', latency: '', passphrase: '' });
  const [error, setError] = useState('');
  useEffect(() => {
    setDraft({
      mode: String(p.mode ?? 'caller'),
      url: String(p.url ?? ''),
      host: String(p.host ?? ''),
      port: p.port === undefined ? '' : String(p.port),
      latency: String(p.latencyMs ?? 120),
      passphrase: '',
    });
    setError('');
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [cam?.id, JSON.stringify(p)]);

  if (!cam) {
    return (
      <aside className="insp" aria-label="Inspector">
        <div className="insp-h">
          <span className="k">INSPECTOR</span>
          <div className="t dim">No camera selected</div>
        </div>
        <p className="note pad">Select a camera in Multiview to edit its input and outputs.</p>
      </aside>
    );
  }
  const cs = snap.state.cameras?.[cam.id];
  const stats = snap.stats?.cameras?.[cam.id];
  const health = cs?.input?.health ?? 'offline';
  const dirty =
    draft.mode !== String(p.mode ?? 'caller') ||
    draft.url !== String(p.url ?? '') ||
    draft.host !== String(p.host ?? '') ||
    draft.port !== (p.port === undefined ? '' : String(p.port)) ||
    draft.latency !== String(p.latencyMs ?? 120) ||
    draft.passphrase !== '';

  async function apply() {
    const params: Record<string, unknown> = { mode: draft.mode, latencyMs: Number(draft.latency) || 120 };
    if (draft.url) params.url = draft.url;
    else {
      params.port = Number(draft.port);
      if (draft.host) params.host = draft.host;
    }
    params.passphrase = draft.passphrase ? draft.passphrase : { set: (p.passphrase as { set?: boolean })?.set ?? false };
    if (p.streamId) params.streamId = p.streamId;
    try {
      await client.request('updateCamera', { id: cam!.id, input: { kind: cam!.input.kind, params } });
      setError('');
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }

  return (
    <aside className="insp" aria-label="Inspector">
      <div className="insp-h">
        <span className="k">INSPECTOR · CAM {n}</span>
        <div className="t">
          <span className={`in-chip ${health === 'receiving' ? 'ready' : health === 'degraded' ? 'conn' : health === 'down' ? 'err' : 'off'}`}>{inputCode(cam.input.kind)}</span>
          <span className="n">{cam.name}</span>
        </div>
      </div>
      <div className="insp-b">
        <Section title="Input" right={<span className="ro">{health}</span>}>
          {cs?.input?.error && <ErrorBlock title="Input failed." message={cs.input.error.message} />}
          {cs?.tickError && <ErrorBlock title="Processing failed." message={cs.tickError} />}
          {error && <ErrorBlock message={error} />}
          <Prop label="Mode">
            {(id) => (
              <select id={id} className="ctl" value={draft.mode} onChange={(e) => setDraft({ ...draft, mode: e.target.value })}>
                <option value="caller">Caller (BreadCamera dials out)</option>
                <option value="listener">Listener (waits for the source)</option>
              </select>
            )}
          </Prop>
          {draft.mode === 'caller' && (
            <Prop label="URL">{(id) => <input id={id} className="ctl mono" value={draft.url} placeholder="srt://host:port" onChange={(e) => setDraft({ ...draft, url: e.target.value })} />}</Prop>
          )}
          {!draft.url && (
            <>
              {draft.mode === 'caller' && <Prop label="Host">{(id) => <input id={id} className="ctl mono" value={draft.host} onChange={(e) => setDraft({ ...draft, host: e.target.value })} />}</Prop>}
              <Prop label="Port">{(id) => <input id={id} className="ctl mono" inputMode="numeric" value={draft.port} onChange={(e) => setDraft({ ...draft, port: e.target.value })} />}</Prop>
            </>
          )}
          <Prop label="Latency (ms)">{(id) => <input id={id} className="ctl mono" type="number" min={20} max={8000} value={draft.latency} onChange={(e) => setDraft({ ...draft, latency: e.target.value })} />}</Prop>
          <Prop label="Passphrase" hint={(p.passphrase as { set?: boolean })?.set ? 'Set. Type a new one to replace it.' : 'Optional, stored encrypted.'}>
            {(id) => <input id={id} className="ctl" type="password" value={draft.passphrase} onChange={(e) => setDraft({ ...draft, passphrase: e.target.value })} />}
          </Prop>
          {stats && (
            <Prop label="Receiving">
              {() => (
                <span className="ro">
                  {stats.fps.toFixed(0)} fps · {stats.drops} dropped · {stats.latencyMs.toFixed(0)} ms
                </span>
              )}
            </Prop>
          )}
          <div className="row-end">
            <button className="btn" disabled={!dirty} onClick={apply}>
              Apply input changes
            </button>
          </div>
        </Section>
        {children}
        <Section
          title="Outputs"
          right={
            <button className="tb sm" onClick={onAddOutput}>
              <Icon name="plus" size={12} />
              Add output
            </button>
          }
        >
          {cam.outputs.map((o) => cs?.outputs?.[o.id]?.error && <ErrorBlock key={`e-${o.id}`} title={`${outputName(o.kind)} failed.`} message={cs!.outputs![o.id].error!.message} />)}
          {cam.outputs.length === 0 && <p className="note">No outputs yet. Add one to send this camera on.</p>}
          {cam.outputs.map((o) => {
            const os = cs?.outputs?.[o.id];
            const st = os?.state ?? 'ready';
            const dest = o.kind === 'ndi' ? String(o.params.name ?? `BreadCamera - ${cam.name}`) : o.kind === 'fullscreen' ? `Monitor ${Number(o.params.monitor ?? 0) + 1}` : outputName(o.kind);
            return (
              <div className="orow" key={o.id}>
                {os?.error ? (
                  <span className="err-chip">
                    <Icon name="retry" size={11} />
                    {outputCode(o.kind)}
                  </span>
                ) : (
                  <span className={`pill ${st}`} aria-label={`${outputName(o.kind)}: ${signalLabel[st]}`}>
                    <i />
                    {outputCode(o.kind)}
                  </span>
                )}
                <div className="o-main">
                  <div className="n">{dest}</div>
                  <div className="m">{formatLabel(o)}</div>
                </div>
                <label className="sw-wrap" title={o.enabled ? 'Sending. Switch off to stop this output.' : 'Stopped. Switch on to send.'}>
                  <input type="checkbox" role="switch" aria-label={`${outputName(o.kind)} on`} checked={o.enabled} onChange={(e) => client.request('updateOutput', { cameraId: cam.id, id: o.id, enabled: e.target.checked }).catch(() => {})} />
                  <span className="sw" />
                </label>
                <button className="ibtn" aria-label={`Remove ${outputName(o.kind)}`} onClick={() => setConfirm({ kind: 'output', id: o.id })}>
                  <Icon name="trash" size={14} />
                </button>
              </div>
            );
          })}
        </Section>
        <div className="pad">
          <button className="btn ghost-danger" onClick={() => setConfirm({ kind: 'camera' })}>
            <Icon name="trash" size={14} />
            Remove camera…
          </button>
        </div>
      </div>
      {confirm?.kind === 'camera' && (
        <ConfirmDialog
          title="Remove camera?"
          body={`CAM ${n} "${cam.name}" and all its outputs stop now. Their receivers lose picture.`}
          confirm="Remove camera"
          onCancel={() => setConfirm(null)}
          onConfirm={async () => {
            await client.request('removeCamera', { id: cam.id }).catch(() => {});
            setConfirm(null);
            onRemoved();
          }}
        />
      )}
      {confirm?.kind === 'output' && (
        <ConfirmDialog
          title="Remove output?"
          body="Its receiver loses picture now."
          confirm="Remove output"
          onCancel={() => setConfirm(null)}
          onConfirm={async () => {
            await client.request('removeOutput', { cameraId: cam.id, id: confirm.id }).catch(() => {});
            setConfirm(null);
          }}
        />
      )}
    </aside>
  );
}
```

`ui/src/dialogs/ConfirmDialog.tsx`:
```tsx
import { Modal } from '../components/Modal';

// Destructive actions never run at once: title states the consequence, the button names the verb (UX contract §8.4).
export function ConfirmDialog({ title, body, confirm, onConfirm, onCancel, danger = true }: { title: string; body: string; confirm: string; onConfirm: () => void; onCancel: () => void; danger?: boolean }) {
  return (
    <Modal
      title={title}
      onClose={onCancel}
      footer={
        <>
          <button className="btn" onClick={onCancel}>
            Cancel
          </button>
          <button className={`btn ${danger ? 'danger' : 'primary'}`} onClick={onConfirm}>
            {confirm}
          </button>
        </>
      }
    >
      <p>{body}</p>
    </Modal>
  );
}
```

`ui/src/dialogs/AddCameraDialog.tsx`:
```tsx
import { useState } from 'react';

import type { EngineClient } from '../api/client';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon } from '../components/Icon';
import { Modal } from '../components/Modal';
import { Prop } from '../components/Prop';

const chars = (s: string) => [...s].length;

export function AddCameraDialog({ client, onClose }: { client: EngineClient; onClose: () => void }) {
  const [name, setName] = useState('');
  const [mode, setMode] = useState<'caller' | 'listener'>('caller');
  const [url, setUrl] = useState('');
  const [host, setHost] = useState('');
  const [port, setPort] = useState('9000');
  const [latency, setLatency] = useState('120');
  const [passphrase, setPassphrase] = useState('');
  const [streamId, setStreamId] = useState('');
  const [serverError, setServerError] = useState('');

  const portNum = Number(port);
  const errors = {
    name: chars(name.trim()) < 1 || chars(name) > 80 ? 'Name must be 1 to 80 characters' : '',
    port: !url && (!Number.isInteger(portNum) || portNum < 1 || portNum > 65535) ? 'Port is a number from 1 to 65535' : '',
    passphrase: passphrase && (passphrase.length < 10 || passphrase.length > 79) ? 'Passphrase is empty or 10 to 79 characters' : '',
    host: mode === 'caller' && !url && !host ? 'Caller mode needs a host or a URL' : '',
  };
  const valid = Object.values(errors).every((e) => !e);

  async function submit() {
    const params: Record<string, unknown> = { mode, latencyMs: Number(latency) || 120 };
    if (url) params.url = url;
    else {
      params.port = portNum;
      if (host) params.host = host;
    }
    if (passphrase) params.passphrase = passphrase;
    if (streamId) params.streamId = streamId;
    try {
      await client.request('addCamera', { name: name.trim(), input: { kind: 'srt', params } });
      onClose();
    } catch (e) {
      setServerError(e instanceof Error ? e.message : String(e));
    }
  }

  return (
    <Modal
      title="Add camera"
      onClose={onClose}
      footer={
        <>
          <button className="btn" onClick={onClose}>
            Cancel
          </button>
          <button className="btn primary" disabled={!valid} onClick={submit}>
            <Icon name="plus" size={14} />
            Add camera
          </button>
        </>
      }
    >
      {serverError && <ErrorBlock message={serverError} />}
      <Prop label="Name" hint={name && errors.name ? errors.name : 'Any language works.'}>
        {(id) => <input id={id} className="ctl" autoFocus value={name} onChange={(e) => setName(e.target.value)} aria-invalid={!!name && !!errors.name} />}
      </Prop>
      <Prop label="Input type">
        {(id) => (
          <select id={id} className="ctl" value="srt" disabled>
            <option value="srt">SRT (BreadCall feeds, encoders)</option>
          </select>
        )}
      </Prop>
      <Prop label="Mode">
        {(id) => (
          <select id={id} className="ctl" value={mode} onChange={(e) => setMode(e.target.value as 'caller' | 'listener')}>
            <option value="caller">Caller (BreadCamera dials out)</option>
            <option value="listener">Listener (waits for the source)</option>
          </select>
        )}
      </Prop>
      {mode === 'caller' && <Prop label="URL" hint="Paste the feed URL from BreadCall.">{(id) => <input id={id} className="ctl mono" value={url} placeholder="srt://host:port" onChange={(e) => setUrl(e.target.value)} />}</Prop>}
      {!url && (
        <>
          {mode === 'caller' && <Prop label="Host" hint={errors.host}>{(id) => <input id={id} className="ctl mono" value={host} onChange={(e) => setHost(e.target.value)} />}</Prop>}
          <Prop label="Port" hint={errors.port}>{(id) => <input id={id} className="ctl mono" inputMode="numeric" value={port} onChange={(e) => setPort(e.target.value)} />}</Prop>
        </>
      )}
      <Prop label="Latency (ms)">{(id) => <input id={id} className="ctl mono" type="number" value={latency} onChange={(e) => setLatency(e.target.value)} />}</Prop>
      <Prop label="Passphrase" hint={errors.passphrase || 'Optional, stored encrypted.'}>{(id) => <input id={id} className="ctl" type="password" value={passphrase} onChange={(e) => setPassphrase(e.target.value)} />}</Prop>
      <Prop label="Stream ID">{(id) => <input id={id} className="ctl mono" value={streamId} onChange={(e) => setStreamId(e.target.value)} />}</Prop>
    </Modal>
  );
}
```

`ui/src/dialogs/AddOutputDialog.tsx`:
```tsx
import { useEffect, useState } from 'react';

import type { EngineClient } from '../api/client';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon } from '../components/Icon';
import { Modal } from '../components/Modal';
import { Prop } from '../components/Prop';

interface Monitor {
  index: number;
  name: string;
  width: number;
  height: number;
  primary: boolean;
}

const SIZES: Record<string, [number, number]> = { same: [0, 0], '1280x720': [1280, 720], '960x540': [960, 540] };

// Output format per spec §4.3: same as show or smaller; same rate or half. M4 adds kinds.
export function AddOutputDialog({ client, camera, n, onClose }: { client: EngineClient; camera: { id: string; name: string }; n: number; onClose: () => void }) {
  const [kind, setKind] = useState<'ndi' | 'fullscreen'>('ndi');
  const [ndiName, setNdiName] = useState(`BreadCamera - ${camera.name}`);
  const [monitors, setMonitors] = useState<Monitor[]>([]);
  const [monitor, setMonitor] = useState(0);
  const [size, setSize] = useState('same');
  const [rate, setRate] = useState<'same' | 'half'>('same');
  const [serverError, setServerError] = useState('');

  useEffect(() => {
    client.request<Monitor[]>('listMonitors').then(setMonitors).catch(() => setMonitors([]));
  }, [client]);

  async function submit() {
    const params = kind === 'ndi' ? { name: ndiName } : { monitor };
    const [width, height] = kind === 'fullscreen' ? [0, 0] : SIZES[size];
    try {
      await client.request('addOutput', { cameraId: camera.id, output: { kind, params, format: { width, height, halfRate: rate === 'half' } } });
      onClose();
    } catch (e) {
      setServerError(e instanceof Error ? e.message : String(e));
    }
  }

  return (
    <Modal
      title={`Add output to CAM ${n}`}
      onClose={onClose}
      footer={
        <>
          <button className="btn" onClick={onClose}>
            Cancel
          </button>
          <button className="btn primary" disabled={kind === 'ndi' && !ndiName.trim()} onClick={submit}>
            <Icon name="plus" size={14} />
            Add output
          </button>
        </>
      }
    >
      {serverError && <ErrorBlock message={serverError} />}
      <Prop label="Output type">
        {(id) => (
          <select id={id} className="ctl" value={kind} onChange={(e) => setKind(e.target.value as 'ndi' | 'fullscreen')}>
            <option value="ndi">NDI</option>
            <option value="fullscreen">Fullscreen on a monitor</option>
          </select>
        )}
      </Prop>
      {kind === 'ndi' && <Prop label="NDI source name">{(id) => <input id={id} className="ctl" value={ndiName} onChange={(e) => setNdiName(e.target.value)} />}</Prop>}
      {kind === 'fullscreen' && (
        <Prop label="Monitor">
          {(id) => (
            <select id={id} className="ctl" value={monitor} onChange={(e) => setMonitor(Number(e.target.value))}>
              {monitors.map((m) => (
                <option key={m.index} value={m.index}>
                  {m.index + 1}. {m.name} ({m.width}×{m.height})
                </option>
              ))}
            </select>
          )}
        </Prop>
      )}
      {kind !== 'fullscreen' && (
        <Prop label="Resolution">
          {(id) => (
            <select id={id} className="ctl" value={size} onChange={(e) => setSize(e.target.value)}>
              <option value="same">Same as show</option>
              <option value="1280x720">1280×720</option>
              <option value="960x540">960×540</option>
            </select>
          )}
        </Prop>
      )}
      <Prop label="Frame rate" hint="Half sends every second frame. Other rates are not offered because converting them judders.">
        {(id) => (
          <select id={id} className="ctl" value={rate} onChange={(e) => setRate(e.target.value as 'same' | 'half')}>
            <option value="same">Same as show</option>
            <option value="half">Half rate</option>
          </select>
        )}
      </Prop>
    </Modal>
  );
}
```

`ui/src/dialogs/QuitDialog.tsx`:
```tsx
import { useEffect, useState } from 'react';

import { onShellMessage, postToShell } from '../bridge';
import { ConfirmDialog } from './ConfirmDialog';

// The tray's Quit… asks the page; the operator confirms with a verb (spec §2).
export function QuitDialog() {
  const [open, setOpen] = useState(false);
  useEffect(() => onShellMessage((m) => m.type === 'confirmQuit' && setOpen(true)), []);
  if (!open) return null;
  return (
    <ConfirmDialog
      title="Quit BreadCamera?"
      body="Every output stops now and cameras on air lose picture. The engine shuts down."
      confirm="Quit and stop all outputs"
      onCancel={() => setOpen(false)}
      onConfirm={() => postToShell({ type: 'quitConfirmed' })}
    />
  );
}
```

- [ ] **Step 9: The workspace, the app and the styles**

`ui/src/App.tsx`:
```tsx
import { useEffect, useMemo, useRef, useState } from 'react';

import { EngineClient } from './api/client';
import { useEngine } from './api/useEngine';
import { ErrorBlock } from './components/ErrorBlock';
import { AddCameraDialog } from './dialogs/AddCameraDialog';
import { AddOutputDialog } from './dialogs/AddOutputDialog';
import { ConfirmDialog } from './dialogs/ConfirmDialog';
import { QuitDialog } from './dialogs/QuitDialog';
import { Inspector } from './inspector/Inspector';
import { Multiview, type Layout } from './multiview/Multiview';
import { StatusBar } from './shell/StatusBar';
import { TitleBar } from './shell/TitleBar';
import { Toolbar } from './shell/Toolbar';
import { postToShell } from './bridge';
import { nextMarks, type Marks } from './state/marks';

function engineUrl(): string | null {
  const p = new URLSearchParams(location.hash.slice(1));
  const port = p.get('port');
  const token = p.get('token');
  return port && token ? `ws://127.0.0.1:${port}/?token=${encodeURIComponent(token)}` : null;
}

// Everything below the engine connection; tests render this with a fake client.
export function Workspace({ client }: { client: EngineClient }) {
  const snap = useEngine(client);
  const cameras = snap.show?.cameras ?? [];
  const offline = snap.status !== 'open';
  const [selected, setSelected] = useState<string | null>(null);
  const [layout, setLayout] = useState<Layout>('auto');
  const [dialog, setDialog] = useState<null | 'addCamera' | 'addOutput' | 'stopAll'>(null);
  const [now, setNow] = useState(() => Date.now());
  const marks = useRef<Record<string, Marks>>({});

  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 1000);
    return () => clearInterval(t);
  }, []);
  useEffect(() => {
    if (selected && cameras.some((c) => c.id === selected)) return;
    setSelected(cameras[0]?.id ?? null);
  }, [cameras, selected]);
  useEffect(() => {
    const t = Date.now();
    for (const c of cameras) marks.current[c.id] = nextMarks(marks.current[c.id], snap.state.cameras?.[c.id], t);
  }, [snap.state, cameras]);
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.ctrlKey && e.shiftKey && e.key.toLowerCase() === 'n') {
        e.preventDefault();
        setDialog('addCamera');
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  function select(id: string) {
    setSelected(id);
    if (marks.current[id]) marks.current[id] = { ...marks.current[id], recoveredAt: undefined };
  }

  const index = cameras.findIndex((c) => c.id === selected);
  const cam = index >= 0 ? cameras[index] : undefined;
  const holdMs = snap.show?.settings?.holdMs ?? 5000;
  const menus = useMemo(
    () => ({
      file: [{ label: 'Quit…', onSelect: () => postToShell({ type: 'window', action: 'close' }) }],
      camera: [
        { label: 'Add camera…', shortcut: 'Ctrl+Shift+N', onSelect: () => setDialog('addCamera') },
        { label: 'Add output…', disabled: !cam, onSelect: () => setDialog('addOutput') },
      ],
      view: (['auto', '2x2', '3x2', '4x2'] as Layout[]).map((l) => ({ label: `Layout ${l === 'auto' ? 'Auto' : l.replace('x', '×')}`, onSelect: () => setLayout(l) })),
      help: [{ label: 'BreadCamera 0.1', disabled: true, onSelect: () => {} }],
    }),
    [cam],
  );

  return (
    <div className="win">
      <TitleBar menus={menus} />
      <Toolbar onAddCamera={() => setDialog('addCamera')} onStopAll={() => setDialog('stopAll')} />
      <div className="body">
        <Multiview
          client={client}
          cameras={cameras}
          state={snap.state}
          marks={marks.current}
          holdMs={holdMs}
          now={now}
          selected={selected}
          onSelect={select}
          layout={layout}
          onLayout={setLayout}
          offline={offline}
        />
        <div className="splitter" />
        <Inspector client={client} snap={snap} cam={cam} n={index + 1} onAddOutput={() => setDialog('addOutput')} onRemoved={() => setSelected(null)} />
      </div>
      <StatusBar snap={snap} />
      {dialog === 'addCamera' && <AddCameraDialog client={client} onClose={() => setDialog(null)} />}
      {dialog === 'addOutput' && cam && <AddOutputDialog client={client} camera={cam} n={index + 1} onClose={() => setDialog(null)} />}
      {dialog === 'stopAll' && (
        <ConfirmDialog
          title="Stop all outputs?"
          body="Every receiver loses picture now. The engine and the show stay open; switch outputs back on in each camera's Inspector."
          confirm="Stop all outputs"
          onCancel={() => setDialog(null)}
          onConfirm={() => {
            client.request('stopAllOutputs').catch(() => {});
            setDialog(null);
          }}
        />
      )}
      <QuitDialog />
    </div>
  );
}

function Connected({ url }: { url: string }) {
  const client = useMemo(() => new EngineClient(url), [url]);
  useEffect(() => {
    client.connect();
    return () => client.close();
  }, [client]);
  return <Workspace client={client} />;
}

export function App() {
  const url = engineUrl();
  if (!url) return <ErrorBlock message="Engine connection details are missing. Open BreadCamera from its app icon." />;
  return <Connected url={url} />;
}
```
Note `Workspace` stores marks in a ref and re-renders every second through `now`, so the countdown and marks stay current without extra state.

`ui/src/main.tsx`:
```tsx
import '@bmsolutionsvn/design/bm.css';
import '@bmsolutionsvn/design/fonts';
import './app.css';

import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import { App } from './App';

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
```

`ui/src/app.css` — the window styles, ported from the approved mockup (`docs/design/mockups/index.html`) onto the `--bm-*` tokens:
```css
:root {
  --ui: 'Segoe UI Variable Text', 'Segoe UI Variable', 'Segoe UI', system-ui, sans-serif;
  --mono: var(--bm-font-mono);
  --well: #050607;
  --ease: cubic-bezier(.2, 0, 0, 1);
}
* { box-sizing: border-box; margin: 0; }
html, body, #root { height: 100%; }
body { background: var(--bm-bg); color: var(--bm-text); font: 400 12px/1.4 var(--ui); caret-color: var(--bm-brand); overflow: hidden; user-select: none; }
input, select, textarea { user-select: text; }
::selection { background: var(--bm-brand-wash); color: var(--bm-text); }
::-webkit-scrollbar { width: 10px; height: 10px; }
::-webkit-scrollbar-thumb { background: var(--bm-border-strong); border-radius: 999px; border: 3px solid var(--bm-surface); }
::-webkit-scrollbar-track { background: transparent; }
button { font: inherit; color: inherit; background: none; border: none; cursor: default; }
:focus-visible { outline: 1px solid var(--bm-brand); outline-offset: -1px; }
svg.i { flex: none; fill: none; stroke: currentColor; stroke-width: 1.4; stroke-linecap: round; stroke-linejoin: round; }
.mono, .ro { font-family: var(--mono); font-feature-settings: 'tnum' 1, 'lnum' 1; }

/* window */
.win { height: 100%; display: grid; grid-template-rows: 32px 38px minmax(0, 1fr) 24px; background: var(--bm-bg); position: relative; }
.titlebar { display: flex; align-items: stretch; background: var(--bm-rail); app-region: drag; }
.titlebar .menus, .titlebar .caption, .titlebar .app-ic { app-region: no-drag; }
.app-ic { width: 40px; display: grid; place-items: center; color: var(--bm-brand); }
.menus { display: flex; align-items: stretch; }
.menu-root { position: relative; display: flex; }
.menu-btn { padding: 0 9px; color: var(--bm-text-muted); }
.menu-btn:hover, .menu-btn.open { background: var(--bm-surface-hover); color: var(--bm-text); }
.wintitle { flex: 1; display: grid; place-items: center; color: var(--bm-text-dim); }
.caption { display: flex; }
.caption button { width: 46px; display: grid; place-items: center; color: var(--bm-text-muted); }
.caption button:hover { background: var(--bm-surface-hover); color: var(--bm-text); }
.caption button.close:hover { background: #C42B1C; color: #fff; }
.menu { position: absolute; top: 100%; left: 0; z-index: 20; min-width: 240px; background: var(--bm-surface-raised); border: 1px solid var(--bm-border-strong); border-radius: 6px; padding: 4px; box-shadow: var(--bm-shadow-overlay); display: grid; }
.menu .it { height: 28px; display: grid; grid-template-columns: 1fr auto; align-items: center; gap: 16px; padding: 0 10px; border-radius: 4px; text-align: left; }
.menu .it:hover:not(:disabled) { background: var(--bm-surface-hover); }
.menu .it:disabled { color: var(--bm-text-disabled); }
.menu .it .k { font: 400 11px var(--mono); color: var(--bm-text-dim); }
.menu .it.danger { color: var(--bm-danger); }
.menu hr { border: none; border-top: 1px solid var(--bm-border); margin: 4px 2px; }

.toolbar { display: flex; align-items: center; gap: 4px; padding: 0 8px; border-bottom: 1px solid var(--bm-border); }
.tb { height: 28px; padding: 0 9px; display: inline-flex; align-items: center; gap: 6px; border-radius: 4px; border: 1px solid transparent; white-space: nowrap; }
.tb:hover { background: var(--bm-surface-hover); border-color: var(--bm-border); }
.tb svg.i { color: var(--bm-text-muted); }
.tb.accent svg.i { color: var(--bm-brand); }
.tb.danger, .tb.danger svg.i { color: var(--bm-danger); }
.tb.sm { height: 22px; padding: 0 6px; color: var(--bm-text-muted); }
.tb.sm svg.i { color: var(--bm-brand); }
.tsep { width: 1px; height: 20px; background: var(--bm-border); margin: 0 4px; }
.tspace { flex: 1; }

.body { display: grid; grid-template-columns: minmax(0, 1fr) 1px 360px; min-height: 0; }
.splitter { background: var(--bm-border); }
.center { display: grid; grid-template-rows: 30px minmax(0, 1fr); min-width: 0; min-height: 0; background: var(--well); }
.subbar { display: flex; align-items: center; gap: 10px; padding: 0 10px; border-bottom: 1px solid var(--bm-border-subtle); background: var(--bm-bg); color: var(--bm-text-dim); }
.subbar b { color: var(--bm-text); font-weight: 600; }
.subbar .right { margin-left: auto; display: flex; gap: 8px; align-items: center; }
.seg { display: inline-flex; border: 1px solid var(--bm-border); border-radius: 4px; overflow: hidden; }
.seg button { height: 22px; padding: 0 8px; color: var(--bm-text-muted); border-left: 1px solid var(--bm-border); }
.seg button:first-child { border-left: none; }
.seg button.on { background: var(--bm-surface-raised); color: var(--bm-text); }

/* multiview */
.wall-wrap { display: flex; flex-direction: column; min-height: 0; height: 100%; }
.wall-area { flex: 1; min-height: 0; display: grid; place-items: center; padding: 4px; }
.wall { display: grid; gap: 3px; outline: none; }
.tile { display: flex; flex-direction: column; background: var(--bm-surface); min-width: 0; overflow: hidden; }
.tile.sel { outline: 1px solid var(--bm-brand); outline-offset: 1px; }
.pic { aspect-ratio: 16 / 9; background: #000; overflow: hidden; }
.pic canvas { display: block; width: 100%; height: 100%; }
.strip { height: 28px; display: flex; align-items: center; gap: 7px; padding: 0 7px; border-top: 1px solid var(--bm-border); min-width: 0; }
.tile:hover .strip { background: var(--bm-surface-hover); }
.strip.err { box-shadow: inset 2px 0 0 var(--bm-danger); background: var(--bm-danger-wash); }
.in-chip { height: 18px; min-width: 36px; padding: 0 5px; border-radius: 3px; display: inline-grid; place-items: center; font: 600 10.5px var(--mono); letter-spacing: .05em; color: var(--bm-text-on-brand); flex: none; }
.in-chip.ready { background: var(--bm-signal-ready); }
.in-chip.conn { background: var(--bm-signal-connecting); }
.in-chip.err { background: var(--bm-danger); color: #2A0A10; }
.in-chip.off { background: var(--bm-signal-offline); color: var(--bm-text); }
.cam-no { font: 500 11px var(--mono); color: var(--bm-text-dim); flex: none; }
.cam-name { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-weight: 600; font-size: 12.5px; }
.fx { display: flex; gap: 3px; color: var(--bm-text-muted); flex: none; }
.fx .bad { color: var(--bm-danger); }
.flow { color: var(--bm-text-disabled); flex: none; display: grid; }
.outs { display: flex; gap: 3px; flex: none; }
.pill { height: 18px; padding: 0 6px 0 5px; border-radius: 999px; border: 1px solid; display: inline-flex; align-items: center; gap: 4px; font: 500 10.5px var(--mono); letter-spacing: .05em; white-space: nowrap; }
.pill i { width: 6px; height: 6px; border-radius: 50%; }
.pill.live { color: var(--bm-signal-live-text); border-color: rgba(239, 68, 68, .55); }
.pill.live i { background: var(--bm-signal-live); animation: bm-live-pulse 2s ease-in-out infinite; }
.pill.connecting { color: var(--bm-signal-connecting-text); border-color: rgba(245, 158, 11, .55); }
.pill.connecting i { background: var(--bm-signal-connecting); }
.pill.ready { color: var(--bm-signal-ready-text); border-color: rgba(34, 197, 94, .55); }
.pill.ready i { background: var(--bm-signal-ready); }
.pill.offline { color: var(--bm-signal-offline-text); border-color: var(--bm-border-strong); }
.pill.offline i { background: var(--bm-signal-offline); }
.err-chip { height: 18px; padding: 0 5px; border-radius: 3px; border: 1px solid rgba(251, 113, 133, .6); background: var(--bm-danger-wash); color: var(--bm-danger); display: inline-flex; align-items: center; gap: 3px; font: 500 10.5px var(--mono); letter-spacing: .05em; }
.more { font: 500 10.5px var(--mono); color: var(--bm-text-muted); }
.seen { display: inline-flex; align-items: center; gap: 3px; color: var(--bm-info); font: 500 10.5px var(--mono); flex: none; }

.block { display: flex; gap: 8px; align-items: flex-start; padding: 7px 10px; border-left: 2px solid; }
.block b { font-weight: 600; }
.block .d { color: var(--bm-text-muted); }
.block.danger { border-color: var(--bm-danger); background: var(--bm-danger-wash); }
.block.danger svg { color: var(--bm-danger); }
.block.warning { border-color: var(--bm-warning); background: var(--bm-warning-wash); }
.block.warning svg { color: var(--bm-warning); }
.block.info { border-color: var(--bm-info); background: var(--bm-info-wash); }
.block.info svg { color: var(--bm-info); }

.empty { flex: 1; display: grid; place-items: center; text-align: center; color: var(--bm-text-muted); }
.empty > div { display: grid; gap: 6px; justify-items: center; max-width: 360px; }
.empty .frame { width: 140px; aspect-ratio: 16 / 9; border: 1px dashed var(--bm-border-strong); border-radius: 4px; display: grid; place-items: center; color: var(--bm-text-dim); margin-bottom: 6px; }
.empty .h { color: var(--bm-text); font-size: 14px; font-weight: 600; }

/* inspector */
.insp { background: var(--bm-surface); display: grid; grid-template-rows: auto minmax(0, 1fr); min-height: 0; }
.insp-h { padding: 8px 12px; border-bottom: 1px solid var(--bm-border); display: grid; gap: 3px; }
.insp-h .k { font: 500 10px var(--mono); letter-spacing: .1em; color: var(--bm-text-dim); }
.insp-h .t { display: flex; align-items: center; gap: 8px; font-size: 14px; font-weight: 600; min-width: 0; }
.insp-h .t.dim { color: var(--bm-text-dim); }
.insp-h .n { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.insp-b { overflow: auto; }
details.sec { border-bottom: 1px solid var(--bm-border); }
details.sec > summary { list-style: none; height: 30px; display: flex; align-items: center; gap: 6px; padding: 0 8px 0 6px; font-weight: 600; background: var(--bm-bg); }
details.sec > summary::-webkit-details-marker { display: none; }
details.sec > summary .chev { color: var(--bm-text-dim); display: grid; transition: transform 120ms var(--ease); }
details.sec[open] > summary .chev { transform: rotate(90deg); }
details.sec > summary .right { margin-left: auto; display: flex; align-items: center; gap: 4px; color: var(--bm-text-dim); font-weight: 400; }
.sec-b { padding: 8px 12px 10px; display: grid; gap: 6px; }
.prop { display: grid; grid-template-columns: 104px minmax(0, 1fr); align-items: center; gap: 8px; min-height: 26px; }
.prop > label { color: var(--bm-text-dim); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.prop-c { display: grid; gap: 3px; min-width: 0; }
.hint { font-size: 11.5px; color: var(--bm-text-dim); }
.ctl { height: 26px; width: 100%; border-radius: 4px; background: var(--bm-bg); border: 1px solid var(--bm-border); color: var(--bm-text); padding: 0 8px; font: 400 12px var(--ui); min-width: 0; }
.ctl.mono { font: 400 11.5px var(--mono); }
.ctl:hover { border-color: var(--bm-border-strong); }
.ctl:focus { border-color: var(--bm-brand); outline: none; }
.ctl[aria-invalid='true'] { border-color: var(--bm-danger); }
select.ctl { appearance: auto; }
.note { color: var(--bm-text-dim); font-size: 11.5px; }
.pad { padding: 10px 12px; }
.row-end { display: flex; justify-content: flex-end; }
.ibtn { width: 24px; height: 24px; border-radius: 4px; display: grid; place-items: center; color: var(--bm-text-muted); border: 1px solid transparent; }
.ibtn:hover { background: var(--bm-surface-hover); border-color: var(--bm-border); color: var(--bm-text); }
.orow { display: grid; grid-template-columns: auto minmax(0, 1fr) auto 24px; gap: 8px; align-items: center; padding: 6px 0; border-bottom: 1px solid var(--bm-border-subtle); }
.orow:last-child { border-bottom: none; }
.o-main { min-width: 0; }
.orow .n { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.orow .m { font: 400 11px var(--mono); color: var(--bm-text-dim); }
.sw-wrap { position: relative; display: inline-flex; }
.sw-wrap input { position: absolute; inset: 0; opacity: 0; margin: 0; }
.sw { width: 28px; height: 16px; border-radius: 999px; background: var(--bm-border-strong); position: relative; }
.sw::after { content: ''; position: absolute; top: 2px; left: 2px; width: 12px; height: 12px; border-radius: 50%; background: var(--bm-text); transition: left 120ms var(--ease); }
.sw-wrap input:checked + .sw { background: var(--bm-brand); }
.sw-wrap input:checked + .sw::after { left: 14px; background: var(--bm-text-on-brand); }
.sw-wrap input:focus-visible + .sw { outline: 1px solid var(--bm-brand); outline-offset: 2px; }
.btn { height: 26px; padding: 0 12px; border-radius: 4px; border: 1px solid var(--bm-border-strong); background: var(--bm-surface-raised); color: var(--bm-text); display: inline-flex; align-items: center; justify-content: center; gap: 6px; white-space: nowrap; }
.btn:hover:not(:disabled) { background: var(--bm-surface-hover); }
.btn:disabled { color: var(--bm-text-disabled); }
.btn.primary { background: var(--bm-brand); border-color: var(--bm-brand); color: var(--bm-text-on-brand); font-weight: 600; }
.btn.primary:hover:not(:disabled) { background: var(--bm-brand-hover); }
.btn.primary:disabled { opacity: .5; }
.btn.danger { background: var(--bm-danger); border-color: var(--bm-danger); color: #2A0A10; font-weight: 600; }
.btn.ghost-danger { background: none; border-color: transparent; color: var(--bm-danger); padding: 0 4px; }

/* status bar */
.status { display: flex; align-items: center; background: var(--bm-rail); border-top: 1px solid var(--bm-border); font: 400 11px var(--mono); color: var(--bm-text-dim); min-width: 0; overflow: hidden; }
.status > span { display: flex; align-items: center; gap: 6px; padding: 0 10px; height: 100%; border-right: 1px solid var(--bm-border-subtle); white-space: nowrap; }
.status > span.r { margin-left: auto; border-left: 1px solid var(--bm-border-subtle); }
.status .dot { width: 6px; height: 6px; border-radius: 50%; background: var(--bm-signal-ready); }
.status .dot.warn { background: var(--bm-warning); }
.status .w { color: var(--bm-warning); }
.status .e { color: var(--bm-danger); }
.status b { color: var(--bm-text-muted); font-weight: 500; }

/* dialogs */
.scrim { position: fixed; inset: 0; background: rgba(0, 0, 0, .5); display: grid; place-items: center; z-index: 30; }
.dlg { width: 480px; max-width: calc(100% - 32px); background: var(--bm-surface-raised); border: 1px solid var(--bm-border-strong); border-radius: 6px; box-shadow: var(--bm-shadow-overlay); overflow: hidden; }
.dlg.wide { width: 680px; }
.dlg-h { height: 36px; display: flex; align-items: center; padding: 0 4px 0 14px; font-weight: 600; font-size: 13px; }
.dlg-h .x { margin-left: auto; width: 32px; height: 28px; display: grid; place-items: center; color: var(--bm-text-muted); border-radius: 4px; }
.dlg-h .x:hover { background: #C42B1C; color: #fff; }
.dlg-b { padding: 6px 14px 14px; display: grid; gap: 8px; color: var(--bm-text-muted); }
.dlg-b .prop { grid-template-columns: 120px minmax(0, 1fr); }
.dlg-f { display: flex; justify-content: flex-end; gap: 6px; padding: 10px 14px; background: var(--bm-surface); border-top: 1px solid var(--bm-border); }
.dlg-f .btn { min-width: 84px; }

@media (prefers-reduced-motion: reduce) { .pill.live i { animation: none; } }
```

- [ ] **Step 10: Run the UI tests and build**

Run in `ui/`: `PATH="/c/Program Files/nodejs:$PATH" npm test` — Expected: 7 files, all tests pass. Then `PATH="/c/Program Files/nodejs:$PATH" npm run build` — Expected: `ui/dist/index.html` and assets, no type errors.

- [ ] **Step 11: Ship `ui/dist` next to the shell**

`cmake/CopyUi.cmake`:
```cmake
# Copies the built UI next to BreadCamera.exe when it exists (ui/ is built by npm, not CMake).
if(EXISTS "${SRC}/index.html")
  file(COPY "${SRC}/" DESTINATION "${DST}")
endif()
```
In `CMakeLists.txt`, after the `BreadCamera` target:
```cmake
add_custom_command(TARGET BreadCamera POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR}/ui/dist -DDST=$<TARGET_FILE_DIR:BreadCamera>/ui
          -P ${CMAKE_SOURCE_DIR}/cmake/CopyUi.cmake)
```
Run `scripts/build.ps1`; expected `build/release/ui/index.html` exists.


- [ ] **Step 12: Manual-path evidence (automated stand-in)**

Start `build\release\BreadCamera.exe`, wait 5 s, then take a screenshot of the window to `%TEMP%\bcam-m1-ui.png` (PowerShell: `Add-Type -AssemblyName System.Windows.Forms,System.Drawing`, capture `[System.Windows.Forms.Screen]::PrimaryScreen.Bounds`) and attach its path in the report. Expected, compared with the "Empty" tab of `docs/design/mockups/index.html`: the dark custom title bar with the menus and the minimise/maximise/close buttons (no system caption), the toolbar with Add camera and Stop all outputs…, the empty Multiview, the Inspector saying "No camera selected", and the status bar with the GPU name. Drag the window by the title bar and resize it from the right edge once; note both in the report. Then stop both processes (`Get-Process BreadCamera, breadcamera-engine | Stop-Process -Force`).

- [ ] **Step 13: Commit**

```bash
git add ui/package.json ui/package-lock.json ui/.npmrc ui/tsconfig.json ui/vite.config.ts ui/index.html ui/src cmake/CopyUi.cmake CMakeLists.txt
git commit -m "feat(ui): native window shell with Multiview, Inspector, status bar and dialogs"
```

### Task 14: M1 acceptance — 4× 1080p60 passthrough, hold/slate under load, crash restart, follow-ups

**Files:**
- Create: `tests/e2e_m1_test.cpp`, `docs/superpowers/follow-ups.md`
- Modify: `CMakeLists.txt` (test file)

**Interfaces:**
- Consumes: `Engine`, `DefaultAdapterFactory`, `AdapterFactory` (Task 9); `FakeFactory`/`FakeOutput` (Tasks 7, 9 helpers); `writeTestTs`, `TsPusher` (Task 6); `ProcessLauncher`, `Supervisor` (Task 12); `ShowStore` (Task 3); `breadcamera-engine.exe` (Task 11).
- Produces: the spec §9 M1 "done when" evidence: **4× 1080p60 passthrough with the input-loss hold and slate working**, plus the §8.4 engine-kill check.

- [ ] **Step 1: Write the acceptance tests**

`tests/e2e_m1_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <thread>

#include "core/show_store.h"
#include "core/state_store.h"
#include "engine/engine.h"
#include "gpu/device.h"
#include "shell/process_launcher.h"
#include "shell/supervisor.h"
#include "support/fake_input.h"
#include "support/requires.h"
#include "support/temp_dir.h"
#include "support/test_media.h"

using namespace std::chrono_literals;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {
// Real SRT inputs, recording fake outputs.
struct HybridFactory : bc::AdapterFactory {
  bc::DefaultAdapterFactory real;
  bc::test::FakeFactory fake;
  HybridFactory(bc::GpuDevice& g, bc::TexturePool& p, bc::Converter& c) : real(g, p), fake(c) {}
  std::unique_ptr<bc::Input> makeInput(const bc::InputConfig& cfg) override { return real.makeInput(cfg); }
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig& cfg, const bc::CameraConfig& cam) override {
    return fake.makeOutput(cfg, cam);
  }
};

std::string phase(bc::StateStore& s, const std::string& id) {
  auto p = json::json_pointer("/cameras/" + id + "/phase");
  auto snap = s.snapshot();
  return snap.contains(p) ? snap.at(p).get<std::string>() : "";
}

int countSince(bc::test::FakeOutput* o, Clock::time_point a, Clock::time_point b) {
  int n = 0;
  for (auto& s : o->seen()) n += (s.at >= a && s.at < b) ? 1 : 0;
  return n;
}

std::filesystem::path exeDir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}
}  // namespace

TEST_CASE("M1 acceptance: 4x 1080p60 SRT passthrough, then hold and slate on one camera" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  bc::test::TempDir dir;
  const auto clip = dir.path / "1080p60.ts";
  bc::test::writeTestTs(clip, 1920, 1080, 60, 10);

  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::StateStore state;
  HybridFactory factory(*gpu, pool, conv);
  bc::Engine engine(conv, state, factory);

  bc::Show show;
  show.settings.holdMs = 2000;
  for (int i = 0; i < 4; ++i) {
    bc::CameraConfig c;
    c.id = "cam" + std::to_string(i);
    c.name = "Cam " + std::to_string(i);
    c.input.kind = "srt";
    c.input.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47911 + i}, {"latencyMs", 120}};
    c.outputs.push_back({"out" + std::to_string(i), "fake", false, true, {}});
    show.cameras.push_back(c);
  }
  engine.apply(show);
  engine.start();
  std::vector<std::unique_ptr<bc::test::TsPusher>> pushers;
  for (int i = 0; i < 4; ++i) {
    pushers.push_back(std::make_unique<bc::test::TsPusher>(clip, "srt://127.0.0.1:" + std::to_string(47911 + i) + "?mode=caller", true));
  }

  // Warm up: every camera live.
  auto warm = Clock::now() + 20s;
  auto allLive = [&] {
    for (int i = 0; i < 4; ++i) if (phase(state, "cam" + std::to_string(i)) != "live") return false;
    return true;
  };
  while (!allLive() && Clock::now() < warm) std::this_thread::sleep_for(100ms);
  REQUIRE(allLive());
  std::this_thread::sleep_for(3s);  // let latency/audio settle

  // 20 s under full load.
  const uint64_t overruns0 = engine.stats().overruns;
  const auto t0 = Clock::now();
  std::this_thread::sleep_for(20s);
  const auto t1 = Clock::now();
  const auto st = engine.stats();
  CHECK(st.overruns - overruns0 == 0);
  for (int i = 0; i < 4; ++i) {
    const auto id = "cam" + std::to_string(i);
    auto* out = factory.fake.output("out" + std::to_string(i));
    const int n = countSince(out, t0, t1);
    MESSAGE(id << ": " << n << " frames in 20 s, latency " << st.cameras.at(id).latencyMs << " ms, fps " << st.cameras.at(id).fps);
    CHECK(n >= 1180);  // 60 fps × 20 s, ≤ 20 lost
    CHECK(st.cameras.at(id).latencyMs <= 50.0);
    CHECK(phase(state, id) == "live");
  }

  // Pull camera 0's source: hold, then slate; the others keep going.
  pushers[0]->stop();
  const auto cut = Clock::now();
  bool sawHolding = false;
  while (Clock::now() < cut + 5s) {
    const auto p = phase(state, "cam0");
    sawHolding = sawHolding || p == "holding";
    if (p == "slate") break;
    std::this_thread::sleep_for(20ms);
  }
  CHECK(sawHolding);
  CHECK(phase(state, "cam0") == "slate");
  const auto t2 = Clock::now();
  CHECK(countSince(factory.fake.output("out0"), cut, t2) >= static_cast<int>((t2 - cut) / 1ms / 17) - 10);  // never stopped
  CHECK(countSince(factory.fake.output("out1"), cut, t2) >= static_cast<int>((t2 - cut) / 1ms / 17) - 10);

  // The source comes back: live again.
  pushers[0] = std::make_unique<bc::test::TsPusher>(clip, "srt://127.0.0.1:47911?mode=caller", true);
  const auto back = Clock::now() + 15s;
  while (phase(state, "cam0") != "live" && Clock::now() < back) std::this_thread::sleep_for(100ms);
  CHECK(phase(state, "cam0") == "live");

  for (auto& p : pushers) p->stop();
  engine.stop();
}

TEST_CASE("M1 acceptance: a killed engine is back with its show in under 3 s") {
  bc::test::TempDir dir;
  {
    bc::ShowStore store(dir.path / "shows");
    bc::Show show;
    bc::CameraConfig c;
    c.id = "keepme01";
    c.name = "Kept";
    c.input.params = {{"mode", "listener"}, {"port", 47920}};
    show.cameras.push_back(c);
    store.saveNow(show);
  }
  const std::string token = "e2etoken0123456789ab";
  const int port = 47921;
  bc::ProcessLauncher launcher(exeDir() / "breadcamera-engine.exe",
                               L"--port " + std::to_wstring(port) + L" --token " + std::wstring(token.begin(), token.end()) +
                                   L" --show-dir \"" + (dir.path / "shows").wstring() + L"\" --discovery \"" +
                                   (dir.path / "engine.json").wstring() + L"\"");
  bc::Supervisor sup(launcher);
  std::atomic<bool> run{true};
  std::thread poller([&] {
    while (run) {
      sup.poll(Clock::now());
      std::this_thread::sleep_for(100ms);
    }
  });

  ix::initNetSystem();
  auto askShow = [&]() -> std::optional<json> {  // connects, asks getShow, returns the result or nothing
    ix::WebSocket ws;
    ws.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/?token=" + token);
    ws.disableAutomaticReconnection();
    std::mutex m;
    std::optional<json> result;
    std::atomic<bool> open{false};
    ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
      if (msg->type == ix::WebSocketMessageType::Open) open = true;
      if (msg->type != ix::WebSocketMessageType::Message) return;
      auto j = json::parse(msg->str, nullptr, false);
      if (j.is_object() && j.value("id", json()) == 1 && j.value("ok", false)) {
        std::lock_guard l(m);
        result = j["result"];
      }
    });
    ws.start();
    for (int i = 0; i < 20 && !open; ++i) std::this_thread::sleep_for(25ms);
    if (open) ws.send(R"({"id":1,"cmd":"getShow"})");
    for (int i = 0; i < 20; ++i) {
      {
        std::lock_guard l(m);
        if (result) break;
      }
      std::this_thread::sleep_for(25ms);
    }
    ws.stop();
    std::lock_guard l(m);
    return result;
  };

  auto until = Clock::now() + 10s;
  std::optional<json> first;
  while (!(first = askShow()) && Clock::now() < until) {}
  REQUIRE(first);
  CHECK((*first)["cameras"][0]["id"] == "keepme01");

  launcher.kill();  // simulated crash
  const auto killed = Clock::now();
  std::optional<json> again;
  while (!(again = askShow()) && Clock::now() < killed + 10s) {}
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - killed);
  MESSAGE("engine back after " << took.count() << " ms");
  REQUIRE(again);
  CHECK(took < 3000ms);
  CHECK((*again)["cameras"][0]["id"] == "keepme01");

  sup.stopSupervising();
  run = false;
  poller.join();
  launcher.kill();
}
```

Add it to `bc_tests` and make the test binary depend on the engine so the exe exists: `add_dependencies(bc_tests breadcamera-engine)`.

- [ ] **Step 2: Run the acceptance tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "M1 acceptance*"`
Expected: both PASS on the RX 6600. The log shows per-camera frame counts (≥ 1180), latency (≤ 50 ms) and "engine back after N ms" (< 3000). These numbers are the M1 evidence — paste them into the report.

If the 4× 1080p60 case fails on overruns or latency, do **not** loosen the bounds. Record the numbers, profile one tick (add a temporary `MESSAGE` of per-camera `tick()` time, then remove it) and report the hotspot as the finding; the planner decides.

- [ ] **Step 3: Write the follow-ups list**

`docs/superpowers/follow-ups.md`:
```markdown
# BreadCamera follow-ups

Deliberate shortcuts and open items. One line each, with where it lives.

## Untested on hardware
- `h264_nvenc` preview path — never opened on the AMD build PC (`src/engine/preview_encoder.cpp`).
- NDI output — only tested if the NDI SDK was installed on the build PC; see the M1 report (`src/io/ndi_output.cpp`).

## Known ceilings (ponytail)
- Readback maps synchronously under the context lock (`src/gpu/converter.cpp`, `Converter::readback`); double-buffer staging if the tick budget gets tight.
- Bilinear-only downscale for previews (`src/gpu/shaders/linear_to_bgra8.hlsl`).
- No audio clock-drift resampling; overflow trims (`src/core/audio_delay.h`).
- The audio resampler is built from the first frame only (`src/io/ffmpeg_input.cpp`, `emitAudio`).
- Fullscreen output stretches, no letterbox (`src/io/fullscreen_output.h`).
- `Converter::fit` is plain bilinear; add a box pre-filter if 1080p → 540p outputs shimmer (`src/gpu/shaders/fit_linear.hlsl`).
- After a tick overrun a half-rate output can wait one extra tick (`src/engine/camera.cpp`).
- Preview encoding runs on the tick thread (`src/engine/preview_service.h`).
- WebSocket sends happen under the client-map lock (`src/engine/control_server.cpp`).

## Spec deviations (authorised in M1)
- NDI alpha uses BGRA instead of UYVA (`src/io/ndi_output.cpp`).
- Preview sizes are 512×288 / 1280×720 instead of ~480×270 (`src/engine/preview_hub.h`).
- The UI ships beside the shell exe (`ui/` folder) instead of embedded in it (`CMakeLists.txt`, `cmake/CopyUi.cmake`).

## Elsewhere
- The UI uses `@bmsolutionsvn/design`'s tokens (`bm.css`) but not its MUI theme or `BmAppShell`: the native pro-tool window is BreadCamera's own (spec §6.4, `ui/src/app.css`).
- The title bar loses the top-edge resize handle (the side and bottom edges and Snap still resize) (`src/shell/shell_main.cpp`, `WM_NCCALCSIZE`).
```
Add any ledger `ponytail:` or deferred minor from this run to the list.

- [ ] **Step 4: Full verification**

Run, in order:
1. `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1` — the whole C++ suite; record passed/skipped counts and every skip reason.
2. In `ui/`: `PATH="/c/Program Files/nodejs:$PATH" npm test` and `npm run build`.
3. `$env:BC_FORCE_WARP='1'; .\build\release\bc_tests.exe` — record which cases skip under WARP and that the rest pass.

Expected: no failures. Paste all three summaries into the report.

- [ ] **Step 5: Commit**

```bash
git add tests/e2e_m1_test.cpp CMakeLists.txt docs/superpowers/follow-ups.md
git commit -m "test: M1 acceptance (4x1080p60 passthrough, hold/slate, crash restart) and follow-ups"
```
