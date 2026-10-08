# BreadCamera M4b — DeckLink, AJA (experimental) and the Windows virtual camera Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The hardware-bound adapters: Blackmagic DeckLink SDI/HDMI input and output (with external key+fill), AJA NTV2 input and output (experimental, fill only), and a Windows 11 virtual camera per camera (Media Foundation `MFCreateVirtualCamera` + a custom media source DLL fed through shared memory).

**Amended 2026-10-08 after the UI mockup review (owner):** English only; Task 4's UI part is ported to M1's native dialogs; SDI (DeckLink, AJA) and the virtual camera keep their fixed formats, so the Add output dialog hides Resolution / Frame rate for them and the router refuses a format override (spec §4.3).

**Architecture:** DeckLink and AJA follow the M1 adapter model (`Input` / `QueuedOutput`), converting UYVY ⇄ BGRA on the CPU with swscale. The virtual camera has two halves: `breadcamera-vcam.dll` (a media source vendored from `smourier/VCamSample`, MIT, whose frame generator now copies the latest frame from a `Global\` shared-memory section it creates) and `VirtualCameraOutput` in the engine, which registers the camera with `MFCreateVirtualCamera` and writes frames into that section with a sequence lock. All three are compile-gated and **untested on the build PC** unless the hardware/registration exists.

**Tech Stack:** Blackmagic Desktop Video SDK 14.x (owner installs; IDL compiled with MIDL), AJA libajantv2 `ntv2_18_0_0` (MIT, CMake FetchContent), Media Foundation virtual camera API (Windows 11 22000+), C++/WinRT headers from the Windows SDK, WIL.

**Spec:** `docs/superpowers/specs/2026-10-08-breadcamera-design.md` (§4.2, §4.3, §7.1, §7.2).

**Branch:** `bcam-m4b`, cut from `bcam-m4a`.

## Global Constraints

- All earlier Global Constraints apply.
- `BC_WITH_DECKLINK` (SDK found via `DECKLINK_SDK_DIR`), `BC_WITH_AJA` (FetchContent succeeds), `BC_WITH_VCAM` (Windows SDK has `mfvirtualcamera.h`) — each ON when available, OFF with a message otherwise.
- AJA is **experimental**: the UI labels every AJA item "Experimental — not verified on hardware". AJA output is **fill only** in this milestone (key output is a follow-up) — a deviation from spec §4.3's "key+fill" table row, recorded in the follow-ups.
- DeckLink output: `keyed` outputs use the card's **external keyer** (fill on one connector, key on its pair) when the card reports `BMDDeckLinkSupportsExternalKeying`; otherwise the output refuses with a clear error.
- SDI/HDMI outputs run at 1920×1080 at the show rate (`50/1`, `60000/1001`, `60/1` → `bmdModeHD1080p50`, `bmdModeHD1080p5994`, `bmdModeHD1080p6000`); frames of other sizes are scaled.
- Virtual camera: 1920×1080 at 30 fps, at most **4** (one per camera that uses it), indexes 1–4 unique within a show. The DLL needs registration in HKLM (admin) — done by the installer (M5) or `regsvr32` for development; without it the output shows "the BreadCamera virtual camera is not installed".
- The vendored VCamSample code keeps its MIT licence notice (`vcam/LICENSE`).

## Review Focus

1. **No card / card removed / driver missing** — inputs and outputs report a readable error and retry; the engine and other outputs keep running — owned by Tasks 1, 2 (automated: device index that does not exist).
2. **Input format change on SDI** (1080i50 → 1080p60) — DeckLink format detection re-enables the input without a restart; AJA re-reads the format on loss — owned by Task 1 (code path) and Task 2.
3. **Virtual camera opened by two apps at once, or by none** — the engine writes only when the shared section exists; no crash, no blocking — owned by Task 3 (shared-memory unit tests).
4. **A torn frame in shared memory** (writer mid-copy) — the reader detects the odd/changed sequence and keeps the previous frame — owned by Task 3.
5. **Duplicate virtual camera index in a show** — refused by the router — owned by Task 4.

## Prerequisites the owner installs (not the night run)

- Blackmagic Desktop Video (driver) + Desktop Video SDK 14.x; set `DECKLINK_SDK_DIR` to the SDK folder (it contains `Win\include\DeckLinkAPI.idl`).
- For the virtual camera loopback test: run once as administrator `regsvr32 <build>\breadcamera-vcam.dll` (or install with the M5 installer).

## File Structure

```
src/io/uyvy.h/.cpp                 UYVY ⇄ BGRA (swscale) helpers
src/io/decklink.h/.cpp             device listing, mode mapping (BC_WITH_DECKLINK)
src/io/decklink_input.h/.cpp, decklink_output.h/.cpp
src/io/aja.h/.cpp, aja_input.h/.cpp, aja_output.h/.cpp   (BC_WITH_AJA)
src/core/vcam_shared.h/.cpp         shared-memory frame protocol (both sides)
src/io/vcam_output.h/.cpp           MFCreateVirtualCamera + writer (BC_WITH_VCAM)
vcam/                               vendored VCamSample media source → breadcamera-vcam.dll
cmake/FindDeckLink.cmake
```

## Execution notes for the controller

- 5 tasks → batched reviews (1–3, 4–5).
- Most hardware tests will be **skipped** on the build PC. That is expected; a skip with its reason is the correct result. A **build** failure is not acceptable: every task must build both with its SDK absent (option OFF) and, where the SDK is present, ON.

---

### Task 1: DeckLink input and output

**Files:**
- Create: `cmake/FindDeckLink.cmake`, `src/io/uyvy.h`, `src/io/uyvy.cpp`, `src/io/decklink.h`, `src/io/decklink.cpp`, `src/io/decklink_input.h`, `src/io/decklink_input.cpp`, `src/io/decklink_output.h`, `src/io/decklink_output.cpp`
- Modify: `CMakeLists.txt` (`project(BreadCamera LANGUAGES C CXX)`, `BC_WITH_DECKLINK`, MIDL step), `tests/support/requires.h/.cpp` (`Req::DeckLink`)
- Test: `tests/uyvy_test.cpp`, `tests/decklink_test.cpp`

**Interfaces:**
- Consumes: `Input`, `InputSink`, `VideoFrameIn`, `AudioBlock`, `QueuedOutput`, `OutFrame`, `TexturePool`, `GpuDevice`, `Rate` (M1).
- Produces:
  - `src/io/uyvy.h` (always built): `std::vector<uint8_t> bc::uyvyToBgra(const uint8_t* uyvy, int width, int height, int rowBytes, bool bt709)`, `void bc::bgraToUyvy(const uint8_t* bgra, int width, int height, uint8_t* out, int outRowBytes, bool bt709)` (both via swscale, limited range).
  - `src/io/decklink.h` (BC_WITH_DECKLINK): `bc::DeckLinkInfo { int index; std::string name; bool hasInput, hasOutput, externalKeying; }`, `std::vector<DeckLinkInfo> bc::listDeckLinkDevices()`, `uint32_t bc::deckLinkModeFor(Rate)` (BMDDisplayMode as `uint32_t`; throws for other rates).
  - `bc::DeckLinkInput(GpuDevice&, TexturePool&, int deviceIndex)`, `bc::DeckLinkOutput(int deviceIndex, Rate showRate, bool keyed)` (error code `"decklink-failed"`).
  - `Req::DeckLink` (missing unless `BC_WITH_DECKLINK` and at least one device is listed).

SDK facts used (Desktop Video SDK 14.x, `DeckLinkAPI.idl` compiled with MIDL into `DeckLinkAPI_h.h` + `DeckLinkAPI_i.c`): `CoCreateInstance(CLSID_CDeckLinkIterator, …, IID_IDeckLinkIterator)`, `IDeckLinkIterator::Next`, `IDeckLink::GetDisplayName(BSTR*)`, `IDeckLinkProfileAttributes::GetFlag(BMDDeckLinkSupportsExternalKeying, BOOL*)`; input: `IDeckLinkInput::SetCallback`, `EnableVideoInput(mode, bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection)`, `EnableAudioInput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger, 2)`, `StartStreams/StopStreams/PauseStreams/FlushStreams`, callback `VideoInputFormatChanged` / `VideoInputFrameArrived(IDeckLinkVideoInputFrame*, IDeckLinkAudioInputPacket*)`; SDK 14 frame bytes through `IDeckLinkVideoBuffer` (`StartAccess(bmdBufferAccessRead)`, `GetBytes`, `EndAccess`); output: `EnableVideoOutput(mode, bmdVideoOutputFlagDefault)`, `EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 2, bmdAudioOutputStreamContinuous)`, `CreateVideoFrame(w, h, rowBytes, bmdFormat8BitBGRA, bmdFrameFlagDefault, &frame)`, `DisplayVideoFrameSync`, `WriteAudioSamplesSync`, `IDeckLinkKeyer::Enable(TRUE /*external*/)`, `SetLevel(255)`, `Disable()`. If the installed SDK is 12.x (no `IDeckLinkVideoBuffer`), use `IDeckLinkVideoFrame::GetBytes` instead and ledger it.

- [ ] **Step 1: UYVY helpers with a test (always built)**

`tests/uyvy_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "io/uyvy.h"

TEST_CASE("UYVY <-> BGRA round trip keeps colours (BT.709 limited)") {
  const int w = 16, h = 2;
  std::vector<uint8_t> bgra(w * h * 4);
  for (int i = 0; i < w * h; ++i) {
    bgra[i * 4] = 40;
    bgra[i * 4 + 1] = 120;
    bgra[i * 4 + 2] = 200;
    bgra[i * 4 + 3] = 255;
  }
  std::vector<uint8_t> uyvy(w * 2 * h);
  bc::bgraToUyvy(bgra.data(), w, h, uyvy.data(), w * 2, true);
  auto back = bc::uyvyToBgra(uyvy.data(), w, h, w * 2, true);
  for (int i = 0; i < w * h; ++i) {
    CHECK(std::abs(back[i * 4] - 40) <= 3);
    CHECK(std::abs(back[i * 4 + 1] - 120) <= 3);
    CHECK(std::abs(back[i * 4 + 2] - 200) <= 3);
  }
}
```

`src/io/uyvy.h`:
```cpp
#pragma once
#include <cstdint>
#include <vector>

namespace bc {
// SDI cards speak 8-bit 4:2:2 UYVY; the pipeline uploads BGRA (spec §3.2). Limited range.
std::vector<uint8_t> uyvyToBgra(const uint8_t* uyvy, int width, int height, int rowBytes, bool bt709);
void bgraToUyvy(const uint8_t* bgra, int width, int height, uint8_t* out, int outRowBytes, bool bt709);
}  // namespace bc
```

`src/io/uyvy.cpp`:
```cpp
#include "io/uyvy.h"

extern "C" {
#include <libswscale/swscale.h>
}

#include <stdexcept>

namespace bc {
namespace {
SwsContext* make(int w, int h, AVPixelFormat from, AVPixelFormat to, bool bt709, bool toYuv) {
  SwsContext* s = sws_getContext(w, h, from, w, h, to, SWS_BILINEAR, nullptr, nullptr, nullptr);
  if (!s) throw std::runtime_error("cannot create a pixel converter");
  const int* cs = sws_getCoefficients(bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
  // srcRange/dstRange: 0 = limited (MPEG), 1 = full (JPEG). RGB is full, SDI YUV is limited.
  sws_setColorspaceDetails(s, cs, toYuv ? 1 : 0, cs, toYuv ? 0 : 1, 0, 1 << 16, 1 << 16);
  return s;
}
}  // namespace

std::vector<uint8_t> uyvyToBgra(const uint8_t* uyvy, int w, int h, int rowBytes, bool bt709) {
  std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
  SwsContext* s = make(w, h, AV_PIX_FMT_UYVY422, AV_PIX_FMT_BGRA, bt709, false);
  const uint8_t* src[1] = {uyvy};
  const int srcStride[1] = {rowBytes};
  uint8_t* dst[1] = {out.data()};
  const int dstStride[1] = {w * 4};
  sws_scale(s, src, srcStride, 0, h, dst, dstStride);
  sws_freeContext(s);
  return out;
}

void bgraToUyvy(const uint8_t* bgra, int w, int h, uint8_t* out, int outRowBytes, bool bt709) {
  SwsContext* s = make(w, h, AV_PIX_FMT_BGRA, AV_PIX_FMT_UYVY422, bt709, true);
  const uint8_t* src[1] = {bgra};
  const int srcStride[1] = {w * 4};
  uint8_t* dst[1] = {out};
  const int dstStride[1] = {outRowBytes};
  sws_scale(s, src, srcStride, 0, h, dst, dstStride);
  sws_freeContext(s);
}
}  // namespace bc
```
ponytail: a converter is created per frame (~0.1 ms); cache it per size if profiling shows it.

Add `src/io/uyvy.cpp` to `bc_io` and the test to `bc_tests`. Run `-Filter "UYVY*"` → PASS. Commit this step on its own is not required; continue.

- [ ] **Step 2: Find the SDK and compile the IDL**

`cmake/FindDeckLink.cmake`:
```cmake
# Blackmagic Desktop Video SDK (spec §7.2: optional). DECKLINK_SDK_DIR = the SDK root.
set(_dl "$ENV{DECKLINK_SDK_DIR}")
find_file(DECKLINK_IDL DeckLinkAPI.idl PATHS "${_dl}/Win/include" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(DeckLink DEFAULT_MSG DECKLINK_IDL)
```
In `CMakeLists.txt`: change `project(BreadCamera LANGUAGES CXX)` to `project(BreadCamera LANGUAGES C CXX)`; add the option (same pattern as NDI) and, after `bc_io`:
```cmake
if(BC_WITH_DECKLINK)
  set(_dlgen ${CMAKE_BINARY_DIR}/decklink)
  file(MAKE_DIRECTORY ${_dlgen})
  get_filename_component(_dlinc ${DECKLINK_IDL} DIRECTORY)
  add_custom_command(
    OUTPUT ${_dlgen}/DeckLinkAPI_h.h ${_dlgen}/DeckLinkAPI_i.c
    COMMAND midl /nologo /env x64 /I ${_dlinc} /h DeckLinkAPI_h.h /iid DeckLinkAPI_i.c /out ${_dlgen} ${DECKLINK_IDL}
    DEPENDS ${DECKLINK_IDL} VERBATIM)
  target_sources(bc_io PRIVATE ${_dlgen}/DeckLinkAPI_i.c src/io/decklink.cpp src/io/decklink_input.cpp src/io/decklink_output.cpp)
  target_include_directories(bc_io PUBLIC ${_dlgen})
  target_compile_definitions(bc_io PUBLIC BC_WITH_DECKLINK=1)
else()
  target_compile_definitions(bc_io PUBLIC BC_WITH_DECKLINK=0)
endif()
```
`requires`: `DeckLink` → missing if `BC_WITH_DECKLINK == 0` or `bc::listDeckLinkDevices().empty()` (guard the call with `#if BC_WITH_DECKLINK`); reason "no DeckLink SDK or card".

- [ ] **Step 3: Device listing and modes**

`src/io/decklink.h`:
```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/rate.h"

struct IDeckLink;

namespace bc {
struct DeckLinkInfo {
  int index = 0;
  std::string name;
  bool hasInput = false, hasOutput = false, externalKeying = false;
};
std::vector<DeckLinkInfo> listDeckLinkDevices();
uint32_t deckLinkModeFor(Rate r);  // 1080p50 / 1080p59.94 / 1080p60
IDeckLink* openDeckLink(int index);  // AddRef'd; throws std::runtime_error when absent
}  // namespace bc
```

`src/io/decklink.cpp`:
```cpp
#include "io/decklink.h"

#include <windows.h>

#include <stdexcept>

#include "DeckLinkAPI_h.h"
#include "core/utf8.h"

namespace bc {
namespace {
struct Com {
  Com() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
  ~Com() { CoUninitialize(); }
};
}  // namespace

IDeckLink* openDeckLink(int index) {
  IDeckLinkIterator* it = nullptr;
  if (FAILED(CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, reinterpret_cast<void**>(&it)))) {
    throw std::runtime_error("the Blackmagic Desktop Video driver is not installed");
  }
  IDeckLink* dl = nullptr;
  for (int i = 0; it->Next(&dl) == S_OK; ++i) {
    if (i == index) break;
    dl->Release();
    dl = nullptr;
  }
  it->Release();
  if (!dl) throw std::runtime_error("DeckLink device " + std::to_string(index) + " not found");
  return dl;
}

std::vector<DeckLinkInfo> listDeckLinkDevices() {
  Com com;
  std::vector<DeckLinkInfo> out;
  IDeckLinkIterator* it = nullptr;
  if (FAILED(CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, reinterpret_cast<void**>(&it)))) return out;
  IDeckLink* dl = nullptr;
  for (int i = 0; it->Next(&dl) == S_OK; ++i) {
    DeckLinkInfo info;
    info.index = i;
    BSTR name = nullptr;
    if (dl->GetDisplayName(&name) == S_OK) {
      info.name = toUtf8(name);
      SysFreeString(name);
    }
    IDeckLinkInput* in = nullptr;
    IDeckLinkOutput* o = nullptr;
    info.hasInput = dl->QueryInterface(IID_IDeckLinkInput, reinterpret_cast<void**>(&in)) == S_OK;
    info.hasOutput = dl->QueryInterface(IID_IDeckLinkOutput, reinterpret_cast<void**>(&o)) == S_OK;
    if (in) in->Release();
    if (o) o->Release();
    IDeckLinkProfileAttributes* attrs = nullptr;
    if (dl->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void**>(&attrs)) == S_OK) {
      BOOL ext = FALSE;
      attrs->GetFlag(BMDDeckLinkSupportsExternalKeying, &ext);
      info.externalKeying = ext != FALSE;
      attrs->Release();
    }
    out.push_back(info);
    dl->Release();
  }
  it->Release();
  return out;
}

uint32_t deckLinkModeFor(Rate r) {
  if (r == Rate{50, 1}) return bmdModeHD1080p50;
  if (r == Rate{60000, 1001}) return bmdModeHD1080p5994;
  if (r == Rate{60, 1}) return bmdModeHD1080p6000;
  throw std::invalid_argument("no DeckLink mode for rate " + r.str());
}
}  // namespace bc
```

- [ ] **Step 4: Input**

`src/io/decklink_input.h`:
```cpp
#pragma once
#include <atomic>
#include <mutex>
#include <thread>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

struct IDeckLink;
struct IDeckLinkInput;

namespace bc {
// DeckLink SDI/HDMI capture with input format detection (spec §4.2).
class DeckLinkInput final : public Input {
 public:
  DeckLinkInput(GpuDevice& gpu, TexturePool& pool, int deviceIndex) : gpu_(gpu), pool_(pool), index_(deviceIndex) {}
  ~DeckLinkInput() override { stop(); }
  void start(InputSink& sink) override;
  void stop() override;

  // Called by the SDK callback object.
  void onFormatChanged(uint32_t mode, bool rgb);
  void onFrame(const uint8_t* bytes, int width, int height, int rowBytes, bool noSignal, const int32_t* audio, int audioFrames);

 private:
  void run();
  GpuDevice& gpu_;
  TexturePool& pool_;
  int index_;
  InputSink* sink_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  IDeckLink* device_ = nullptr;
  IDeckLinkInput* input_ = nullptr;
  std::mutex m_;
  bool live_ = false;
};
}  // namespace bc
```

`src/io/decklink_input.cpp`:
```cpp
#include "io/decklink_input.h"

#include <windows.h>

#include <chrono>

#include "DeckLinkAPI_h.h"
#include "core/backoff.h"
#include "io/decklink.h"
#include "io/uyvy.h"

namespace bc {
namespace {
class Callback final : public IDeckLinkInputCallback {
 public:
  explicit Callback(DeckLinkInput* owner) : owner_(owner) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode* mode,
                                                    BMDDetectedVideoInputFormatFlags flags) override {
    owner_->onFormatChanged(mode->GetDisplayMode(), (flags & bmdDetectedVideoInputRGB444) != 0);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* frame, IDeckLinkAudioInputPacket* audio) override {
    const int32_t* samples = nullptr;
    int count = 0;
    if (audio) {
      void* p = nullptr;
      audio->GetBytes(&p);
      samples = static_cast<const int32_t*>(p);
      count = static_cast<int>(audio->GetSampleFrameCount());
    }
    if (!frame) {
      owner_->onFrame(nullptr, 0, 0, 0, true, samples, count);
      return S_OK;
    }
    const bool noSignal = (frame->GetFlags() & bmdFrameHasNoInputSource) != 0;
    IDeckLinkVideoBuffer* buf = nullptr;
    void* bytes = nullptr;
    if (frame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&buf)) == S_OK &&
        buf->StartAccess(bmdBufferAccessRead) == S_OK) {
      buf->GetBytes(&bytes);
      owner_->onFrame(static_cast<const uint8_t*>(bytes), static_cast<int>(frame->GetWidth()), static_cast<int>(frame->GetHeight()),
                      static_cast<int>(frame->GetRowBytes()), noSignal, samples, count);
      buf->EndAccess(bmdBufferAccessRead);
    }
    if (buf) buf->Release();
    return S_OK;
  }

 private:
  DeckLinkInput* owner_;
  std::atomic<ULONG> refs_{1};
};
}  // namespace

void DeckLinkInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
  thread_ = std::thread([this] { run(); });
}

void DeckLinkInput::stop() {
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
}

void DeckLinkInput::onFormatChanged(uint32_t mode, bool rgb) {
  std::lock_guard l(m_);
  if (!input_) return;
  input_->PauseStreams();
  input_->EnableVideoInput(mode, rgb ? bmdFormat8BitBGRA : bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection);
  input_->FlushStreams();
  input_->StartStreams();
}

void DeckLinkInput::onFrame(const uint8_t* bytes, int w, int h, int rowBytes, bool noSignal, const int32_t* audio, int audioFrames) {
  if (audio && audioFrames > 0) {
    AudioBlock blk;
    blk.samples.resize(static_cast<size_t>(audioFrames) * 2);
    for (size_t i = 0; i < blk.samples.size(); ++i) blk.samples[i] = static_cast<float>(audio[i]) / 2147483648.0f;
    sink_->onAudio(std::move(blk));
  }
  if (!bytes || noSignal) {
    if (live_) {
      live_ = false;
      sink_->onState(SignalState::Offline, ErrorInfo{"decklink-no-signal", "no signal on the DeckLink input"});
    }
    return;
  }
  // rowBytes == w*4 means the card delivered BGRA (RGB 4:4:4 detected); else UYVY.
  std::vector<uint8_t> bgra = rowBytes >= w * 4 ? std::vector<uint8_t>() : uyvyToBgra(bytes, w, h, rowBytes, h >= 720);
  VideoFrameIn f;
  f.layout = PixelLayout::BGRA8;
  f.fullRange = true;
  f.tex = pool_.acquire(static_cast<uint32_t>(w), static_cast<uint32_t>(h), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
  {
    auto lock = gpu_.lock();
    gpu_.context()->UpdateSubresource(f.tex->tex.Get(), 0, nullptr, bgra.empty() ? bytes : bgra.data(),
                                      static_cast<UINT>(bgra.empty() ? rowBytes : w * 4), 0);
  }
  if (!live_) {
    live_ = true;
    sink_->onState(SignalState::Live, std::nullopt);
  }
  sink_->onVideo(std::move(f));
}

void DeckLinkInput::run() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Backoff backoff;
  while (!stopping_) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    Callback* cb = nullptr;
    try {
      device_ = openDeckLink(index_);
      if (device_->QueryInterface(IID_IDeckLinkInput, reinterpret_cast<void**>(&input_)) != S_OK) {
        throw std::runtime_error("DeckLink device " + std::to_string(index_) + " has no input");
      }
      cb = new Callback(this);
      input_->SetCallback(cb);
      if (input_->EnableVideoInput(bmdModeHD1080p6000, bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection) != S_OK ||
          input_->EnableAudioInput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger, 2) != S_OK ||
          input_->StartStreams() != S_OK) {
        throw std::runtime_error("the DeckLink input could not start (in use by another app?)");
      }
      backoff.reset();
      while (!stopping_) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } catch (const std::exception& e) {
      sink_->onState(SignalState::Offline, ErrorInfo{"decklink-failed", e.what()});
    }
    {
      std::lock_guard l(m_);
      if (input_) {
        input_->StopStreams();
        input_->SetCallback(nullptr);
        input_->Release();
        input_ = nullptr;
      }
    }
    if (cb) cb->Release();
    if (device_) {
      device_->Release();
      device_ = nullptr;
    }
    for (auto d = backoff.next(); !stopping_ && d.count() > 0; d -= std::chrono::milliseconds(100)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  sink_->onState(SignalState::Offline, std::nullopt);
  CoUninitialize();
}
}  // namespace bc
```

- [ ] **Step 5: Output**

`src/io/decklink_output.h`:
```cpp
#pragma once
#include <vector>

#include "core/rate.h"
#include "io/queued_output.h"

struct IDeckLink;
struct IDeckLinkOutput;
struct IDeckLinkKeyer;
struct SwsContext;

namespace bc {
// DeckLink SDI/HDMI playout; keyed outputs use the external keyer (fill + key, spec §4.3).
class DeckLinkOutput final : public QueuedOutput {
 public:
  DeckLinkOutput(int deviceIndex, Rate showRate, bool keyed) : index_(deviceIndex), rate_(showRate), keyed_(keyed) {}
  ~DeckLinkOutput() override { stop(); }
  bool needsCpu() const override { return true; }

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "decklink-failed"; }

 private:
  int index_;
  Rate rate_;
  bool keyed_;
  IDeckLink* device_ = nullptr;
  IDeckLinkOutput* output_ = nullptr;
  IDeckLinkKeyer* keyer_ = nullptr;
  SwsContext* sws_ = nullptr;
  std::vector<int16_t> pcm_;
};
}  // namespace bc
```

`src/io/decklink_output.cpp`:
```cpp
#include "io/decklink_output.h"

#include <windows.h>

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "DeckLinkAPI_h.h"
#include "io/decklink.h"

namespace bc {
void DeckLinkOutput::open() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  device_ = openDeckLink(index_);
  if (device_->QueryInterface(IID_IDeckLinkOutput, reinterpret_cast<void**>(&output_)) != S_OK) {
    throw std::runtime_error("DeckLink device " + std::to_string(index_) + " has no output");
  }
  if (output_->EnableVideoOutput(deckLinkModeFor(rate_), bmdVideoOutputFlagDefault) != S_OK) {
    throw std::runtime_error("the DeckLink output could not start (in use, or the mode is not supported)");
  }
  output_->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 2, bmdAudioOutputStreamContinuous);
  if (keyed_) {
    if (device_->QueryInterface(IID_IDeckLinkKeyer, reinterpret_cast<void**>(&keyer_)) != S_OK || keyer_->Enable(TRUE) != S_OK) {
      throw std::runtime_error("this DeckLink card has no external keyer; use an unkeyed output");
    }
    keyer_->SetLevel(255);
  }
}

void DeckLinkOutput::deliver(const OutFrame& f) {
  if (!f.bgra || f.bgra->bgra.empty()) return;
  constexpr int W = 1920, H = 1080;
  IDeckLinkMutableVideoFrame* frame = nullptr;
  if (output_->CreateVideoFrame(W, H, W * 4, bmdFormat8BitBGRA, bmdFrameFlagDefault, &frame) != S_OK) {
    throw std::runtime_error("cannot allocate a DeckLink frame");
  }
  IDeckLinkVideoBuffer* buf = nullptr;
  void* bytes = nullptr;
  frame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&buf));
  buf->StartAccess(bmdBufferAccessWrite);
  buf->GetBytes(&bytes);
  if (static_cast<int>(f.bgra->width) == W && static_cast<int>(f.bgra->height) == H) {
    std::memcpy(bytes, f.bgra->bgra.data(), static_cast<size_t>(W) * H * 4);
  } else {
    sws_ = sws_getCachedContext(sws_, static_cast<int>(f.bgra->width), static_cast<int>(f.bgra->height), AV_PIX_FMT_BGRA, W, H,
                                AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    const uint8_t* src[1] = {f.bgra->bgra.data()};
    const int ss[1] = {static_cast<int>(f.bgra->width * 4)};
    uint8_t* dst[1] = {static_cast<uint8_t*>(bytes)};
    const int ds[1] = {W * 4};
    sws_scale(sws_, src, ss, 0, static_cast<int>(f.bgra->height), dst, ds);
  }
  if (!f.hasAlpha) {  // unkeyed frames must be opaque on the key output
    auto* p = static_cast<uint8_t*>(bytes);
    for (size_t i = 3; i < static_cast<size_t>(W) * H * 4; i += 4) p[i] = 255;
  }
  buf->EndAccess(bmdBufferAccessWrite);
  buf->Release();
  const HRESULT hr = output_->DisplayVideoFrameSync(frame);
  frame->Release();
  if (hr != S_OK) throw std::runtime_error("the DeckLink output stopped accepting frames");
  if (f.audio && !f.audio->empty()) {
    pcm_.resize(f.audio->size());
    for (size_t i = 0; i < pcm_.size(); ++i) pcm_[i] = static_cast<int16_t>(std::clamp((*f.audio)[i], -1.0f, 1.0f) * 32767.0f);
    uint32_t written = 0;
    output_->WriteAudioSamplesSync(pcm_.data(), static_cast<uint32_t>(pcm_.size() / 2), &written);
  }
}

void DeckLinkOutput::close() noexcept {
  if (keyer_) {
    keyer_->Disable();
    keyer_->Release();
    keyer_ = nullptr;
  }
  if (output_) {
    output_->DisableAudioOutput();
    output_->DisableVideoOutput();
    output_->Release();
    output_ = nullptr;
  }
  if (device_) {
    device_->Release();
    device_ = nullptr;
  }
  sws_freeContext(sws_);
  sws_ = nullptr;
}
}  // namespace bc
```
ponytail: `DisplayVideoFrameSync` paces nothing (the tick does); scheduled playback with a preroll would remove jitter on long shows.

- [ ] **Step 6: Hardware tests**

`tests/decklink_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_DECKLINK
#include <thread>

#include "gpu/device.h"
#include "io/decklink.h"
#include "io/decklink_input.h"
#include "io/decklink_output.h"

using namespace std::chrono_literals;

TEST_CASE("DeckLink: a missing device index is reported, not fatal") {
  bc::DeckLinkOutput out(99, {60, 1}, false);
  out.start();
  std::this_thread::sleep_for(500ms);
  auto st = out.status();
  CHECK(st.state == bc::SignalState::Offline);
  REQUIRE(st.error);
  CHECK(st.error->code == "decklink-failed");
  out.stop();
}

TEST_CASE("DeckLink: the first card's output plays 2 s of frames" BC_REQUIRES(bc::test::Req::DeckLink)) {
  auto info = bc::listDeckLinkDevices().front();
  MESSAGE("card: " << info.name << " in=" << info.hasInput << " out=" << info.hasOutput << " extKey=" << info.externalKeying);
  if (!info.hasOutput) return;
  bc::DeckLinkOutput out(info.index, {60, 1}, false);
  out.start();
  auto img = std::make_shared<bc::CpuImage>();
  img->width = 1920;
  img->height = 1080;
  img->bgra.assign(1920 * 1080 * 4, 100);
  for (uint64_t t = 0; t < 120; ++t) {
    bc::OutFrame f;
    f.tick = t;
    f.rate = {60, 1};
    f.bgra = img;
    out.submit(std::move(f));
    std::this_thread::sleep_for(16ms);
  }
  CHECK(out.status().state == bc::SignalState::Live);
  CHECK_FALSE(out.status().error);
  out.stop();
}
#else
TEST_CASE("DeckLink: the first card's output plays 2 s of frames" BC_REQUIRES(bc::test::Req::DeckLink)) {}
#endif
```
Add it to `bc_tests`.

- [ ] **Step 7: Build both ways and run**

1. Without `DECKLINK_SDK_DIR`: `scripts/build.ps1` → configure prints `BC_WITH_DECKLINK=OFF`, build succeeds; `-Filter "UYVY*,DeckLink*"` → UYVY passes, DeckLink skipped.
2. If the owner installed the SDK: build ON; fix compile errors only against the installed headers and ledger each; run the tests (card tests skip without a card).

- [ ] **Step 8: Commit**

```bash
git add cmake/FindDeckLink.cmake src/io tests CMakeLists.txt
git commit -m "feat(io): DeckLink input (format detection) and output (external key+fill), compile-gated"
```

---

### Task 2: AJA NTV2 input and output (experimental, fill only)

**Files:**
- Create: `src/io/aja.h`, `src/io/aja.cpp`, `src/io/aja_input.h`, `src/io/aja_input.cpp`, `src/io/aja_output.h`, `src/io/aja_output.cpp`
- Modify: `CMakeLists.txt` (FetchContent + `BC_WITH_AJA`), `tests/support/requires.h/.cpp` (`Req::Aja`)
- Test: `tests/aja_test.cpp`

**Interfaces:**
- Consumes: `uyvyToBgra`, `bgraToUyvy` (Task 1); M1 adapter interfaces.
- Produces: `bc::AjaInfo { int index; std::string name; }`, `std::vector<AjaInfo> bc::listAjaDevices()`, `bc::AjaInput(GpuDevice&, TexturePool&, int deviceIndex, int channel /*1..8*/)`, `bc::AjaOutput(int deviceIndex, int channel, Rate showRate)` (error code `"aja-failed"`); `Req::Aja`.

libajantv2 facts (tag `ntv2_18_0_0`, MIT, CMake options `AJANTV2_BUILD_SHARED` (OFF), `AJANTV2_DISABLE_DEMOS`, `AJANTV2_DISABLE_TESTS`, `AJANTV2_DISABLE_TOOLS`, `AJANTV2_DISABLE_DRIVER`, `AJANTV2_DISABLE_PLUGIN_LOAD`, `AJANTV2_DISABLE_RDMA`; library target `ajantv2`): `CNTV2DeviceScanner::GetDeviceAtIndex(ULWord, CNTV2Card&)`, `CNTV2Card::GetDisplayName()`, `AutoCirculateInitForInput(channel, frameCount=7, audioSystem, optionFlags)`, `AutoCirculateInitForOutput(...)`, `AutoCirculateStart(channel)`, `AutoCirculateStop(channel)`, `AutoCirculateGetStatus(channel, AUTOCIRCULATE_STATUS&)`, `AutoCirculateTransfer(channel, AUTOCIRCULATE_TRANSFER&)`, `AUTOCIRCULATE_TRANSFER::SetVideoBuffer(ULWord*, ULWord)`, `SetAudioBuffer(ULWord*, ULWord)`, `GetCapturedAudioByteCount()`. Routing/format helpers used: `CNTV2Card::GetInputVideoFormat(NTV2InputSource)`, `SetVideoFormat`, `SetFrameBufferFormat(ch, NTV2_FBF_8BIT_YCBCR)`, `SetMode(ch, NTV2_MODE_CAPTURE | NTV2_MODE_DISPLAY)`, `EnableChannel`, `SetSDITransmitEnable`, `Connect`, `::NTV2ChannelToInputSource`, `::GetFrameBufferInputXptFromChannel`, `::GetSDIInputOutputXptFromChannel`, `::GetSDIOutputInputXpt`, `::GetFrameBufferOutputXptFromChannel`, `::NTV2ChannelToAudioSystem`, `::NTV2ChannelToEmbeddedAudioInput`, `SetAudioSystemInputSource`, `SetNumberAudioChannels`, `SetAudioRate`, `::GetVideoWriteSize(NTV2VideoFormat, NTV2FrameBufferFormat)`. If a helper's name differs in 18.0, use the equivalent the NTV2 demos (`demos/ntv2capture`, `demos/ntv2player`) use and ledger it.

- [ ] **Step 1: Fetch and build libajantv2**

In `CMakeLists.txt`, before `bc_io`:
```cmake
option(BC_WITH_AJA "Build the AJA adapters (experimental)" ON)
if(BC_WITH_AJA)
  include(FetchContent)
  set(AJANTV2_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  foreach(o DEMOS TESTS TOOLS DRIVER PLUGIN_LOAD RDMA)
    set(AJANTV2_DISABLE_${o} ON CACHE BOOL "" FORCE)
  endforeach()
  FetchContent_Declare(libajantv2 GIT_REPOSITORY https://github.com/aja-video/libajantv2.git GIT_TAG ntv2_18_0_0 GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(libajantv2)
  if(NOT TARGET ajantv2)
    message(WARNING "libajantv2 did not provide the ajantv2 target; turning AJA off")
    set(BC_WITH_AJA OFF)
  endif()
endif()
message(STATUS "BC_WITH_AJA=${BC_WITH_AJA}")
```
after `bc_io`:
```cmake
if(BC_WITH_AJA)
  target_sources(bc_io PRIVATE src/io/aja.cpp src/io/aja_input.cpp src/io/aja_output.cpp)
  target_link_libraries(bc_io PUBLIC ajantv2)
  target_compile_definitions(bc_io PUBLIC BC_WITH_AJA=1)
else()
  target_compile_definitions(bc_io PUBLIC BC_WITH_AJA=0)
endif()
```
Run `scripts/build.ps1`. If libajantv2's own build fails under VS 2026, set `BC_WITH_AJA` default OFF, ledger the compiler error verbatim, and continue the task with the code compiled only when ON — the planner decides.

- [ ] **Step 2: Listing, input, output**

`src/io/aja.h`:
```cpp
#pragma once
#include <string>
#include <vector>

namespace bc {
struct AjaInfo {
  int index = 0;
  std::string name;
};
std::vector<AjaInfo> listAjaDevices();
}  // namespace bc
```

`src/io/aja.cpp`:
```cpp
#include "io/aja.h"

#include "ntv2card.h"
#include "ntv2devicescanner.h"

namespace bc {
std::vector<AjaInfo> listAjaDevices() {
  std::vector<AjaInfo> out;
  for (ULWord i = 0; i < 8; ++i) {
    CNTV2Card card;
    if (!CNTV2DeviceScanner::GetDeviceAtIndex(i, card)) break;
    out.push_back({static_cast<int>(i), card.GetDisplayName()});
  }
  return out;
}
}  // namespace bc
```

`src/io/aja_input.h`:
```cpp
#pragma once
#include <atomic>
#include <thread>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

namespace bc {
// AJA SDI capture through AutoCirculate (spec §4.2). EXPERIMENTAL — not verified on hardware.
class AjaInput final : public Input {
 public:
  AjaInput(GpuDevice& gpu, TexturePool& pool, int deviceIndex, int channel)
      : gpu_(gpu), pool_(pool), index_(deviceIndex), channel_(channel) {}
  ~AjaInput() override { stop(); }
  void start(InputSink& sink) override;
  void stop() override;

 private:
  void run();
  void runOnce();
  GpuDevice& gpu_;
  TexturePool& pool_;
  int index_, channel_;
  InputSink* sink_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};
}  // namespace bc
```

`src/io/aja_input.cpp`:
```cpp
#include "io/aja_input.h"

#include <chrono>
#include <stdexcept>
#include <vector>

#include "core/backoff.h"
#include "io/uyvy.h"
#include "ntv2card.h"
#include "ntv2devicescanner.h"
#include "ntv2utils.h"

namespace bc {
void AjaInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
  thread_ = std::thread([this] { run(); });
}

void AjaInput::stop() {
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
}

void AjaInput::run() {
  Backoff backoff;
  while (!stopping_) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    try {
      runOnce();
    } catch (const std::exception& e) {
      if (!stopping_) sink_->onState(SignalState::Offline, ErrorInfo{"aja-failed", e.what()});
    }
    for (auto d = backoff.next(); !stopping_ && d.count() > 0; d -= std::chrono::milliseconds(100)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  sink_->onState(SignalState::Offline, std::nullopt);
}

void AjaInput::runOnce() {
  CNTV2Card card;
  if (!CNTV2DeviceScanner::GetDeviceAtIndex(static_cast<ULWord>(index_), card)) {
    throw std::runtime_error("AJA device " + std::to_string(index_) + " not found");
  }
  const auto ch = static_cast<NTV2Channel>(channel_ - 1);
  const NTV2InputSource src = ::NTV2ChannelToInputSource(ch);
  card.EnableChannel(ch);
  card.SetSDITransmitEnable(ch, false);
  const NTV2VideoFormat vf = card.GetInputVideoFormat(src);
  if (vf == NTV2_FORMAT_UNKNOWN) throw std::runtime_error("no signal on AJA SDI input " + std::to_string(channel_));
  card.SetVideoFormat(vf, false, false, ch);
  card.SetFrameBufferFormat(ch, NTV2_FBF_8BIT_YCBCR);
  card.SetMode(ch, NTV2_MODE_CAPTURE);
  card.Connect(::GetFrameBufferInputXptFromChannel(ch), ::GetSDIInputOutputXptFromChannel(ch));
  const NTV2AudioSystem as = ::NTV2ChannelToAudioSystem(ch);
  card.SetAudioSystemInputSource(as, NTV2_AUDIO_EMBEDDED, ::NTV2ChannelToEmbeddedAudioInput(ch));
  card.SetNumberAudioChannels(8, as);
  card.SetAudioRate(NTV2_AUDIO_48K, as);
  card.AutoCirculateStop(ch);
  if (!card.AutoCirculateInitForInput(ch, 7, as) || !card.AutoCirculateStart(ch)) {
    throw std::runtime_error("AJA AutoCirculate could not start on channel " + std::to_string(channel_));
  }
  const NTV2FormatDescriptor fd(vf, NTV2_FBF_8BIT_YCBCR);
  const int w = static_cast<int>(fd.GetRasterWidth()), h = static_cast<int>(fd.GetRasterHeight());
  const int rowBytes = static_cast<int>(fd.GetBytesPerRow());
  std::vector<ULWord> video(static_cast<size_t>(rowBytes) * h / 4);
  std::vector<ULWord> audio(NTV2_AUDIOSIZE_MAX / 4);
  AUTOCIRCULATE_TRANSFER xfer;
  xfer.SetVideoBuffer(video.data(), static_cast<ULWord>(video.size() * 4));
  xfer.SetAudioBuffer(audio.data(), static_cast<ULWord>(audio.size() * 4));
  bool live = false;
  while (!stopping_) {
    AUTOCIRCULATE_STATUS st;
    card.AutoCirculateGetStatus(ch, st);
    if (!(st.IsRunning() && st.HasAvailableInputFrame())) {
      card.WaitForInputVerticalInterrupt(ch);
      if (card.GetInputVideoFormat(src) != vf) break;  // format change: re-init from the top
      continue;
    }
    card.AutoCirculateTransfer(ch, xfer);
    auto bgra = uyvyToBgra(reinterpret_cast<const uint8_t*>(video.data()), w, h, rowBytes, h >= 720);
    VideoFrameIn f;
    f.layout = PixelLayout::BGRA8;
    f.fullRange = true;
    f.tex = pool_.acquire(static_cast<uint32_t>(w), static_cast<uint32_t>(h), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    {
      auto lock = gpu_.lock();
      gpu_.context()->UpdateSubresource(f.tex->tex.Get(), 0, nullptr, bgra.data(), static_cast<UINT>(w * 4), 0);
    }
    const ULWord bytes = xfer.GetCapturedAudioByteCount();
    const int frames = static_cast<int>(bytes / (8 * 4));  // 8 channels × int32
    if (frames > 0) {
      AudioBlock blk;
      blk.samples.resize(static_cast<size_t>(frames) * 2);
      const auto* s = reinterpret_cast<const int32_t*>(audio.data());
      for (int i = 0; i < frames; ++i) {
        blk.samples[static_cast<size_t>(i) * 2] = static_cast<float>(s[i * 8]) / 2147483648.0f;
        blk.samples[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(s[i * 8 + 1]) / 2147483648.0f;
      }
      sink_->onAudio(std::move(blk));
    }
    if (!live) {
      live = true;
      sink_->onState(SignalState::Live, std::nullopt);
    }
    sink_->onVideo(std::move(f));
  }
  card.AutoCirculateStop(ch);
}
}  // namespace bc
```

`src/io/aja_output.h`:
```cpp
#pragma once
#include <memory>
#include <vector>

#include "core/rate.h"
#include "io/queued_output.h"

class CNTV2Card;

namespace bc {
// AJA SDI playout, fill only (spec §4.3 deviation, see follow-ups). EXPERIMENTAL.
class AjaOutput final : public QueuedOutput {
 public:
  AjaOutput(int deviceIndex, int channel, Rate showRate);
  ~AjaOutput() override;
  bool needsCpu() const override { return true; }

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "aja-failed"; }

 private:
  int index_, channel_;
  Rate rate_;
  std::unique_ptr<CNTV2Card> card_;
  std::vector<uint8_t> uyvy_, scaled_;
  std::vector<int32_t> pcm_;
};
}  // namespace bc
```

`src/io/aja_output.cpp`:
```cpp
#include "io/aja_output.h"

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <stdexcept>

#include "io/uyvy.h"
#include "ntv2card.h"
#include "ntv2devicescanner.h"
#include "ntv2utils.h"

namespace bc {
namespace {
NTV2VideoFormat formatFor(Rate r) {
  if (r == Rate{50, 1}) return NTV2_FORMAT_1080p_5000_A;
  if (r == Rate{60000, 1001}) return NTV2_FORMAT_1080p_5994_A;
  if (r == Rate{60, 1}) return NTV2_FORMAT_1080p_6000_A;
  throw std::invalid_argument("no AJA format for rate " + r.str());
}
}  // namespace

AjaOutput::AjaOutput(int deviceIndex, int channel, Rate showRate) : index_(deviceIndex), channel_(channel), rate_(showRate) {}
AjaOutput::~AjaOutput() { stop(); }

void AjaOutput::open() {
  card_ = std::make_unique<CNTV2Card>();
  if (!CNTV2DeviceScanner::GetDeviceAtIndex(static_cast<ULWord>(index_), *card_)) {
    throw std::runtime_error("AJA device " + std::to_string(index_) + " not found");
  }
  const auto ch = static_cast<NTV2Channel>(channel_ - 1);
  card_->EnableChannel(ch);
  card_->SetVideoFormat(formatFor(rate_), false, false, ch);
  card_->SetFrameBufferFormat(ch, NTV2_FBF_8BIT_YCBCR);
  card_->SetMode(ch, NTV2_MODE_DISPLAY);
  card_->SetSDITransmitEnable(ch, true);
  card_->Connect(::GetSDIOutputInputXpt(ch), ::GetFrameBufferOutputXptFromChannel(ch));
  const NTV2AudioSystem as = ::NTV2ChannelToAudioSystem(ch);
  card_->SetNumberAudioChannels(8, as);
  card_->SetAudioRate(NTV2_AUDIO_48K, as);
  card_->AutoCirculateStop(ch);
  if (!card_->AutoCirculateInitForOutput(ch, 7, as) || !card_->AutoCirculateStart(ch)) {
    throw std::runtime_error("AJA AutoCirculate could not start on output " + std::to_string(channel_));
  }
}

void AjaOutput::deliver(const OutFrame& f) {
  if (!f.bgra || f.bgra->bgra.empty()) return;
  const auto ch = static_cast<NTV2Channel>(channel_ - 1);
  AUTOCIRCULATE_STATUS st;
  card_->AutoCirculateGetStatus(ch, st);
  if (!st.CanAcceptMoreOutputFrames()) return;  // drop rather than block (spec §4.1)
  constexpr int W = 1920, H = 1080;
  const uint8_t* bgra = f.bgra->bgra.data();
  if (static_cast<int>(f.bgra->width) != W || static_cast<int>(f.bgra->height) != H) {
    scaled_.resize(static_cast<size_t>(W) * H * 4);
    SwsContext* s = sws_getContext(static_cast<int>(f.bgra->width), static_cast<int>(f.bgra->height), AV_PIX_FMT_BGRA, W, H,
                                   AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    const uint8_t* src[1] = {bgra};
    const int ss[1] = {static_cast<int>(f.bgra->width * 4)};
    uint8_t* dst[1] = {scaled_.data()};
    const int ds[1] = {W * 4};
    sws_scale(s, src, ss, 0, static_cast<int>(f.bgra->height), dst, ds);
    sws_freeContext(s);
    bgra = scaled_.data();
  }
  uyvy_.resize(static_cast<size_t>(W) * H * 2);
  bgraToUyvy(bgra, W, H, uyvy_.data(), W * 2, true);
  AUTOCIRCULATE_TRANSFER xfer;
  xfer.SetVideoBuffer(reinterpret_cast<ULWord*>(uyvy_.data()), static_cast<ULWord>(uyvy_.size()));
  if (f.audio && !f.audio->empty()) {
    const size_t frames = f.audio->size() / 2;
    pcm_.assign(frames * 8, 0);
    for (size_t i = 0; i < frames; ++i) {
      pcm_[i * 8] = static_cast<int32_t>(std::clamp((*f.audio)[i * 2], -1.0f, 1.0f) * 2147483647.0f);
      pcm_[i * 8 + 1] = static_cast<int32_t>(std::clamp((*f.audio)[i * 2 + 1], -1.0f, 1.0f) * 2147483647.0f);
    }
    xfer.SetAudioBuffer(reinterpret_cast<ULWord*>(pcm_.data()), static_cast<ULWord>(pcm_.size() * 4));
  }
  if (!card_->AutoCirculateTransfer(ch, xfer)) throw std::runtime_error("the AJA output stopped accepting frames");
}

void AjaOutput::close() noexcept {
  if (card_) {
    card_->AutoCirculateStop(static_cast<NTV2Channel>(channel_ - 1));
    card_.reset();
  }
}
}  // namespace bc
```

- [ ] **Step 3: Tests**

`tests/aja_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_AJA
#include <thread>

#include "io/aja.h"
#include "io/aja_output.h"

using namespace std::chrono_literals;

TEST_CASE("AJA: listing runs; a missing device is reported, not fatal") {
  MESSAGE(bc::listAjaDevices().size() << " AJA device(s)");
  bc::AjaOutput out(7, 1, {60, 1});
  out.start();
  std::this_thread::sleep_for(500ms);
  auto st = out.status();
  CHECK(st.state == bc::SignalState::Offline);
  REQUIRE(st.error);
  CHECK(st.error->code == "aja-failed");
  out.stop();
}

TEST_CASE("AJA: the first card's output 1 plays 2 s of frames" BC_REQUIRES(bc::test::Req::Aja)) {
  bc::AjaOutput out(0, 1, {60, 1});
  out.start();
  auto img = std::make_shared<bc::CpuImage>();
  img->width = 1920;
  img->height = 1080;
  img->bgra.assign(1920 * 1080 * 4, 100);
  for (uint64_t t = 0; t < 120; ++t) {
    bc::OutFrame f;
    f.tick = t;
    f.rate = {60, 1};
    f.bgra = img;
    out.submit(std::move(f));
    std::this_thread::sleep_for(16ms);
  }
  CHECK_FALSE(out.status().error);
  out.stop();
}
#else
TEST_CASE("AJA: the first card's output 1 plays 2 s of frames" BC_REQUIRES(bc::test::Req::Aja)) {}
#endif
```
`requires`: `Aja` → missing if `BC_WITH_AJA == 0` or `bc::listAjaDevices().empty()`.

Run `scripts/test.ps1 -Filter "AJA*"` — the first case runs (no card → offline error) when AJA is built; the card case skips.

- [ ] **Step 4: Commit**

```bash
git add src/io tests CMakeLists.txt
git commit -m "feat(io): AJA NTV2 input and fill-only output (experimental, libajantv2 18.0)"
```

---

### Task 3: Windows virtual camera (media source DLL + engine output)

**Files:**
- Create: `src/core/vcam_shared.h`, `src/core/vcam_shared.cpp`, `src/io/vcam_output.h`, `src/io/vcam_output.cpp`, `vcam/` (vendored source, see Step 2), `vcam/CMakeLists.txt`
- Modify: `CMakeLists.txt` (`BC_WITH_VCAM`, `add_subdirectory(vcam)`), `tests/support/requires.h/.cpp` (`Req::VcamInstalled`)
- Test: `tests/vcam_shared_test.cpp`, `tests/vcam_output_test.cpp`

**Interfaces:**
- Consumes: `QueuedOutput`, `OutFrame` (M1), `MfCaptureInput`, `listCaptureDevices` (M4a, test only).
- Produces:
  - `src/core/vcam_shared.h`: namespace `bc::vcam` — `kWidth = 1920`, `kHeight = 1080`, `kFrameBytes`, `kMappingBytes`, `std::wstring mappingName(int index, bool global = true)` (`Global\BreadCameraVcam<i>` / `Local\…` for tests), `const wchar_t* kClsids[4]` (string CLSIDs for indexes 1..4), `class Writer { bool open(int index, bool global = true); bool write(const uint8_t* bgra1080p); void close(); }`, `class Reader { bool create(int index, bool global = true); /* DLL side: CreateFileMapping with an SDDL that lets Authenticated Users read/write */ bool readLatest(uint8_t* dstBgra1080p, std::chrono::milliseconds maxAge); void close(); }`.
  - `bc::VirtualCameraOutput(int index /*1..4*/, std::string friendlyName)` (BC_WITH_VCAM; `needsCpu() == true`; error code `"vcam-failed"`).
  - Target `breadcamera-vcam` (SHARED, the media source DLL).
  - `Req::VcamInstalled` (missing unless Windows build ≥ 22000 and `HKLM\Software\Classes\CLSID\<kClsids[0]>\InprocServer32` exists).

Shared protocol: the **DLL** (running inside the Frame Server service, which may create `Global\` objects) creates the section with SDDL `D:(A;;GA;;;SY)(A;;GRGW;;;AU)`. The engine opens it only while a consumer has the camera open; if it cannot, the frame is dropped quietly. Header (64 bytes): `uint32 magic 'BCAM'`, `uint32 version 1`, `int64 seq` (odd while writing), `int64 writtenAtQpc`, `uint32 width`, `uint32 height`; then 1920×1080 BGRA, top-down. Writer: `seq++` (odd), copy, `seq++` (even), with release/acquire fences (`InterlockedIncrement64` + `MemoryBarrier`). Reader: read `seq` (must be even), copy, re-read `seq` — equal → good frame; otherwise keep the previous frame. A frame older than `maxAge` (1 s) → the DLL shows its built-in slate.

- [ ] **Step 1: The shared protocol, with tests (always built)**

`tests/vcam_shared_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "core/vcam_shared.h"

using namespace std::chrono_literals;

TEST_CASE("vcam shared memory: writer to reader, and stale frames") {
  bc::vcam::Reader r;
  REQUIRE(r.create(9, false));  // Local\ for tests
  bc::vcam::Writer w;
  REQUIRE(w.open(9, false));
  std::vector<uint8_t> frame(bc::vcam::kFrameBytes, 0x40), got(bc::vcam::kFrameBytes, 0);
  CHECK_FALSE(r.readLatest(got.data(), 1000ms));  // nothing written yet
  CHECK(w.write(frame.data()));
  REQUIRE(r.readLatest(got.data(), 1000ms));
  CHECK(got[12345] == 0x40);
  std::this_thread::sleep_for(60ms);
  CHECK_FALSE(r.readLatest(got.data(), 50ms));  // too old
  w.close();
  r.close();
}

TEST_CASE("vcam shared memory: a reader never accepts a torn frame") {
  bc::vcam::Reader r;
  REQUIRE(r.create(10, false));
  bc::vcam::Writer w;
  REQUIRE(w.open(10, false));
  std::atomic<bool> run{true};
  std::thread writer([&] {
    std::vector<uint8_t> a(bc::vcam::kFrameBytes, 0x11), b(bc::vcam::kFrameBytes, 0x22);
    for (int i = 0; run; ++i) w.write(i % 2 ? a.data() : b.data());
  });
  std::vector<uint8_t> got(bc::vcam::kFrameBytes);
  int good = 0;
  for (int i = 0; i < 200; ++i) {
    if (!r.readLatest(got.data(), 1000ms)) continue;
    ++good;
    const uint8_t first = got[0];
    CHECK((first == 0x11 || first == 0x22));
    CHECK(got[bc::vcam::kFrameBytes - 1] == first);  // whole frame from one write
  }
  run = false;
  writer.join();
  CHECK(good > 0);
}

TEST_CASE("vcam: the writer opens nothing when no reader exists") {
  bc::vcam::Writer w;
  CHECK_FALSE(w.open(11, false));
}
```

`src/core/vcam_shared.h`:
```cpp
#pragma once
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace bc::vcam {
constexpr uint32_t kWidth = 1920, kHeight = 1080;
constexpr size_t kFrameBytes = size_t{kWidth} * kHeight * 4;
constexpr size_t kHeaderBytes = 64;
constexpr size_t kMappingBytes = kHeaderBytes + kFrameBytes;
constexpr uint32_t kMagic = 0x4D414342;  // "BCAM"

// Fixed CLSIDs of the four BreadCamera virtual cameras (the DLL registers all four).
inline constexpr const wchar_t* kClsids[4] = {
    L"{7B1E3A40-5C2D-4E8F-9A61-0B3C4D5E6F01}", L"{7B1E3A40-5C2D-4E8F-9A61-0B3C4D5E6F02}",
    L"{7B1E3A40-5C2D-4E8F-9A61-0B3C4D5E6F03}", L"{7B1E3A40-5C2D-4E8F-9A61-0B3C4D5E6F04}"};

inline std::wstring mappingName(int index, bool global = true) {
  return std::wstring(global ? L"Global\\" : L"Local\\") + L"BreadCameraVcam" + std::to_wstring(index);
}

struct Header {
  uint32_t magic;
  uint32_t version;
  volatile LONG64 seq;
  int64_t writtenAtQpc;
  uint32_t width, height;
};

// Engine side: opens the section the DLL created; false when no app has the camera open.
class Writer {
 public:
  ~Writer() { close(); }
  bool open(int index, bool global = true);
  bool write(const uint8_t* bgra1080p);
  void close();
  bool isOpen() const { return view_ != nullptr; }

 private:
  HANDLE map_ = nullptr;
  uint8_t* view_ = nullptr;
};

// DLL side: creates the section (with a DACL the user's engine can open) and reads frames.
class Reader {
 public:
  ~Reader() { close(); }
  bool create(int index, bool global = true);
  bool readLatest(uint8_t* dstBgra1080p, std::chrono::milliseconds maxAge);
  void close();

 private:
  HANDLE map_ = nullptr;
  uint8_t* view_ = nullptr;
};
}  // namespace bc::vcam
```

`src/core/vcam_shared.cpp`:
```cpp
#include "core/vcam_shared.h"

#include <sddl.h>

#include <cstring>

namespace bc::vcam {
namespace {
int64_t qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
int64_t qpcFreq() {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return f.QuadPart;
}
}  // namespace

bool Writer::open(int index, bool global) {
  close();
  map_ = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, mappingName(index, global).c_str());
  if (!map_) return false;
  view_ = static_cast<uint8_t*>(MapViewOfFile(map_, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, kMappingBytes));
  if (!view_) {
    close();
    return false;
  }
  return true;
}

bool Writer::write(const uint8_t* bgra) {
  if (!view_) return false;
  auto* h = reinterpret_cast<Header*>(view_);
  InterlockedIncrement64(&h->seq);  // odd: writing
  MemoryBarrier();
  std::memcpy(view_ + kHeaderBytes, bgra, kFrameBytes);
  h->width = kWidth;
  h->height = kHeight;
  h->writtenAtQpc = qpc();
  MemoryBarrier();
  InterlockedIncrement64(&h->seq);  // even: done
  return true;
}

void Writer::close() {
  if (view_) UnmapViewOfFile(view_);
  if (map_) CloseHandle(map_);
  view_ = nullptr;
  map_ = nullptr;
}

bool Reader::create(int index, bool global) {
  close();
  PSECURITY_DESCRIPTOR sd = nullptr;
  // SYSTEM full control, Authenticated Users read/write (the engine runs as the signed-in user).
  ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GRGW;;;AU)", SDDL_REVISION_1, &sd, nullptr);
  SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
  map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, sd ? &sa : nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(kMappingBytes),
                            mappingName(index, global).c_str());
  if (sd) LocalFree(sd);
  if (!map_) return false;
  view_ = static_cast<uint8_t*>(MapViewOfFile(map_, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, kMappingBytes));
  if (!view_) {
    close();
    return false;
  }
  auto* h = reinterpret_cast<Header*>(view_);
  if (h->magic != kMagic) {
    h->magic = kMagic;
    h->version = 1;
    h->seq = 0;
    h->writtenAtQpc = 0;
  }
  return true;
}

bool Reader::readLatest(uint8_t* dst, std::chrono::milliseconds maxAge) {
  if (!view_) return false;
  auto* h = reinterpret_cast<Header*>(view_);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const LONG64 before = h->seq;
    MemoryBarrier();
    if ((before & 1) || before == 0) continue;
    const int64_t at = h->writtenAtQpc;
    std::memcpy(dst, view_ + kHeaderBytes, kFrameBytes);
    MemoryBarrier();
    if (h->seq != before) continue;  // torn: try once more, else keep the previous frame
    const int64_t ageMs = (qpc() - at) * 1000 / qpcFreq();
    return ageMs <= maxAge.count();
  }
  return false;
}

void Reader::close() {
  if (view_) UnmapViewOfFile(view_);
  if (map_) CloseHandle(map_);
  view_ = nullptr;
  map_ = nullptr;
}
}  // namespace bc::vcam
```
Note: when a read fails, the caller (the DLL) keeps its own previous frame buffer — `readLatest` writes into `dst` even on a torn read, so the DLL must read into a scratch buffer and copy on success (Step 2 does).

Add `src/core/vcam_shared.cpp` to `bc_core` (link `advapi32`), the test to `bc_tests`. Run `-Filter "vcam*"` → 3 cases pass.

- [ ] **Step 2: Vendor the media source and feed it from shared memory**

1. Copy `smourier/VCamSample` at commit `58b6d4b0e0c2c97fdffd66d23e7c8da6b89f570d`, folder `VCamSampleSource/`, into `vcam/source/` (all `.cpp`, `.h`, `.def`, `.rc`, `resource.h`; not the `.vcxproj*`/`packages.config`), plus its `LICENSE` as `vcam/LICENSE`. Add a header line to `vcam/README.md`: "Vendored from github.com/smourier/VCamSample @ 58b6d4b (MIT). Changes: four CLSIDs, frames from BreadCamera shared memory."
2. `vcam/CMakeLists.txt`:
```cmake
file(GLOB VCAM_SRC ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp)
add_library(breadcamera-vcam SHARED ${VCAM_SRC} ${CMAKE_CURRENT_SOURCE_DIR}/source/VCamSampleSource.def
            ${CMAKE_CURRENT_SOURCE_DIR}/source/VCamSampleSource.rc ${CMAKE_SOURCE_DIR}/src/core/vcam_shared.cpp)
target_include_directories(breadcamera-vcam PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(breadcamera-vcam PRIVATE _WINDOWS _USRDLL)
target_compile_options(breadcamera-vcam PRIVATE /await:strict /bigobj)
target_link_libraries(breadcamera-vcam PRIVATE WIL::WIL mfplat mfuuid mf d2d1 dwrite d3d11 dxgi windowscodecs advapi32 runtimeobject)
```
   In the root `CMakeLists.txt`: `include(CheckIncludeFileCXX)`, `check_include_file_cxx(mfvirtualcamera.h HAVE_MFVCAM)`, `option(BC_WITH_VCAM "Build the virtual camera" ${HAVE_MFVCAM})`, and `if(BC_WITH_VCAM) add_subdirectory(vcam) endif()`. If the sample's `pch.h`/`framework.h` include NuGet-only headers, replace them with the vcpkg WIL and the Windows SDK's C++/WinRT (`winrt/base.h` ships with the SDK) and ledger each change.
3. `vcam/source/dllmain.cpp` — four classes instead of one:
   - Replace the single `GUID CLSID_VCam = …` with an array parsed from `bc::vcam::kClsids` (`CLSIDFromString` at DLL load into `GUID g_clsids[4]`).
   - `DllGetClassObject`: if `rclsid == g_clsids[i]`, return the class factory with an `index = i + 1` member that it passes to `Activator::Initialize(index)` (add the parameter, store it, and pass it to `MediaSource` → `MediaStream` → `FrameGenerator`).
   - `DllRegisterServer` / `DllUnregisterServer`: loop over the four CLSIDs (HKLM, exactly as the sample does for one).
4. `vcam/source/MediaStream.cpp`: set `NUM_IMAGE_COLS 1920`, `NUM_IMAGE_ROWS 1080`.
5. `vcam/source/FrameGenerator.h/.cpp`: add members `int _index = 1; bc::vcam::Reader _shared; std::vector<uint8_t> _scratch, _last; bool _sharedReady = false;` and a method `void SetIndex(int i)`. In `Generate`, **replace** the drawing block (the whole `if (_renderTarget && _textFormat && _dwrite && _whiteBrush) { … }` that clears to blue, draws HSL blocks, ellipses and the text) with:
```cpp
	// BreadCamera: copy the engine's newest frame instead of drawing (spec §4.3).
	if (!_sharedReady) _sharedReady = _shared.create(_index);
	_scratch.resize(bc::vcam::kFrameBytes);
	if (_last.size() != bc::vcam::kFrameBytes) _last.assign(bc::vcam::kFrameBytes, 0x30);  // slate: dark grey
	if (_sharedReady && _shared.readLatest(_scratch.data(), std::chrono::milliseconds(1000))) {
		_last.swap(_scratch);
	} else if (!_sharedReady || _frame % 30 == 0) {
		std::fill(_last.begin(), _last.end(), (uint8_t)0x30);  // no engine frames for 1 s → slate
		for (size_t i = 3; i < _last.size(); i += 4) _last[i] = 0xFF;
	}
	if (HasD3DManager())
	{
		wil::com_ptr_nothrow<ID3D11Device> device;
		_texture->GetDevice(&device);
		wil::com_ptr_nothrow<ID3D11DeviceContext> ctx;
		device->GetImmediateContext(&ctx);
		ctx->UpdateSubresource(_texture.get(), 0, nullptr, _last.data(), _width * 4, 0);
	}
	else
	{
		wil::com_ptr_nothrow<IWICBitmapLock> wlock;
		if (SUCCEEDED(_bitmap->Lock(nullptr, WICBitmapLockWrite, &wlock)))
		{
			UINT stride = 0, size = 0;
			WICInProcPointer ptr = nullptr;
			wlock->GetStride(&stride);
			wlock->GetDataPointer(&size, &ptr);
			for (UINT y = 0; y < _height; y++) memcpy(ptr + (size_t)y * stride, _last.data() + (size_t)y * _width * 4, (size_t)_width * 4);
		}
	}
```
   Keep everything after the drawing block (sample building and NV12 conversion) unchanged. `#include "core/vcam_shared.h"`, `<chrono>`, `<vector>`.
6. Build: `scripts/build.ps1` produces `build/release/vcam/breadcamera-vcam.dll`. Check with `dumpbin /exports` that `DllGetClassObject`, `DllCanUnloadNow`, `DllRegisterServer`, `DllUnregisterServer` are exported (paste into the report).

- [ ] **Step 3: The engine output**

`src/io/vcam_output.h`:
```cpp
#pragma once
#include <string>
#include <vector>

#include "core/vcam_shared.h"
#include "io/queued_output.h"

struct IMFVirtualCamera;
struct SwsContext;

namespace bc {
// Windows 11 virtual camera N (1..4): registers it with MFCreateVirtualCamera and writes frames
// into the shared section the media source creates (spec §4.3). No alpha.
class VirtualCameraOutput final : public QueuedOutput {
 public:
  VirtualCameraOutput(int index, std::string friendlyName) : index_(index), name_(std::move(friendlyName)) {}
  ~VirtualCameraOutput() override { stop(); }
  bool needsCpu() const override { return true; }
  static bool installed();  // Windows 11 + HKLM registration of the DLL

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "vcam-failed"; }

 private:
  int index_;
  std::string name_;
  IMFVirtualCamera* cam_ = nullptr;
  vcam::Writer writer_;
  uint64_t frames_ = 0;
  SwsContext* sws_ = nullptr;
  std::vector<uint8_t> scaled_;
};
}  // namespace bc
```

`src/io/vcam_output.cpp`:
```cpp
#include "io/vcam_output.h"

#include <mfapi.h>
#include <mfvirtualcamera.h>

extern "C" {
#include <libswscale/swscale.h>
}

#include <stdexcept>

#include "core/utf8.h"

namespace bc {
bool VirtualCameraOutput::installed() {
  OSVERSIONINFOEXW v{sizeof(v)};
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  if (!fn || fn(&v) != 0 || v.dwBuildNumber < 22000) return false;
  HKEY key = nullptr;
  const std::wstring path = std::wstring(L"Software\\Classes\\CLSID\\") + vcam::kClsids[0] + L"\\InprocServer32";
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
  RegCloseKey(key);
  return true;
}

void VirtualCameraOutput::open() {
  if (index_ < 1 || index_ > 4) throw std::runtime_error("virtual camera index must be 1 to 4");
  if (!installed()) throw std::runtime_error("the BreadCamera virtual camera is not installed (Windows 11 and the installer are needed)");
  MFStartup(MF_VERSION);
  const std::wstring friendly = toWide(name_);
  const HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                                           MFVirtualCameraAccess_CurrentUser, friendly.c_str(), vcam::kClsids[index_ - 1],
                                           nullptr, 0, &cam_);
  if (FAILED(hr) || FAILED(cam_->Start(nullptr))) throw std::runtime_error("Windows refused to start the virtual camera");
}

void VirtualCameraOutput::deliver(const OutFrame& f) {
  if (!f.bgra || f.bgra->bgra.empty()) return;
  if (++frames_ % 2) return;  // ~30 fps from a 60 fps tick
  if (!writer_.isOpen() && (frames_ % 30 == 0 || frames_ == 2)) writer_.open(index_);
  if (!writer_.isOpen()) return;  // no app has the camera open: nothing to do
  const uint8_t* src = f.bgra->bgra.data();
  if (f.bgra->width != vcam::kWidth || f.bgra->height != vcam::kHeight) {
    scaled_.resize(vcam::kFrameBytes);
    sws_ = sws_getCachedContext(sws_, static_cast<int>(f.bgra->width), static_cast<int>(f.bgra->height), AV_PIX_FMT_BGRA,
                                vcam::kWidth, vcam::kHeight, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    const uint8_t* s[1] = {src};
    const int ss[1] = {static_cast<int>(f.bgra->width * 4)};
    uint8_t* d[1] = {scaled_.data()};
    const int ds[1] = {static_cast<int>(vcam::kWidth * 4)};
    sws_scale(sws_, s, ss, 0, static_cast<int>(f.bgra->height), d, ds);
    src = scaled_.data();
  }
  writer_.write(src);
}

void VirtualCameraOutput::close() noexcept {
  writer_.close();
  if (cam_) {
    cam_->Shutdown();
    cam_->Release();
    cam_ = nullptr;
  }
  sws_freeContext(sws_);
  sws_ = nullptr;
}
}  // namespace bc
```
CMake: inside `if(BC_WITH_VCAM)` add `target_sources(bc_io PRIVATE src/io/vcam_output.cpp)`, `target_link_libraries(bc_io PUBLIC mfsensorgroup)` (the import library for `MFCreateVirtualCamera`), `target_compile_definitions(bc_io PUBLIC BC_WITH_VCAM=1)`; else `BC_WITH_VCAM=0`. If the linker cannot find `mfsensorgroup.lib`, ledger the Windows SDK version and load `MFCreateVirtualCamera` with `GetProcAddress` from `mfsensorgroup.dll` instead.

- [ ] **Step 4: Loopback test (needs the DLL registered)**

`requires`: `VcamInstalled` → missing unless `BC_WITH_VCAM` and `bc::VirtualCameraOutput::installed()`; reason "virtual camera not registered (run regsvr32 breadcamera-vcam.dll as admin on Windows 11)".

`tests/vcam_output_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_VCAM
#include <thread>

#include "gpu/device.h"
#include "io/mf_capture_input.h"
#include "io/vcam_output.h"

using namespace std::chrono_literals;

namespace {
struct Grab : bc::InputSink {
  std::atomic<int> video{0};
  void onVideo(bc::VideoFrameIn) override { ++video; }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
};
}  // namespace

TEST_CASE("VirtualCameraOutput: Windows sees the camera and a capture app reads it" BC_REQUIRES(bc::test::Req::VcamInstalled)) {
  const std::string name = "BreadCamera Test " + std::to_string(GetCurrentProcessId());
  bc::VirtualCameraOutput out(1, name);
  out.start();
  std::atomic<bool> run{true};
  std::thread feeder([&] {
    auto img = std::make_shared<bc::CpuImage>();
    img->width = 1920;
    img->height = 1080;
    img->bgra.assign(img->width * img->height * 4, 160);
    for (uint64_t t = 0; run; ++t) {
      bc::OutFrame f;
      f.tick = t;
      f.rate = {60, 1};
      f.bgra = img;
      out.submit(std::move(f));
      std::this_thread::sleep_for(16ms);
    }
  });
  std::this_thread::sleep_for(2s);
  bool listed = false;
  for (auto& d : bc::listCaptureDevices()) listed = listed || d.name == name;
  CHECK(listed);
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::MfCaptureInput cap(*gpu, pool, name);
  Grab grab;
  cap.start(grab);
  std::this_thread::sleep_for(4s);
  cap.stop();
  run = false;
  feeder.join();
  out.stop();
  CHECK(grab.video > 30);
}
#else
TEST_CASE("VirtualCameraOutput: Windows sees the camera and a capture app reads it" BC_REQUIRES(bc::test::Req::VcamInstalled)) {}
#endif
```

Run `scripts/test.ps1 -Filter "vcam*,VirtualCameraOutput*"` — shared-memory cases pass; the loopback case is skipped unless the owner registered the DLL (then it must pass).

- [ ] **Step 5: Commit**

```bash
git add src vcam tests CMakeLists.txt
git commit -m "feat: Windows 11 virtual camera (vendored VCamSample media source + shared-memory frames)"
```

---

### Task 4: Factory, router and UI for DeckLink, AJA and the virtual camera

**Files:**
- Modify: `src/engine/adapter_factory.cpp`, `src/engine/command_router.h/.cpp` (validators, `listDeckLinkDevices`, `listAjaDevices`, vcam index uniqueness), `src/engine/engine_main.cpp`, `tests/command_router_test.cpp`, `ui/src/dialogs/AddCameraDialog.tsx`, `ui/src/dialogs/AddOutputDialog.tsx`
- Test: `tests/command_router_test.cpp` (cases), `ui/src/dialogs/AddOutputDialog.test.tsx` (case)

**Interfaces:**
- Produces: input kinds `decklink {device: 0..15}`, `aja {device: 0..7, channel: 1..8}`; output kinds `decklink {device: 0..15}` (uses `OutputConfig::keyed` for external keying), `aja {device: 0..7, channel: 1..8}`, `vcam {index: 1..4, name?: 1..80}`; a show may hold each vcam index **once** (router: `invalid` "virtual camera N is already used by camera X"); commands `listDeckLinkDevices` → `[{index, name, hasInput, hasOutput, externalKeying}]`, `listAjaDevices` → `[{index, name}]`; `Effects` gains both.

Factory (`makeInput`/`makeOutput`): `decklink` → `DeckLinkInput(gpu, pool, device)` / `DeckLinkOutput(device, showRate, cfg.keyed)`; `aja` → `AjaInput(gpu, pool, device, channel)` / `AjaOutput(device, channel, showRate)`; `vcam` → `VirtualCameraOutput(index, name or "BreadCamera - <camera name>")`; each behind its `#if BC_WITH_*` with the "… support is not installed in this build" error otherwise. The factory needs the show rate: add `Rate showRate` to `DefaultAdapterFactory` via `void setShowRate(Rate)` called by the engine in `apply()` before cameras are (re)built (and in the test fakes: a no-op default `virtual void setShowRate(Rate) {}` on `AdapterFactory`).

UI copy (English): kinds "DeckLink (SDI/HDMI)", "AJA (experimental)", "Windows virtual camera"; "Card", "Channel", "Virtual camera number"; AJA badge "Experimental — not verified on hardware"; DeckLink keyed switch "Key + fill out (2 SDI ports)", disabled with the hint "This card has no external keyer" when `externalKeying` is false; fixed-format note "SDI and the virtual camera use a fixed format: 1080p at the show rate (SDI), 1080p30 (virtual camera)."

- [ ] **Step 1: Router tests**

Append to `tests/command_router_test.cpp` (and add `listDeckLinkDevices` / `listAjaDevices` to `FakeFx` returning one fake device each):
```cpp
TEST_CASE("CommandRouter: card and virtual camera kinds") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto addCam = [&](json input) { return call(r, {{"cmd", "addCamera"}, {"args", {{"name", "C"}, {"input", input}}}}); };
  CHECK(addCam({{"kind", "decklink"}, {"params", {{"device", 0}}}})["ok"] == true);
  CHECK(addCam({{"kind", "aja"}, {"params", {{"device", 0}, {"channel", 9}}}})["error"]["code"] == "invalid");
  auto a = addCam({{"kind", "aja"}, {"params", {{"device", 0}, {"channel", 1}}}})["result"]["id"];
  auto b = addCam(kSrt)["result"]["id"];
  auto out = [&](json cam, json o) { return call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", o}}}}); };
  CHECK(out(a, {{"kind", "vcam"}, {"params", {{"index", 1}}}})["ok"] == true);
  auto dup = out(b, {{"kind", "vcam"}, {"params", {{"index", 1}}}});
  CHECK(dup["error"]["code"] == "invalid");
  CHECK(dup["error"]["message"].get<std::string>().find("1") != std::string::npos);
  CHECK(out(b, {{"kind", "vcam"}, {"params", {{"index", 5}}}})["error"]["code"] == "invalid");
  CHECK(out(b, {{"kind", "decklink"}, {"keyed", true}, {"params", {{"device", 0}}}})["ok"] == true);
  CHECK(call(r, {{"cmd", "listDeckLinkDevices"}})["result"].size() == 1);
  CHECK(call(r, {{"cmd", "listAjaDevices"}})["result"].size() == 1);
}
```
The vcam uniqueness check lives in `addOutput`/`updateOutput` (it needs the whole show), not in `validateOutput`.

- [ ] **Step 2: Implement router, factory, engine wiring**

In `command_router.cpp`, next to `strParam`:
```cpp
int intParam(const json& p, const char* key, int lo, int hi, const char* what) {
  if (!p.contains(key) || !p[key].is_number_integer()) throw std::invalid_argument(std::string(what) + " is required (a whole number)");
  const int v = p[key].get<int>();
  if (v < lo || v > hi) throw std::invalid_argument(std::string(what) + " must be " + std::to_string(lo) + " to " + std::to_string(hi));
  return v;
}
```
`validateInput`, before the final `else`:
```cpp
  } else if (in.kind == "decklink") {
    intParam(in.params, "device", 0, 15, "DeckLink card");
  } else if (in.kind == "aja") {
    intParam(in.params, "device", 0, 7, "AJA card");
    intParam(in.params, "channel", 1, 8, "AJA channel");
```
`validateOutput`, before the final `throw`:
```cpp
  if (o.kind == "decklink") {
    intParam(o.params, "device", 0, 15, "DeckLink card");
    return;
  }
  if (o.kind == "aja") {
    intParam(o.params, "device", 0, 7, "AJA card");
    intParam(o.params, "channel", 1, 8, "AJA channel");
    return;
  }
  if (o.kind == "vcam") {
    intParam(o.params, "index", 1, 4, "virtual camera number");
    if (o.params.contains("name")) strParam(o.params, "name", 1, 80, "virtual camera name");
    return;
  }
```
A helper used by `addOutput` and `updateOutput` right after `checkOutput(...)`:
```cpp
void checkVcamFree(const Show& show, const OutputConfig& o) {
  if (o.kind != "vcam") return;
  const int n = o.params["index"].get<int>();
  for (auto& c : show.cameras)
    for (auto& other : c.outputs)
      if (other.id != o.id && other.kind == "vcam" && other.params.value("index", 0) == n) {
        throw CmdError("invalid", "virtual camera " + std::to_string(n) + " is already used by camera " + c.name);
      }
}
```
(in `addOutput` call it with `next` **before** `c.outputs.push_back(out)`; in `updateOutput` after the field updates).

Commands next to `listMonitors`:
```cpp
  if (cmd == "listDeckLinkDevices") return fx_.listDeckLinkDevices();
  if (cmd == "listAjaDevices") return fx_.listAjaDevices();
```
`Effects` gains `virtual nlohmann::json listDeckLinkDevices() = 0; virtual nlohmann::json listAjaDevices() = 0;`; `engine_main` `Fx`:
```cpp
    json listDeckLinkDevices() override {
      json out = json::array();
#if BC_WITH_DECKLINK
      for (auto& d : bc::listDeckLinkDevices()) {
        out.push_back({{"index", d.index}, {"name", d.name}, {"hasInput", d.hasInput}, {"hasOutput", d.hasOutput}, {"externalKeying", d.externalKeying}});
      }
#endif
      return out;
    }
    json listAjaDevices() override {
      json out = json::array();
#if BC_WITH_AJA
      for (auto& d : bc::listAjaDevices()) out.push_back({{"index", d.index}, {"name", d.name}});
#endif
      return out;
    }
```
`AdapterFactory` gains `virtual void setShowRate(Rate) {}`; `DefaultAdapterFactory` overrides it to store `rate_`; `Engine::apply` calls `factory_.setShowRate(show.settings.rate);` as its first line. Factory branches:
```cpp
  // makeInput
  if (cfg.kind == "decklink") {
#if BC_WITH_DECKLINK
    return std::make_unique<DeckLinkInput>(gpu_, pool_, cfg.params.at("device").get<int>());
#else
    throw std::invalid_argument("DeckLink support is not installed in this build");
#endif
  }
  if (cfg.kind == "aja") {
#if BC_WITH_AJA
    return std::make_unique<AjaInput>(gpu_, pool_, cfg.params.at("device").get<int>(), cfg.params.at("channel").get<int>());
#else
    throw std::invalid_argument("AJA support is not installed in this build");
#endif
  }
  // makeOutput
  if (cfg.kind == "decklink") {
#if BC_WITH_DECKLINK
    return std::make_unique<DeckLinkOutput>(cfg.params.at("device").get<int>(), rate_, cfg.keyed);
#else
    throw std::invalid_argument("DeckLink support is not installed in this build");
#endif
  }
  if (cfg.kind == "aja") {
#if BC_WITH_AJA
    return std::make_unique<AjaOutput>(cfg.params.at("device").get<int>(), cfg.params.at("channel").get<int>(), rate_);
#else
    throw std::invalid_argument("AJA support is not installed in this build");
#endif
  }
  if (cfg.kind == "vcam") {
#if BC_WITH_VCAM
    return std::make_unique<VirtualCameraOutput>(cfg.params.at("index").get<int>(), cfg.params.value("name", "BreadCamera - " + cam.name));
#else
    throw std::invalid_argument("the virtual camera is not available in this build");
#endif
  }
```
with the matching guarded `#include`s.

Run `scripts/test.ps1 -Filter "CommandRouter*,Engine*"` — all pass.

- [ ] **Step 3: Fixed formats in the router**

In `validateOutput` (`src/engine/command_router.cpp`), for `decklink`, `aja` and `vcam`, after the kind's own checks:
```cpp
    if (o.width || o.height || o.halfRate) throw std::invalid_argument("this output type has a fixed format (spec §4.3)");
```
and append to `tests/command_router_test.cpp`:
```cpp
TEST_CASE("CommandRouter: SDI and virtual camera outputs refuse a format override") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "C"}, {"input", kSrt}}}})["result"]["id"];
  auto add = call(r, {{"cmd", "addOutput"}, {"args", {{"cameraId", cam}, {"output", {{"kind", "vcam"}, {"params", {{"index", 1}}}, {"format", {{"width", 1280}, {"height", 720}, {"halfRate", false}}}}}}}});
  CHECK(add["error"]["code"] == "invalid");
}
```

- [ ] **Step 4: UI (M1's native dialogs)**

`ui/src/dialogs/AddCameraDialog.tsx` — the Input type select gains `decklink` "DeckLink (SDI/HDMI)" and `aja` "AJA (experimental)". DeckLink: `<Prop label="Card">` select from `listDeckLinkDevices` (only `hasInput`). AJA: Card (from `listAjaDevices`) and Channel (1–8) selects, with `<ErrorBlock severity="warning" message="Experimental — not verified on hardware" />` under them. Params as the router expects.

`ui/src/dialogs/AddOutputDialog.tsx` — Output type gains `decklink` "DeckLink SDI", `aja` "AJA SDI (experimental)", `vcam` "Windows virtual camera":
- DeckLink: Card (only `hasOutput`); "Key + fill out (2 SDI ports)" switch, enabled only when the card's `externalKeying` is true, otherwise disabled with the hint "This card has no external keyer"; sends `keyed` at the output level.
- AJA: Card + Channel + the warning block.
- vcam: "Virtual camera number" select 1–4, defaulting to the lowest number no `vcam` output in the show uses (`useEngine(client).show`).
- For `decklink`, `aja`, `vcam` (and `fullscreen`) the Resolution and Frame rate rows are hidden and replaced by the fixed-format note; the dialog sends `format: { width: 0, height: 0, halfRate: false }`.

Append to `ui/src/dialogs/AddOutputDialog.test.tsx`:
```tsx
test('a virtual camera output picks the first free number and has no format choice', () => {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({
    type: 'snapshot',
    state: {},
    show: { cameras: [{ id: 'z', name: 'Z', input: { kind: 'srt', params: {} }, outputs: [{ id: 'v', kind: 'vcam', keyed: false, enabled: true, params: { index: 1 } }] }] },
  });
  render(<AddOutputDialog client={c} camera={{ id: 'a', name: 'A' }} n={1} onClose={() => {}} />);
  fireEvent.change(screen.getByLabelText('Output type'), { target: { value: 'vcam' } });
  expect(screen.queryByLabelText('Resolution')).toBeNull();
  fireEvent.click(screen.getByRole('button', { name: 'Add output' }));
  const add = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'addOutput');
  expect(add.args.output).toMatchObject({ kind: 'vcam', params: { index: 2 }, format: { width: 0, height: 0, halfRate: false } });
});
```
Run `npm test` and `npm run build` in `ui/`.

- [ ] **Step 5: Commit**

```bash
git add src tests ui/src
git commit -m "feat: DeckLink, AJA and virtual camera kinds in the factory, router and UI dialogs"
```

---

### Task 5: M4b verification and follow-ups

**Files:**
- Modify: `docs/superpowers/follow-ups.md`
- Create: `docs/superpowers/manual-checks/m4b-hardware.md`

- [ ] **Step 1: Build matrix evidence**

Run and paste the configure summary lines (`BC_WITH_NDI/OMT/MAXINE/DECKLINK/AJA/VCAM=…`) and the test totals (passed / skipped with reasons) into the report. The build must succeed with whatever subset this PC has.

- [ ] **Step 2: Owner's hardware checklist**

`docs/superpowers/manual-checks/m4b-hardware.md` — a numbered checklist for a person with the hardware (this is a human gate; do not self-certify it):
```markdown
# M4b hardware checks (owner)

## Virtual camera (Windows 11)
1. As administrator: `regsvr32 <build>\vcam\breadcamera-vcam.dll` → "succeeded".
2. Add an SRT camera, add output "Windows virtual camera" #1.
3. Open the Windows Camera app → pick "BreadCamera - <camera>" → live picture within 2 s.
4. Open it in OBS (Video Capture Device) at the same time as the Camera app → both show picture.
5. Stop the input → after 1 s the virtual camera shows the dark-grey slate, never black.

## DeckLink
1. Card with SDI in: add a DeckLink camera → live; change the source from 1080i50 to 1080p60 → picture returns without touching BreadCamera.
2. SDI out: add a DeckLink output → picture on a monitor.
3. Key + fill: on a card with an external keyer, a keyed output on a camera with Background mode = "Keyed (fill + key)" → fill on SDI A, key on SDI B; a switcher's DSK keys the person cleanly.

## AJA (experimental)
1. Input on channel 1 → live picture and audio meters in the switcher fed from BreadCamera.
2. Output on channel 1 → picture (fill only).
```

- [ ] **Step 3: Follow-ups**

Append to `docs/superpowers/follow-ups.md`:
```markdown
## M4b
- DeckLink, AJA and the virtual camera are **untested on hardware**; the owner's checklist is `docs/superpowers/manual-checks/m4b-hardware.md`.
- AJA output is fill only (spec §4.3 lists key+fill) — add a second channel for the key (`src/io/aja_output.cpp`).
- DeckLink output uses `DisplayVideoFrameSync`; scheduled playback with preroll would remove jitter (`src/io/decklink_output.cpp`).
- UYVY conversion creates a swscale context per frame (`src/io/uyvy.cpp`).
- The virtual camera is 1920×1080 at 30 fps, at most 4 cameras (`src/core/vcam_shared.h`).
- The vcam DLL needs HKLM registration (admin) — done by the installer (M5).
```

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers
git commit -m "docs: M4b hardware checklist and follow-ups"
```
