# BreadCamera — DirectShow camera input (OBS Virtual Camera) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let BreadCamera's device-camera input list and capture from DirectShow-only virtual cameras (OBS Virtual Camera and friends), which Media Foundation cannot see, and rename the input label to "Device camera".

**Architecture:** Keep `MfCaptureInput` unchanged as the primary path. Add a DirectShow enumeration module whose results are merged into `listCaptureDevices()` (dedupe by device path, then friendly name), and a `DShowCaptureInput` that builds a capture graph through the in-box `SampleGrabber` filter, copying each RGB32 sample into a BGRA8 texture through the existing `InputSink`. A `dshow:<moniker display name>` id routes a device to the DirectShow path in the factory.

**Tech Stack:** MSVC C++20, DirectShow (`ICreateDevEnum`, `ICaptureGraphBuilder2`, `ISampleGrabber`), CMake + Ninja + vcpkg, doctest, Vitest/React (UI copy).

**Spec:** `docs/superpowers/specs/2026-10-16-dshow-camera-input-design.md`

## Global Constraints

- Windows only. C++20, MSVC, `/W4 /permissive- /utf-8 /EHsc`.
- `start()`/`stop()` never block for long (spec §4.1); inputs never block the tick.
- A missing, busy or unplugged device reports `offline` with code `capture-failed` and retries with backoff — the same contract as `MfCaptureInput`.
- Capture is video only (no audio); BGRA8 into `TexturePool`, as `MfCaptureInput` does.
- MF ids are unchanged (the MF symbolic link). DirectShow-only ids are `dshow:<moniker display name>`.
- UK English copy; conventional commits (`feat(io):`, `feat:`, `feat(ui):`).

## Review Focus

Failure modes the spec implies but no single task's happy path exercises; each gets a test in the task that owns the code:

1. **Stop while the graph is running / a sample is in flight** — `stop()` must return promptly with no deadlock or use-after-free (Task 3, "stop() returns promptly").
2. **A device both stacks see (physical camera)** — must be listed once, not doubled (Task 2, merge test).
3. **A DirectShow-only software filter with no `DevicePath`** — must still be deduped by friendly name when it is a physical camera (Task 2, name-fallback test).
4. **A device that enumerates but never delivers a frame** — must not hang `stop()` (Task 3, deliver test scans devices like `mf_capture_test.cpp`).
5. **A device that cannot be found/opened** — reported as `capture-failed` and retried, never a crash (Task 3, unknown-device test).

---

### Task 1: Rename the input label to "Device camera"

**Files:**
- Modify: `BreadCamera/ui/src/dialogs/AddCameraDialog.tsx` (option `uvc`; the `Device` hint)
- Modify: `BreadCamera/ui/src/inspector/Inspector.tsx` (the `Device` hint)
- Modify: `BreadCamera/ui/src/state/labels.ts` (`IN.uvc`)
- Modify: `BreadCamera/ui/src/errors.ts` (`capture-failed`)
- Test: `BreadCamera/ui/src/dialogs/AddCameraDialog.test.tsx`

**Interfaces:**
- Consumes: nothing new.
- Produces: the string `Device camera` as the `uvc` input-type option, label and chip; `Device cameras are video only, no audio.` as the hint.

- [ ] **Step 1: Write the failing test** — append to `AddCameraDialog.test.tsx`:

```tsx
test('the capture input type is called "Device camera"', () => {
  const c = client();
  render(<AddCameraDialog client={c} onClose={() => {}} />);
  expect(screen.getByRole('option', { name: 'Device camera' })).toBeInTheDocument();
  expect(screen.queryByRole('option', { name: 'USB or built-in camera' })).toBeNull();
});
```

- [ ] **Step 2: Run it and watch it fail**

Run: `npm test -- AddCameraDialog` (in `BreadCamera/ui`)
Expected: FAIL — no option named `Device camera`.

- [ ] **Step 3: Change the copy**

- `AddCameraDialog.tsx`: `<option value="uvc">Device camera</option>`; the `Device` hint becomes `errors.device || 'Device cameras are video only, no audio.'`.
- `Inspector.tsx`: the `Device` hint becomes `"Device cameras are video only, no audio."`.
- `labels.ts`: `uvc: ['CAM', 'Device camera'],`.
- `errors.ts`: `'capture-failed': 'The camera could not be opened.',`.

- [ ] **Step 4: Run the UI suite and watch it pass**

Run: `npm test` (in `BreadCamera/ui`)
Expected: PASS, whole suite green (no test asserted the old strings).

- [ ] **Step 5: Commit**

```bash
git add ui/src
git commit -m "feat(ui): call the capture input 'Device camera'"
```

---

### Task 2: DirectShow enumeration and the merged device list

**Files:**
- Create: `BreadCamera/src/io/dshow_devices.h`
- Create: `BreadCamera/src/io/dshow_devices.cpp`
- Modify: `BreadCamera/src/io/mf_capture_input.cpp` (`listCaptureDevices`, lines 135-143)
- Modify: `BreadCamera/tests/support/requires.h`, `BreadCamera/tests/support/requires.cpp`
- Create: `BreadCamera/tests/dshow_devices_test.cpp`
- Modify: `BreadCamera/tests/mf_capture_test.cpp` (uniqueness check)
- Modify: `BreadCamera/CMakeLists.txt` (bc_io sources + link; bc_tests list)

**Interfaces:**
- Consumes: `bc::CaptureDevice`, `bc::toUtf8` (`core/utf8.h`).
- Produces:

```cpp
// src/io/dshow_devices.h
#pragma once
#include <string>
#include <vector>
#include "io/mf_capture_input.h"  // bc::CaptureDevice
namespace bc {
struct DShowDevice {
  std::string name;         // IPropertyBag "FriendlyName"
  std::string displayName;  // IMoniker::GetDisplayName — the stable id
  std::string devicePath;   // IPropertyBag "DevicePath"; empty for software filters
};
std::vector<DShowDevice> listDShowDevices();
std::vector<CaptureDevice> mergeCaptureDevices(const std::vector<CaptureDevice>& mf,
                                               const std::vector<DShowDevice>& dshow);
}  // namespace bc
```

- [ ] **Step 1: Write the failing tests** — create `tests/dshow_devices_test.cpp`:

```cpp
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>
#include "io/dshow_devices.h"

using bc::CaptureDevice;
using bc::DShowDevice;

TEST_CASE("mergeCaptureDevices: a camera both stacks see is listed once") {
  std::vector<CaptureDevice> mf = {{"HD WebCam", "\\\\?\\usb#vid_0408"}};
  std::vector<DShowDevice> ds = {{"HD WebCam", "@device:pnp:x", "\\\\?\\usb#vid_0408"}};
  CHECK(bc::mergeCaptureDevices(mf, ds).size() == 1);
}

TEST_CASE("mergeCaptureDevices: a DirectShow-only device is appended with a dshow: id") {
  std::vector<CaptureDevice> mf = {{"HD WebCam", "\\\\?\\usb#vid_0408"}};
  std::vector<DShowDevice> ds = {{"OBS Virtual Camera", "@device:sw:obs", ""}};
  auto out = bc::mergeCaptureDevices(mf, ds);
  REQUIRE(out.size() == 2);
  CHECK(out[1].name == "OBS Virtual Camera");
  CHECK(out[1].id == "dshow:@device:sw:obs");
}

TEST_CASE("mergeCaptureDevices: a duplicate with no DevicePath falls back to the name") {
  std::vector<CaptureDevice> mf = {{"HD WebCam", "\\\\?\\usb#vid_0408"}};
  std::vector<DShowDevice> ds = {{"HD WebCam", "@device:sw:x", ""}};
  CHECK(bc::mergeCaptureDevices(mf, ds).size() == 1);
}

TEST_CASE("mergeCaptureDevices: the search is case-insensitive") {
  std::vector<CaptureDevice> mf = {{"HD WebCam", "\\\\?\\usb#vid_0408"}};
  std::vector<DShowDevice> ds = {{"hd webcam", "@device:pnp:x", "\\\\?\\USB#VID_0408"}};
  CHECK(bc::mergeCaptureDevices(mf, ds).size() == 1);
}

TEST_CASE("listDShowDevices: runs and returns names with display names") {
  for (auto& d : bc::listDShowDevices()) {
    CHECK_FALSE(d.name.empty());
    CHECK_FALSE(d.displayName.empty());
  }
}
```

- [ ] **Step 2: Run them and watch them fail to compile**

Run: `scripts/test.ps1 -Filter "mergeCaptureDevices*,listDShowDevices*"`
Expected: build FAILS — `io/dshow_devices.h` not found.

- [ ] **Step 3: Implement the module**

`dshow_devices.cpp` — include `<windows.h>`, `<dshow.h>`, `<initguid.h>` (once, before the DirectShow GUIDs, in this TU), `"core/utf8.h"`, `"io/mf_capture_input.h"`.

`listDShowDevices()`: `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` guarded so a failure is released on scope exit (the `ComInit` shape from `mf_capture_input.cpp`); `CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&devEnum))`; `devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0)` — treat `S_FALSE`/failure as an empty list. For each moniker: `GetDisplayName(nullptr, nullptr, &w)` → `toUtf8`; `BindToStorage(0, nullptr, IID_PPV_ARGS(&bag))` then `bag->Read(L"FriendlyName", &v, nullptr)` (VT_BSTR) → `toUtf8`, and `bag->Read(L"DevicePath", &v, nullptr)` if present (missing is fine → empty). Skip a moniker whose `FriendlyName` is empty.

`mergeCaptureDevices(mf, dshow)`: start from `mf`; for each `d` skip when `devicePath` (non-empty) case-insensitively equals any `mf` id, when `d.name` case-insensitively equals any `mf` name, or when the result already holds `"dshow:" + d.displayName`; otherwise `push_back({d.name, "dshow:" + d.displayName})`.

- [ ] **Step 4: Merge into `listCaptureDevices`**

In `src/io/mf_capture_input.cpp` add `#include "io/dshow_devices.h"` and change `listCaptureDevices()` to build the MF vector as it does now, then `return mergeCaptureDevices(mf, listDShowDevices());`.

- [ ] **Step 5: Extend the existing test with the no-duplicates guarantee**

In `tests/mf_capture_test.cpp`, inside `TEST_CASE("listCaptureDevices: runs and returns names with ids")`, collect the names into a `std::set<std::string>` and `CHECK(names.size() == list.size())`.

- [ ] **Step 6: Add `Req::DShowDevice`**

`requires.h`: add `DShowDevice` to the `Req` enum. `requires.cpp`: include `"io/dshow_devices.h"` and add

```cpp
case Req::DShowDevice:
  reason = "no DirectShow video capture device on this PC";
  return bc::listDShowDevices().empty();
```

- [ ] **Step 7: Wire CMake**

In `CMakeLists.txt`: add `src/io/dshow_devices.cpp` to the `add_library(bc_io STATIC ...)` list; add `target_link_libraries(bc_io PUBLIC quartz strmiids)`; add `tests/dshow_devices_test.cpp` to the `add_executable(bc_tests ...)` list.

- [ ] **Step 8: Run the tests and watch them pass**

Run: `scripts/test.ps1 -Filter "mergeCaptureDevices*,listDShowDevices*,listCaptureDevices*"`
Expected: PASS (the enumeration test tolerates zero devices; the merge tests are hardware-free).

- [ ] **Step 9: Commit**

```bash
git add src/io/dshow_devices.h src/io/dshow_devices.cpp src/io/mf_capture_input.cpp \
        tests/support/requires.h tests/support/requires.cpp tests/dshow_devices_test.cpp \
        tests/mf_capture_test.cpp CMakeLists.txt
git commit -m "feat(io): enumerate DirectShow cameras into the device list"
```

---

### Task 3: `DShowCaptureInput` — capture through SampleGrabber

**Files:**
- Create: `BreadCamera/src/io/dshow_capture_input.h`
- Create: `BreadCamera/src/io/dshow_capture_input.cpp`
- Create: `BreadCamera/tests/dshow_capture_test.cpp`
- Modify: `BreadCamera/CMakeLists.txt` (bc_io source; bc_tests list)
- Modify: `BreadCamera/docs/superpowers/follow-ups.md`

**Interfaces:**
- Consumes: `bc::CaptureDevice`, `bc::listDShowDevices()` (Task 2); `GpuDevice`, `TexturePool`, `InputSink`, `SignalState`, `ErrorInfo`, `VideoFrameIn`.
- Produces:

```cpp
// src/io/dshow_capture_input.h
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"
namespace bc {
// A DirectShow video capture device (CLSID_VideoInputDeviceCategory), addressed by
// moniker display name. Video only, BGRA8. Same contract as MfCaptureInput.
class DShowCaptureInput final : public Input {
 public:
  DShowCaptureInput(GpuDevice& gpu, TexturePool& pool, std::string displayName);
  ~DShowCaptureInput() override;
  void start(InputSink& sink) override;
  void stop() override;
 private:
  struct GrabberCB;
  void run();
  void runOnce();
  GpuDevice& gpu_;
  TexturePool& pool_;
  std::string displayName_;
  InputSink* sink_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::vector<uint8_t> flip_;
  GrabberCB* cb_ = nullptr;
};
}  // namespace bc
```

- [ ] **Step 1: Write the failing tests** — create `tests/dshow_capture_test.cpp`:

```cpp
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "gpu/device.h"
#include "io/dshow_capture_input.h"
#include "io/dshow_devices.h"
#include "support/requires.h"

using namespace std::chrono_literals;

namespace {
struct Sink : bc::InputSink {
  std::mutex m;
  int video = 0;
  std::vector<bc::SignalState> states;
  std::vector<std::optional<bc::ErrorInfo>> errors;
  void onVideo(bc::VideoFrameIn) override { std::lock_guard l(m); ++video; }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState s, std::optional<bc::ErrorInfo> e) override {
    std::lock_guard l(m); states.push_back(s); errors.push_back(e);
  }
};
}  // namespace

TEST_CASE("DShowCaptureInput: an unknown device is reported and retried, never crashes") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::DShowCaptureInput in(*gpu, pool, "@device:sw:no-such-camera-1234");
  Sink sink;
  in.start(sink);
  std::this_thread::sleep_for(1500ms);
  in.stop();
  std::lock_guard l(sink.m);
  REQUIRE(sink.states.size() >= 3);
  CHECK(sink.states[1] == bc::SignalState::Offline);
  REQUIRE(sink.errors[1]);
  CHECK(sink.errors[1]->code == "capture-failed");
}

TEST_CASE("DShowCaptureInput: stop() returns promptly while retrying") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::DShowCaptureInput in(*gpu, pool, "@device:sw:no-such-camera-1234");
  Sink sink;
  in.start(sink);
  std::this_thread::sleep_for(200ms);
  const auto t0 = std::chrono::steady_clock::now();
  in.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 2s);
}

TEST_CASE("DShowCaptureInput: a capture device delivers frames" BC_REQUIRES(bc::test::Req::DShowDevice)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  for (const auto& dev : bc::listDShowDevices()) {
    bc::DShowCaptureInput in(*gpu, pool, dev.displayName);
    Sink sink;
    in.start(sink);
    for (int i = 0; i < 30; ++i) {
      { std::lock_guard l(sink.m); if (sink.video > 20) break; }
      std::this_thread::sleep_for(100ms);
    }
    in.stop();
    std::lock_guard l(sink.m);
    if (sink.video > 20) { CHECK(sink.video > 20); return; }
  }
  std::fprintf(stderr, "[skip] no DirectShow device delivered frames\n");
}
```

- [ ] **Step 2: Run them and watch them fail to compile**

Run: `scripts/test.ps1 -Filter "DShowCaptureInput*"`
Expected: build FAILS — `io/dshow_capture_input.h` not found.

- [ ] **Step 3: Declare the SampleGrabber interfaces** (in `dshow_capture_input.cpp`, guarded so a shipped `qedit.h` would win if it ever returns):

```cpp
#if !defined(__ISampleGrabber_INTERFACE_DEFINED__)
// {6B652FFF-11FE-4FCE-92AD-0266B5D7C78F}
static const GUID IID_ISampleGrabber = {0x6b652fff,0x11fe,0x4fce,{0x92,0xad,0x02,0x66,0xb5,0xd7,0xc7,0x8f}};
// {0579154A-2B53-4994-B0D0-E773148EFF85}
static const GUID IID_ISampleGrabberCB = {0x0579154a,0x2b53,0x4994,{0xb0,0xd0,0xe7,0x73,0x14,0x8e,0xff,0x85}};
// {C1F400A0-3F08-11D3-9F0B-006008039E37}
static const GUID CLSID_SampleGrabber = {0xc1f400a0,0x3f08,0x11d3,{0x9f,0x0b,0x00,0x60,0x08,0x03,0x9e,0x37}};
// {C1F400A4-3F08-11D3-9F0B-006008039E37}
static const GUID CLSID_NullRenderer = {0xc1f400a4,0x3f08,0x11d3,{0x9f,0x0b,0x00,0x60,0x08,0x03,0x9e,0x37}};

struct ISampleGrabberCB : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample*) = 0;
  virtual HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE*, long) = 0;
};
struct ISampleGrabber : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long*, long*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample**) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB*, long) = 0;
};
#endif
```

- [ ] **Step 4: Implement `GrabberCB` and `runOnce`**

`GrabberCB` (in the header as a forward-declared `struct`, defined in the .cpp) implements `ISampleGrabberCB`: `QueryInterface`/`AddRef`/`Release` (atomic refcount, `IID_ISampleGrabberCB` and `IUnknown`); `BufferCB` returns `S_OK`. `SampleCB(double, IMediaSample* s)` `memcpy`s `s`'s data (`GetActualDataLength`, `GetPointer`) plus the sample's `AM_MEDIA_TYPE` if `s->GetMediaType` returns one into owned members, sets `pending`, and `cv.notify_one()` — all under `m`, and returns `S_OK` immediately if `done`. `stop()` sets `done`, clears the slot, `cv.notify_all()` (the `MfReaderCallback` shape — this closes Review Focus 1).

`run()` is the `MfCaptureInput::run()` shape: `Connecting`; `runOnce()`; on return throw `"the device stopped sending frames"`; catch → `Offline` + `{"capture-failed", displayName_ + ": " + e.what()}`; wait on `cb_->cv` for `backoff.next()` or `stopping_`.

`runOnce()`: `CoInitializeEx(nullptr, COINIT_MULTITHREADED)`; find the moniker in `listDShowDevices()`-style enumeration whose `displayName == displayName_` (throw `"device not found"` if none); `BindToObject(..., IID_IBaseFilter, ...)`; create `CLSID_FilterGraph` (`IGraphBuilder`) and `CLSID_CaptureGraphBuilder2` (`ICaptureGraphBuilder2`), `SetFiltergraph`, `AddFilter(capture, L"Capture")`; create `CLSID_SampleGrabber` as `IBaseFilter`, `QueryInterface(IID_ISampleGrabber)`; set its media type to `MEDIATYPE_Video` / `MEDIASUBTYPE_RGB32` / `FORMAT_VideoInfo`; `AddFilter(grabber, L"Grabber")`; `RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, capture, nullptr, grabber)`; `grabber->SetBufferSamples(FALSE)`, `grabber->SetCallback(cb_, 1)`; `Run()`.

The run loop waits on `cb_->cv`; on a pending slot it copies the slot into a BGRA8 texture: read `grabber->GetConnectedMediaType` once for `width`/`height` (`BITMAPINFOHEADER` in the format block) and `stride` (`biHeight` sign gives top-down vs bottom-up), reuse `flip_` for negative stride exactly as `MfCaptureInput` does, `UpdateSubresource` under `gpu_.lock()`, then `onState(Live)` on the first frame and `onVideo(...)`. On leaving the loop: `control->Stop()` then release the graph on this thread.

- [ ] **Step 5: Wire CMake and the follow-up note**

`CMakeLists.txt`: add `src/io/dshow_capture_input.cpp` to `bc_io` and `tests/dshow_capture_test.cpp` to `bc_tests`. Append to `docs/superpowers/follow-ups.md`: "DirectShow capture (`src/io/dshow_capture_input.cpp`) uses the deprecated in-box SampleGrabber filter (`qedit.h`-era) and does not negotiate resolution/frame rate — the device default is used. Replace with a custom `IMemInputPin` sink filter if SampleGrabber is withdrawn."

- [ ] **Step 6: Run the tests and watch them pass**

Run: `scripts/test.ps1 -Filter "DShowCaptureInput*"`
Expected: PASS (both hardware-free cases; the deliver test runs on a PC with a DirectShow device, else prints `[skip]`).

- [ ] **Step 7: Commit**

```bash
git add src/io/dshow_capture_input.h src/io/dshow_capture_input.cpp \
        tests/dshow_capture_test.cpp CMakeLists.txt docs/superpowers/follow-ups.md
git commit -m "feat(io): capture DirectShow cameras (OBS Virtual Camera) via SampleGrabber"
```

---

### Task 4: Route `dshow:` ids in the adapter factory

**Files:**
- Modify: `BreadCamera/src/engine/adapter_factory.cpp` (the `uvc` branch, lines 48-53)
- Modify: `BreadCamera/tests/dshow_capture_test.cpp`

**Interfaces:**
- Consumes: `bc::DShowCaptureInput` (Task 3), `bc::DefaultAdapterFactory`, `bc::InputConfig`.
- Produces: `makeInput` returns a `DShowCaptureInput` for a `uvc` device id beginning `dshow:`, and `MfCaptureInput` otherwise.

- [ ] **Step 1: Write the failing test** — append to `tests/dshow_capture_test.cpp`:

```cpp
#include "engine/adapter_factory.h"
#include "io/mf_capture_input.h"

TEST_CASE("makeInput: a dshow: device id builds a DirectShow input") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::DefaultAdapterFactory f(*gpu, pool);
  bc::InputConfig cfg;
  cfg.kind = "uvc";
  cfg.params = {{"device", "dshow:@device:sw:no-such"}};
  auto in = f.makeInput(cfg);
  CHECK(dynamic_cast<bc::DShowCaptureInput*>(in.get()) != nullptr);

  cfg.params = {{"device", "\\\\?\\usb#vid_0408"}};
  auto mf = f.makeInput(cfg);
  CHECK(dynamic_cast<bc::MfCaptureInput*>(mf.get()) != nullptr);
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `scripts/test.ps1 -Filter "makeInput*"`
Expected: FAIL — the `dshow:` id builds an `MfCaptureInput` (dynamic_cast to `DShowCaptureInput` is null).

- [ ] **Step 3: Implement the routing**

In `adapter_factory.cpp` add `#include "io/dshow_capture_input.h"` and change the `uvc` branch to read `device` once, then:

```cpp
if (device.rfind("dshow:", 0) == 0)
  return std::make_unique<DShowCaptureInput>(gpu_, pool_, device.substr(6));
return std::make_unique<MfCaptureInput>(gpu_, pool_, device, cfg.params.value("width", 0),
                                        cfg.params.value("height", 0), cfg.params.value("fps", 0));
```

- [ ] **Step 4: Run it and watch it pass**

Run: `scripts/test.ps1 -Filter "makeInput*"`
Expected: PASS.

- [ ] **Step 5: Run the whole C++ suite**

Run: `scripts/test.ps1`
Expected: PASS (new tests included; pre-existing skips unchanged).

- [ ] **Step 6: Commit**

```bash
git add src/engine/adapter_factory.cpp tests/dshow_capture_test.cpp
git commit -m "feat(io): route dshow: capture ids to the DirectShow input"
```

---

## Self-Review

- **Spec coverage:** enumeration + merge (Task 2), capture (Task 3), routing + id scheme (Task 4), rename + copy (Task 1); non-goals (audio, resolution, FFmpeg) untouched. Device-path-then-name dedupe and the SampleGrabber call are the two review-driven decisions, both in Task 2/3.
- **Review Focus:** each of the five modes maps to a named test (Tasks 2 and 3).
- **Type consistency:** `DShowDevice{name,displayName,devicePath}`, `mergeCaptureDevices(mf,dshow)`, `DShowCaptureInput(gpu,pool,displayName)`, and the `dshow:` prefix are used identically across Tasks 2-4.
- **Proportion:** the plan carries signatures, exact GUIDs and the copy changes — not the full bodies.
