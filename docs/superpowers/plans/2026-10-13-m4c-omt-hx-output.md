# BreadCamera M4c — OMT output on omthx (plain OMT and OMT-HX) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One BreadCamera output kind, `omt`, that publishes a camera over Open Media Transport in plain OMT (VMX) or OMT-HX (HEVC from the GPU hardware encoder), using the merged `omthx` library, with alpha and audio, a keyframe request when the output's own queue drops a frame, and tally published on the output's status.

**Architecture:** `OmthOutput` is a `QueuedOutput` (spec A4.1) that owns an `omthx::Sender`. On the worker thread `open()` creates the Sender with the mode from the show params; `deliver()` converts the tick's interleaved audio to planar and calls `sendVideo`/`sendAudio`, and calls `requestKeyframe()` when the drop-oldest queue dropped a frame since the last delivery. `OmthOutput::status()` carries the sender's tally back to the engine, which publishes it on the output's state node. This supersedes the plain-OMT `OmtOutput` sketched in M4a Task 3 — `omthx::Sender` already speaks both modes, so there is one adapter, not two.

**Tech Stack:** C++20, the `omthx` library (OMT-HX repo, merged), FFmpeg through omthx, CMake + Ninja + vcpkg, doctest, Windows PowerShell 5.1 scripts. Windows x64 only.

**Spec:** `docs/superpowers/specs/2026-10-08-breadcamera-design.md` (A4.3, A4.1), and the omthx design `../OMT-HX/docs/superpowers/specs/2026-10-12-omt-hx-design.md`.

## Global Constraints

- All BreadCamera Global Constraints apply: MSVC C++20 `/W4 /permissive- /utf-8 /EHsc`; outputs never block the tick; every networked output is a `QueuedOutput` (queue of 2, drop-oldest, own thread, retry with backoff); PowerShell 5.1 scripts.
- This PC has an AMD Radeon RX 6600 and no NVIDIA GPU. On this PC the encoder is `hevc_amf`; the mode `hx` is the one to exercise. Never claim `hevc_nvenc` works from a run on this PC.
- `omthx` is consumed as a CMake subproject of the OMT-HX checkout beside BreadCamera (`../OMT-HX`), after that checkout's `scripts/build-libomt.ps1` has populated `third_party/libomt/`. Do not copy omthx sources into BreadCamera.
- The public surface is `omthx/omthx.h` only. `OmthOutput` must not include `libomt.h` or `codec.h`.
- `omthx::Sender::sendVideo` wants BGRA straight (not premultiplied) when the app says `premultiplied == false`; BreadCamera's `CpuImage` is straight, so `deliver()` passes `premultiplied = false`.
- **This plan supersedes M4a Task 3 (the libomt plain-OMT `OmtOutput`).** Do not run M4a Task 3. If it already ran, Task 1 Step 1 below removes its artifacts first: `cmake/FindOMT.cmake`, `scripts/fetch-sdks.ps1`, its `find_package(OMT)`/`option(BC_WITH_OMT ...)` block, its `bc_io` `OMT::OMT` link, its `bc_copy_runtime` OMT block, and its `.gitignore` line. Two `BC_WITH_OMT` definitions and two `bc_io` OMT link blocks must never coexist.
- Tally is published on the output state; drawing the tally on the multiview tile is a follow-up (roadmap S2 / multiview), not this plan.
- Commit after each task with a `feat(io):` or `feat:` message, matching BreadCamera's history.

## Review Focus

1. **A receiver joins while the source runs** — omthx sends a keyframe on request and every 2 s; the adapter must not swallow the receiver's request. Owned by Task 3.
2. **The output queue drops a frame** — the tick ran ahead of the network. The adapter must ask for a keyframe so the far side recovers without waiting 2 s. Owned by Task 2 and Task 3.
3. **A show with mode `full` still works** — plain OMT (VMX) is one path through the same Sender. Owned by Task 3.
4. **Bad params from the UI or a hand-edited show file** — an unknown `mode` or a non-string `name` is refused with `invalid`, nothing is created. Owned by Task 4.
5. **Tally arrives back** — BreadStudio (or another receiver) sets program/preview and BreadCamera publishes it. Owned by Task 5.

## File Structure

| File | Responsibility |
|---|---|
| `CMakeLists.txt` | Consume omthx as a subproject; gate `BC_WITH_OMT`; add `src/io/omt_output.cpp`; copy `libomt.dll`/`libvmx.dll` |
| `src/io/queued_output.h/.cpp` | Add a drop signal the subclass consumes in `deliver()` |
| `src/io/omt_output.h/.cpp` | `OmthOutput`: the omthx-backed output (this plan's new file) |
| `src/io/output.h` | Add `program`/`preview` to `OutputStatus` |
| `src/engine/adapter_factory.cpp` | Create `OmthOutput` for kind `omt` |
| `src/engine/command_router.cpp` | `validateOutput` accepts kind `omt` |
| `src/engine/camera.cpp` | Publish the output tally on the state node |
| `tests/support/requires.h/.cpp` | `Req::Omt` |
| `tests/omt_output_test.cpp` | The adapter, end to end through an `omthx::Receiver` |

---

### Task 1: Consume omthx in the BreadCamera build

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `tests/support/requires.h`, `tests/support/requires.cpp`
- Create: `tests/omt_build_test.cpp`

**Interfaces:**
- Consumes: the OMT-HX checkout at `../OMT-HX` with `third_party/libomt/libomt.lib` present.
- Produces: CMake option `BC_WITH_OMT` (default on when the OMT-HX checkout has `include/omthx/omthx.h`); the `omthx` target linked into `bc_io`; `libomt.dll` and `libvmx.dll` copied beside an executable by `bc_copy_runtime`; `bc::test::Req::Omt` (missing when `BC_WITH_OMT == 0`).

- [x] **Step 1: Prerequisite — drop the superseded M4a Task 3, then build libomt once**

If M4a Task 3 was already implemented, remove its artifacts (this plan replaces it):
```powershell
git rm -f --ignore-unmatch cmake/FindOMT.cmake scripts/fetch-sdks.ps1
```
and hand-edit `CMakeLists.txt` to delete its `find_package(OMT)`, its `option(BC_WITH_OMT "..." ${OMT_FOUND})`, its `if(BC_WITH_OMT) ... target_link_libraries(bc_io PUBLIC OMT::OMT) ... endif()` block and its `bc_copy_runtime` OMT block; then delete the `/third_party/` line from `.gitignore`. Confirm with:
```powershell
rg -n "OMT::OMT|find_package\(OMT\)|fetch-sdks|third_party" CMakeLists.txt .gitignore
```
Expected: no hits. If M4a Task 3 never ran, skip this paragraph.

Build the patched libomt:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File ..\OMT-HX\scripts\build-libomt.ps1
```
Expected: `..\OMT-HX\third_party\libomt\libomt.lib` exists. If the OMT-HX checkout is missing, stop: this plan needs it (see the OMT-HX README).

- [x] **Step 2: Write the failing test**

Create `tests/omt_build_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_OMT
#include <string>
#include <vector>

#include "omthx/omthx.h"

TEST_CASE("omthx is linked and discovery runs without a transmitter" BC_REQUIRES(bc::test::Req::Omt)) {
  const std::vector<std::string> names = omthx::discover();  // may be empty; must not throw
  CHECK(names.size() < 10000);
}
#else
TEST_CASE("omthx is linked and discovery runs without a transmitter" BC_REQUIRES(bc::test::Req::Omt)) {}
#endif
```
Add `tests/omt_build_test.cpp` to the `bc_tests` source list in `CMakeLists.txt`.

- [x] **Step 3: Run it; expect a build failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "omthx is linked*"`
Expected: build FAILS — `BC_WITH_OMT` is undefined, so `#if BC_WITH_OMT` takes the `#else` branch, which names `bc::test::Req::Omt` (added in Step 5, not yet present).

- [x] **Step 4: Wire the subproject into CMake**

In `CMakeLists.txt`, after the NDI block (after `message(STATUS "BC_WITH_NDI=${BC_WITH_NDI}")`), add:
```cmake
# Optional OMT-HX output (see ../OMT-HX). The checkout must have run its
# scripts/build-libomt.ps1 once so third_party/libomt is populated.
set(OMTHX_ROOT "${CMAKE_SOURCE_DIR}/../OMT-HX" CACHE PATH "OMT-HX checkout")
if(EXISTS "${OMTHX_ROOT}/include/omthx/omthx.h" AND EXISTS "${OMTHX_ROOT}/third_party/libomt/libomt.lib")
  add_subdirectory("${OMTHX_ROOT}" omthx-build)
  set(BC_WITH_OMT ON)
else()
  set(BC_WITH_OMT OFF)
  message(STATUS "OMT output OFF: no built OMT-HX checkout at ${OMTHX_ROOT} (run its scripts/build-libomt.ps1)")
endif()
message(STATUS "BC_WITH_OMT=${BC_WITH_OMT}")
```
In the `bc_io` block, after the `BC_WITH_NDI` if/else, add:
```cmake
if(BC_WITH_OMT)
  target_sources(bc_io PRIVATE src/io/omt_output.cpp)
  target_link_libraries(bc_io PUBLIC omthx)
  target_compile_definitions(bc_io PUBLIC BC_WITH_OMT=1)
else()
  target_compile_definitions(bc_io PUBLIC BC_WITH_OMT=0)
endif()
```
Extend `bc_copy_runtime` (add before its closing `endfunction()`):
```cmake
  if(BC_WITH_OMT)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${OMTHX_LIBOMT_DIR}/libomt.dll" "${OMTHX_LIBOMT_DIR}/libvmx.dll" "$<TARGET_FILE_DIR:${target}>")
  endif()
```
(`OMTHX_LIBOMT_DIR` is a cache variable omthx sets to its own `third_party/libomt`; it is visible here.)

Note: Task 3 creates `src/io/omt_output.cpp`. Until then, keep the `target_sources(bc_io PRIVATE src/io/omt_output.cpp)` line but create a placeholder file in Step 5 so the configure does not fail.

- [x] **Step 5: Create the placeholder source and add the requirement**

Create `src/io/omt_output.cpp` containing only `#if BC_WITH_OMT\n#include "io/omt_output.h"\n#endif`.

In `tests/support/requires.h`, add `Omt` to `enum class Req`:
```cpp
enum class Req { HardwareGpu, Ndi, H264Encoder, Maxine, ChartDetect, Omt };
```
In `tests/support/requires.cpp`, add a case in `check(...)`:
```cpp
    case Req::Omt:
      reason = "OMT-HX checkout not built (BC_WITH_OMT=OFF)";
      return BC_WITH_OMT == 0;
```

- [x] **Step 6: Run the test; expect a pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "omthx is linked*"`
Expected: `1 passed` (the test body runs; `discover()` returns a list, typically empty).

- [x] **Step 7: Commit**

```powershell
git add CMakeLists.txt tests/support/requires.h tests/support/requires.cpp tests/omt_build_test.cpp src/io/omt_output.cpp
git commit -m "feat(io): link the omthx library into BreadCamera (BC_WITH_OMT)"
```

---

### Task 2: A drop signal on QueuedOutput

**Files:**
- Modify: `src/io/queued_output.h`, `src/io/queued_output.cpp`
- Test: `tests/queued_output_test.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `bool QueuedOutput::consumeDrop()` (protected) — true once for each frame the drop-oldest queue discarded since the last call; false otherwise.

- [x] **Step 1: Write the failing test**

Append to `tests/queued_output_test.cpp`:
```cpp
namespace {
// A delivery that sleeps long enough for the queue (capacity 2) to drop, then records drops.
class DropProbe : public bc::QueuedOutput {
 public:
  std::atomic<int> deliveries{0};
  std::atomic<int> dropsWithHistory{0};

 protected:
  void open() override {}
  void deliver(const bc::OutFrame&) override {
    ++deliveries;
    if (consumeDrop()) ++dropsWithHistory;  // a drop happened before this frame arrived
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }
  void close() noexcept override {}
};
}  // namespace

TEST_CASE("QueuedOutput reports a dropped frame to deliver()") {
  DropProbe out;
  out.start();
  for (int i = 0; i < 5; ++i) out.submit(bc::OutFrame{});
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  out.stop();
  CHECK(out.deliveries >= 1);
  CHECK(out.dropsWithHistory >= 1);  // the queue overflowed at least once
}
```
Ensure the file includes `<atomic>` and `<thread>` (add the includes if missing).

- [x] **Step 2: Run it; expect a link failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "QueuedOutput reports*"`
Expected: build FAILS — `consumeDrop` is not a member of `QueuedOutput`.

- [x] **Step 3: Implement the drop signal**

In `src/io/queued_output.h`, in the `protected:` section, add:
```cpp
  // Latches a drop: true at most once per delivery when the queue discarded a frame since the last call.
  bool consumeDrop() { return dropPending_.exchange(false); }
```
and add the member near the other members:
```cpp
  std::atomic<bool> dropPending_{false};
```
In `src/io/queued_output.cpp`, change `submit`:
```cpp
void QueuedOutput::submit(OutFrame frame) {
  if (queue_.push(std::move(frame))) dropPending_ = true;
}
```

- [x] **Step 4: Run it; expect a pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "QueuedOutput*"`
Expected: all queued-output tests pass, including the new one.

- [x] **Step 5: Commit**

```powershell
git add src/io/queued_output.h src/io/queued_output.cpp tests/queued_output_test.cpp
git commit -m "feat(io): tell a QueuedOutput when its queue dropped a frame"
```

---

### Task 3: The OmthOutput adapter

**Files:**
- Create: `src/io/omt_output.h`
- Modify (replace the placeholder): `src/io/omt_output.cpp`
- Create: `tests/omt_output_test.cpp`

**Interfaces:**
- Consumes: `QueuedOutput` (Task 2), `OutFrame`, and `omthx::Sender`.
- Produces: `bc::OmthOutput(std::string name, omthx::Mode mode)` — `needsCpu() == true`, error code `"omt-failed"`. Mode maps `"full"` → `omthx::Mode::Full`, anything else → `Hx`; the factory validates the string.

- [x] **Step 1: Write the failing test**

Create `tests/omt_output_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_OMT
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>

#include "io/omt_output.h"
#include "omthx/omthx.h"

using namespace std::chrono_literals;

namespace {
bc::OutFrame frameWith(std::shared_ptr<const bc::CpuImage> img, uint64_t tick, const bc::Rate& rate, bool alpha) {
  bc::OutFrame f;
  f.tick = tick;
  f.rate = rate;
  f.bgra = std::move(img);
  f.hasAlpha = alpha;
  return f;
}
}  // namespace

TEST_CASE("OmthOutput: an omthx receiver gets our picture with alpha" BC_REQUIRES(bc::test::Req::Omt)) {
  const std::string name = "BreadCamera OMT " + std::to_string(GetCurrentProcessId());
  bc::OmthOutput out(name, omthx::Mode::Hx);
  auto img = std::make_shared<bc::CpuImage>();
  img->width = 64;
  img->height = 36;
  img->bgra.resize(64 * 36 * 4);
  for (size_t i = 0; i < img->bgra.size(); i += 4) {
    img->bgra[i] = 0;      // B
    img->bgra[i + 1] = 0;  // G
    img->bgra[i + 2] = 200;  // R
    img->bgra[i + 3] = 128;  // A
  }
  out.start();
  const bc::Rate rate{30, 1};
  std::atomic<bool> run{true};
  std::thread feeder([&] {
    for (uint64_t t = 0; run; ++t) {
      out.submit(frameWith(img, t, rate, true));
      std::this_thread::sleep_for(33ms);
    }
  });

  // The address is published when the worker thread opens the sender; poll for it.
  std::string address;
  for (int i = 0; i < 50 && address.empty(); ++i) {
    address = out.address();
    if (address.empty()) std::this_thread::sleep_for(100ms);
  }
  REQUIRE_FALSE(address.empty());
  omthx::Receiver rx(address);
  omthx::VideoFrame vf;
  bool got = false;
  for (int i = 0; i < 60 && !got; ++i) {
    if (rx.receiveVideo(vf, 200)) got = vf.bgra.size() == 64u * 36 * 4;
  }
  run = false;
  feeder.join();
  out.stop();

  REQUIRE(got);
  CHECK(vf.format.width == 64);
  CHECK(vf.format.height == 36);
  CHECK(vf.format.alpha);
  CHECK(vf.premultiplied);  // the wire carries premultiplied alpha; the receiver says so
  // Straight (200,128) is premultiplied on the wire to about 200*128/255 = 100.
  const int r = vf.bgra[2];
  const int a = vf.bgra[3];
  CHECK(std::abs(r - 100) <= 25);
  CHECK(std::abs(a - 128) <= 20);
}

TEST_CASE("OmthOutput: full mode (plain OMT) also delivers a picture" BC_REQUIRES(bc::test::Req::Omt)) {
  const std::string name = "BreadCamera OMT full " + std::to_string(GetCurrentProcessId());
  bc::OmthOutput out(name, omthx::Mode::Full);
  auto img = std::make_shared<bc::CpuImage>();
  img->width = 64;
  img->height = 36;
  img->bgra.assign(64 * 36 * 4, 200);
  out.start();
  const bc::Rate rate{30, 1};
  std::atomic<bool> run{true};
  std::thread feeder([&] {
    for (uint64_t t = 0; run; ++t) {
      out.submit(frameWith(img, t, rate, false));
      std::this_thread::sleep_for(33ms);
    }
  });
  std::string address;
  for (int i = 0; i < 50 && address.empty(); ++i) {
    address = out.address();
    if (address.empty()) std::this_thread::sleep_for(100ms);
  }
  REQUIRE_FALSE(address.empty());
  omthx::Receiver rx(address);
  omthx::VideoFrame vf;
  bool got = false;
  for (int i = 0; i < 60 && !got; ++i) {
    if (rx.receiveVideo(vf, 200)) got = vf.format.width == 64 && vf.format.height == 36;
  }
  run = false;
  feeder.join();
  out.stop();
  REQUIRE(got);
}
#else
TEST_CASE("OmthOutput: an omthx receiver gets our picture with alpha" BC_REQUIRES(bc::test::Req::Omt)) {}
TEST_CASE("OmthOutput: full mode (plain OMT) also delivers a picture" BC_REQUIRES(bc::test::Req::Omt)) {}
#endif
```
The test needs `bc::OmthOutput::address()` (the OMT address of the source, as a `std::string` a receiver connects to). Add it to the interface. Add `tests/omt_output_test.cpp` to `bc_tests`.

- [x] **Step 2: Run it; expect a build failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "OmthOutput*"`
Expected: build FAILS — `io/omt_output.h` missing / `OmthOutput` undefined.

- [x] **Step 3: Implement the output**

`src/io/omt_output.h`:
```cpp
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "io/queued_output.h"
#include "omthx/omthx.h"

namespace bc {
// Open Media Transport sender (spec A4.3), plain OMT or OMT-HX, alpha when the frame has it.
class OmthOutput final : public QueuedOutput {
 public:
  OmthOutput(std::string name, omthx::Mode mode);
  ~OmthOutput() override { stop(); }
  bool needsCpu() const override { return true; }
  std::string address() const;  // "HOST (name)", for a receiver to connect to

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "omt-failed"; }

 private:
  std::string name_;
  omthx::Mode mode_;
  std::unique_ptr<omthx::Sender> send_;
  std::vector<float> planar_;
  omthx::VideoFormat format_;
  mutable std::mutex addrMutex_;  // guards address_, set by the worker in open()
  std::string address_;
};
}  // namespace bc
```

`src/io/omt_output.cpp` (replace the placeholder):
```cpp
#include "io/omt_output.h"

#include <stdexcept>

namespace bc {
OmthOutput::OmthOutput(std::string name, omthx::Mode mode) : name_(std::move(name)), mode_(mode) {}

std::string OmthOutput::address() const {
  std::lock_guard lock(addrMutex_);
  return address_;
}

void OmthOutput::open() {
  send_ = std::make_unique<omthx::Sender>(name_, mode_, omthx::Quality::Medium);
  std::lock_guard lock(addrMutex_);
  address_ = send_->address();
}

void OmthOutput::deliver(const OutFrame& f) {
  if (consumeDrop()) send_->requestKeyframe();  // the tick outran us: ask for a fresh keyframe

  if (f.bgra && !f.bgra->bgra.empty()) {
    format_.width = static_cast<int>(f.bgra->width);
    format_.height = static_cast<int>(f.bgra->height);
    format_.fpsN = static_cast<int>(f.rate.num);
    format_.fpsD = static_cast<int>(f.rate.den);
    format_.alpha = f.hasAlpha;
    const int64_t ts = static_cast<int64_t>(f.tick) * 10'000'000LL * f.rate.den / f.rate.num;
    // BreadCamera's BGRA is straight, so premultiplied == false.
    send_->sendVideo(format_, f.bgra->bgra, false, ts);
  }

  if (f.audio && !f.audio->empty()) {
    const int ch = f.channels;
    const int n = static_cast<int>(f.audio->size()) / ch;
    planar_.resize(f.audio->size());
    for (int c = 0; c < ch; ++c)
      for (int i = 0; i < n; ++i) planar_[static_cast<size_t>(c) * n + i] = (*f.audio)[static_cast<size_t>(i) * ch + c];
    const int64_t ts = static_cast<int64_t>(f.tick) * 10'000'000LL * f.rate.den / f.rate.num;
    send_->sendAudio(planar_, f.sampleRate, ch, ts);
  }
}

void OmthOutput::close() noexcept {
  send_.reset();
  std::lock_guard lock(addrMutex_);
  address_.clear();
}
}  // namespace bc
```

- [x] **Step 4: Run it; expect a pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "OmthOutput*"`
Expected: `2 passed` on this PC (encoder `hevc_amf`; full mode is software VMX). If `receiveVideo` never returns a frame within the loop, the keyframe request or the discovery is not working — debug before loosening the test.

- [x] **Step 5: Commit**

```powershell
git add src/io/omt_output.h src/io/omt_output.cpp tests/omt_output_test.cpp CMakeLists.txt
git commit -m "feat(io): OMT output on omthx, plain OMT and OMT-HX"
```

---

### Task 4: Kind `omt` in the router and the factory

**Files:**
- Modify: `src/engine/command_router.cpp` (`validateOutput`)
- Modify: `src/engine/adapter_factory.cpp` (`makeOutput`)
- Test: `tests/command_router_test.cpp`

**Interfaces:**
- Consumes: `OmthOutput` (Task 3).
- Produces: `validateOutput` and `DefaultAdapterFactory::makeOutput` accept `kind == "omt"`. Params: `mode` (`"hx"` default, or `"full"`), `name` (text, 1..200 chars, default `"BreadCamera - <camera name>"`).

- [x] **Step 1: Write the failing test**

Append to `tests/command_router_test.cpp`:
```cpp
TEST_CASE("output kind omt validates its params") {
  using nlohmann::json;
  CHECK_NOTHROW(bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt"}));
  CHECK_NOTHROW(bc::validateOutput(
      bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"mode", "full"}, {"name", "Cam"}}}));
  CHECK_THROWS_AS(
      bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"mode", "fast"}}}),
      std::invalid_argument);
  CHECK_THROWS_AS(
      bc::validateOutput(bc::OutputConfig{.id = "o1", .kind = "omt", .params = json{{"name", 5}}}),
      std::invalid_argument);
}
```
(If `validateOutput` is not already declared in a header the test includes, include the header that declares it; it is declared in `engine/command_router.h`.)

- [x] **Step 2: Run it; expect a failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "output kind omt*"`
Expected: FAIL — `unknown output kind 'omt'` (the second and third CHECKs may already pass; the first throws).

- [x] **Step 3: Implement validation**

In `src/engine/command_router.cpp`, in `validateOutput`, before the final `throw`, add:
```cpp
  if (o.kind == "omt") {
    if (o.params.contains("mode")) {
      if (!o.params["mode"].is_string()) throw std::invalid_argument("OMT mode must be text");
      const auto m = o.params["mode"].get<std::string>();
      if (m != "hx" && m != "full") throw std::invalid_argument("OMT mode must be 'hx' or 'full'");
    }
    if (o.params.contains("name")) {
      if (!o.params["name"].is_string()) throw std::invalid_argument("OMT name must be text");
      const auto n = o.params["name"].get<std::string>();
      if (n.empty() || utf8Length(n) > 200) throw std::invalid_argument("OMT name must be 1 to 200 characters");
    }
    return;
  }
```

- [x] **Step 4: Implement the factory**

In `src/engine/adapter_factory.cpp`, add the include (near the NDI one):
```cpp
#if BC_WITH_OMT
#include "io/omt_output.h"
#include "omthx/omthx.h"
#endif
```
and in `makeOutput`, before the final `throw`:
```cpp
  if (cfg.kind == "omt") {
#if BC_WITH_OMT
    if (cfg.params.contains("mode") && !cfg.params["mode"].is_string())
      throw std::invalid_argument("OMT mode must be text");
    if (cfg.params.contains("name") && !cfg.params["name"].is_string())
      throw std::invalid_argument("OMT name must be text");
    const std::string mode = cfg.params.value("mode", std::string("hx"));
    std::string name = cfg.params.value("name", std::string("BreadCamera - ") + cam.name);
    if (name.empty() || utf8Length(name) > 200) throw std::invalid_argument("OMT name must be 1 to 200 characters");
    return std::make_unique<OmthOutput>(name, mode == "full" ? omthx::Mode::Full : omthx::Mode::Hx);
#else
    (void)cam;
    throw std::invalid_argument("OMT support is not built (BC_WITH_OMT=OFF)");
#endif
  }
```

- [x] **Step 5: Run the tests; expect a pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "omt*,*output kind*"`
Expected: `validateOutput` tests pass; a factory test that builds an `OmthOutput` passes (add one mirroring the NDI factory test if the file has one; otherwise the validation test is the required one).

- [x] **Step 6: Commit**

```powershell
git add src/engine/command_router.cpp src/engine/adapter_factory.cpp tests/command_router_test.cpp
git commit -m "feat(engine): output kind omt, with mode hx|full"
```

---

### Task 5: Publish the output tally

**Files:**
- Modify: `src/io/output.h` (`OutputStatus`)
- Modify: `src/io/omt_output.h`, `src/io/omt_output.cpp` (override `status()`)
- Modify: `src/engine/camera.cpp` (publish the tally on the state node)
- Test: `tests/omt_output_test.cpp`

**Interfaces:**
- Consumes: `omthx::Sender::tally()`.
- Produces: `OutputStatus::program` and `OutputStatus::preview` (both default false); `OmthOutput::status()` fills them from the sender; the engine writes `outputs/<id>/tally` as JSON `{"program": bool, "preview": bool}`.

- [x] **Step 1: Write the failing test**

Append to `tests/omt_output_test.cpp` (inside the `#if BC_WITH_OMT` block):
```cpp
TEST_CASE("OmthOutput: status carries the tally, off until a receiver sets it" BC_REQUIRES(bc::test::Req::Omt)) {
  bc::OmthOutput out("BreadCamera OMT tally " + std::to_string(GetCurrentProcessId()), omthx::Mode::Hx);
  out.start();
  std::this_thread::sleep_for(300ms);
  const auto st = out.status();
  CHECK_FALSE(st.program);
  CHECK_FALSE(st.preview);
  out.stop();
}
```

- [x] **Step 2: Run it; expect a compile failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "status carries the tally*"`
Expected: build FAILS — `OutputStatus` has no member `program`.

- [x] **Step 3: Add the fields and fill them**

In `src/io/output.h`, in `struct OutputStatus`, add:
```cpp
  bool program = false;  // a downstream receiver has us on program
  bool preview = false;  // ... on preview
```
In `src/io/omt_output.h`, add `#include <atomic>` and, in the `public:` section:
```cpp
  OutputStatus status() const override;
```
and, among the members:
```cpp
  std::atomic<bool> program_{false};
  std::atomic<bool> preview_{false};
```
`OmthOutput::status()` must not touch `send_` from another thread: the worker publishes the tally into the atomics. In `src/io/omt_output.cpp`, in `deliver()` after the video/audio sends, add:
```cpp
  const omthx::Tally t = send_->tally();
  program_ = t.program;
  preview_ = t.preview;
```
and add:
```cpp
OutputStatus OmthOutput::status() const {
  OutputStatus s = QueuedOutput::status();
  s.program = program_;
  s.preview = preview_;
  return s;
}
```

- [x] **Step 4: Publish it in the engine**

In `src/engine/camera.cpp`, in the outputs loop (around the `auto st = s.out->status();` line), after setting `/state` and `/error`, add:
```cpp
      state_.set(p + "/tally", {{"program", st.program}, {"preview", st.preview}});
```

- [x] **Step 5: Run the tests; expect a pass**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "status carries the tally*,*state*"`
Expected: the tally test passes and no state-store test regresses.

- [x] **Step 6: Commit**

```powershell
git add src/io/output.h src/io/omt_output.h src/io/omt_output.cpp src/engine/camera.cpp tests/omt_output_test.cpp
git commit -m "feat(engine): publish each output's OMT tally on its state node"
```

---

### Task 6: Acceptance

**Files:**
- Modify: `docs/superpowers/plans/2026-10-11-remaining-milestones-opencode.md` (tick this milestone), `docs/superpowers/follow-ups.md` (note the multiview-tile follow-up)

- [x] **Step 1: Full suite**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1`
Expected: all tests pass (new OMT tests included; they run on this PC).

- [x] **Step 2: Two tools by hand**

Run `breadcamera-engine` with a show whose camera has an `omt` output (`mode` `hx`), and from the OMT-HX `build\release` connect `omthx-recv.exe --source "<THIS-PC> (BreadCamera - <camera>)"`. Expected: the receiver shows 60 fps HEVC at about 30 Mbps and, with alpha, `+alpha`; the output's `tally` node reads `{"program":false,"preview":false}` until a receiver sets it.

- [x] **Step 3: Record the follow-ups**

In `docs/superpowers/follow-ups.md`, add: "Draw the OMT tally on the multiview tile (reads `outputs/<id>/tally`)". In the milestones plan, mark M4c done and note that M4a Task 3 (libomt `OmtOutput`) is superseded by this plan.

- [x] **Step 4: Commit**

```powershell
git add docs
git commit -m "docs: note the OMT output milestone and its follow-ups"
```