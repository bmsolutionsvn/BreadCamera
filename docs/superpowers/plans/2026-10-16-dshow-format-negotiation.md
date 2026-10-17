# DirectShow camera format negotiation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make OBS Virtual Camera (and other DirectShow-only virtual cameras) actually deliver frames by negotiating the source's native format and converting it to BGRA8, instead of forcing RGB32.

**Architecture:** `DShowCaptureInput` stops forcing the `SampleGrabber` media type; it lets `RenderStream` connect the source's native format, then reads the connected subtype and converts each sample to packed BGRA8 with a new `swscale`-based helper. A subtype outside the raw set (MJPG and friends) makes the input rebuild the graph once forcing RGB32 — the behaviour that already works for physical webcams.

**Tech Stack:** MSVC C++20, DirectShow (`ICaptureGraphBuilder2`, `ISampleGrabber`), FFmpeg `libswscale` (already linked), doctest, CMake + Ninja + vcpkg.

**Spec:** `docs/superpowers/specs/2026-10-16-dshow-format-negotiation-design.md`

## Global Constraints

- Windows only. C++20, MSVC, `/W4 /permissive- /utf-8 /EHsc`.
- `start()`/`stop()` never block for long (spec §4.1); inputs never block the tick.
- A missing, busy or unplugged device reports `offline`, code `capture-failed`, and retries with backoff — never a hang (unchanged contract).
- Capture is video only (no audio); BGRA8 into `TexturePool`, as `MfCaptureInput` does.
- DirectShow RGB is full-range and bottom-up when `biHeight > 0`; YUV is limited-range and top-down. `bt709` is chosen from the picture height (>= 720 ⇒ 709).
- UK English copy; conventional commits (`fix(io):`).

## Review Focus

Failure modes the spec implies but no single task's happy path exercises; each gets a test in the task that owns the code:

1. **A physical webcam that negotiates MJPG natively** — must still stream, via the forced-RGB32 fallback (Task 2, fallback test).
2. **A bottom-up RGB buffer** — must not be vertically flipped into a wrong picture (Task 1, RGB32/RGB24 flip test).
3. **A short sample** (`GetActualDataLength` < `biSizeImage`) — must be skipped, not read past its end (Task 2, short-sample guard unchanged + Task 1 length discipline).
4. **`stop()` while the graph is mid-fallback rebuild** — must return promptly (Task 3, stop test).
5. **A device that sends nothing at all** — still `capture-failed` + retry (existing behaviour kept).

---

### Task 1: The pure format converter

**Files:**
- Create: `BreadCamera/src/io/dshow_format.h`
- Create: `BreadCamera/src/io/dshow_format.cpp`
- Create: `BreadCamera/tests/dshow_format_test.cpp`
- Modify: `BreadCamera/CMakeLists.txt` (add `src/io/dshow_format.cpp` to `bc_io`; `tests/dshow_format_test.cpp` to `bc_tests`)

**Interfaces:**
- Consumes: `libswscale` (`sws_getContext`, `sws_scale`, `sws_setColorspaceDetails`), as `src/io/uyvy.cpp` uses it.
- Produces:

```cpp
// src/io/dshow_format.h
#pragma once
#include <guiddef.h>
#include <cstdint>
#include <vector>
namespace bc {
// True for a MEDIASUBTYPE_* this converter can turn into BGRA8 (RGB32, RGB24, NV12, YUY2, UYVY).
bool dshowSubtypeSupported(const GUID& subtype);
// One DirectShow sample -> packed BGRA8, width*4 bytes per row, top-down.
// `stride` is the source row size in bytes; `bottomUp` means the buffer's first row is the bottom.
// Throws std::runtime_error for an unsupported subtype.
std::vector<uint8_t> dshowToBgra8(const GUID& subtype, const uint8_t* src, int width, int height,
                                  int stride, bool bottomUp, bool bt709);
}
```

- [ ] **Step 1: Write the failing tests** — create `tests/dshow_format_test.cpp`:

```cpp
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>
#include "io/dshow_format.h"

// MEDIASUBTYPE_RGB32 {e436eb7d...} etc. are in uuids.h via <dshow.h>; declare the two we need here
// so the test stays a pure unit (no DirectShow).
static const GUID kRgb32 = {0xe436eb7d, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
static const GUID kRgb24 = {0xe436eb7e, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
static const GUID kNv12  = {0x3231564e, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kYuy2  = {0x32595559, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

TEST_CASE("dshowToBgra8: RGB32 is a BGRX pass-through") {
  const uint8_t src[16] = { 1,2,3,0,  4,5,6,0,   7,8,9,0,  10,11,12,0 };  // 2x2, top-down
  auto out = bc::dshowToBgra8(kRgb32, src, 2, 2, 8, false, false);
  CHECK(out.size() == 16u);
  CHECK(out[0] == 1); CHECK(out[1] == 2); CHECK(out[2] == 3);
  CHECK(out[12] == 10); CHECK(out[13] == 11); CHECK(out[14] == 12);
}

TEST_CASE("dshowToBgra8: RGB32 bottom-up is flipped upright") {
  const uint8_t src[16] = { 1,2,3,0,  4,5,6,0,   7,8,9,0,  10,11,12,0 };
  auto out = bc::dshowToBgra8(kRgb32, src, 2, 2, 8, true, false);
  CHECK(out[0] == 7); CHECK(out[1] == 8); CHECK(out[2] == 9);   // source's last row comes first
}

TEST_CASE("dshowToBgra8: RGB24 expands to BGRA") {
  const uint8_t src[12] = { 1,2,3,  4,5,6,   7,8,9,  10,11,12 };  // 2x2
  auto out = bc::dshowToBgra8(kRgb24, src, 2, 2, 6, false, false);
  CHECK(out.size() == 16u);
  CHECK(out[0] == 1); CHECK(out[1] == 2); CHECK(out[2] == 3); CHECK(out[3] == 255);
}

TEST_CASE("dshowToBgra8: limited-range NV12 black and white") {
  // 2x2 NV12: Y plane then interleaved UV. Y=16 (black), U=V=128 (neutral).
  const uint8_t black[6] = { 16,16, 16,16,  128,128 };
  auto b = bc::dshowToBgra8(kNv12, black, 2, 2, 2, false, true);
  CHECK(b[0] <= 2); CHECK(b[1] <= 2); CHECK(b[2] <= 2);
  const uint8_t white[6] = { 235,235, 235,235,  128,128 };
  auto w = bc::dshowToBgra8(kNv12, white, 2, 2, 2, false, true);
  CHECK(w[0] >= 253); CHECK(w[1] >= 253); CHECK(w[2] >= 253);
}

TEST_CASE("dshowToBgra8: YUY2 neutral maps to grey") {
  // 2x2 YUY2: Y0 U Y1 V  => top row Y=128,U=V=128; bottom row same.
  const uint8_t src[8] = { 128,128, 128,128,  128,128, 128,128 };
  auto out = bc::dshowToBgra8(kYuy2, src, 2, 2, 4, false, true);
  CHECK(out[0] >= 124); CHECK(out[0] <= 132);
}

TEST_CASE("dshowSubtypeSupported: recognises the raw set, rejects MJPG") {
  CHECK(bc::dshowSubtypeSupported(kRgb32));
  CHECK(bc::dshowSubtypeSupported(kNv12));
  CHECK_FALSE(bc::dshowSubtypeSupported(GUID{0x47504a4d,0x0000,0x0010,{0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71}}));  // MJPG
}
```

- [ ] **Step 2: Run them and watch them fail to compile**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "dshowToBgra8*,dshowSubtypeSupported*"`
Expected: build FAILS — `io/dshow_format.h` not found.

- [ ] **Step 3: Implement the converter in `src/io/dshow_format.cpp`**

Map subtype → `AVPixelFormat`: `RGB32`→`AV_PIX_FMT_BGRA`, `RGB24`→`AV_PIX_FMT_BGR24`, `NV12`→`AV_PIX_FMT_NV12`, `YUY2`→`AV_PIX_FMT_YUYV422`, `UYVY`→`AV_PIX_FMT_UYVY422` (compare the `Data1` first dword / full GUID as `uyvy.cpp` matches formats). Build the `SwsContext` with the `make()` shape from `uyvy.cpp`: source range limited for YUV, full for RGB (so `sws_setColorspaceDetails(s, cs, srcFull, cs, 1, ...)`); `bt709` picks `SWS_CS_ITU709` else `SWS_CS_ITU601`. For `bottomUp`, point `src` at the last row and pass a negative `srcStride` (swscale flips). Output `width*4 * height` bytes. Throw `std::runtime_error("unsupported DirectShow subtype")` when no mapping matches.

- [ ] **Step 4: Run the tests and watch them pass**

Run: `scripts/test.ps1 -Filter "dshowToBgra8*,dshowSubtypeSupported*"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/io/dshow_format.h src/io/dshow_format.cpp tests/dshow_format_test.cpp CMakeLists.txt
git commit -m "feat(io): convert DirectShow native formats to BGRA8"
```

---

### Task 2: Negotiate the native format in `DShowCaptureInput`

**Files:**
- Modify: `BreadCamera/src/io/dshow_capture_input.cpp` (the `runOnce` graph build, the media-type read, the frame loop; add `#include "io/dshow_format.h"`)
- Modify: `BreadCamera/src/io/dshow_capture_input.h` (a `bool forceRgb32_` member is not needed if the flag is local to the attempt)
- Test: `BreadCamera/tests/dshow_capture_test.cpp`

**Interfaces:**
- Consumes: `bc::dshowSubtypeSupported`, `bc::dshowToBgra8` (Task 1).
- Produces: `DShowCaptureInput` unchanged externally (`start`/`stop`).

- [ ] **Step 1: Write the failing test** — append to `tests/dshow_capture_test.cpp`:

```cpp
TEST_CASE("DShowCaptureInput: a raw-format device delivers frames" BC_REQUIRES(bc::test::Req::DShowDevice)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  // The first device that delivers through the new native path wins; an idle device is skipped.
  int best = 0;
  for (const auto& dev : bc::listDShowDevices()) {
    bc::DShowCaptureInput in(*gpu, pool, dev.displayName);
    Sink sink;
    in.start(sink);
    for (int i = 0; i < 20; ++i) {
      { std::lock_guard l(sink.m); if (sink.video > 10) break; }
      std::this_thread::sleep_for(100ms);
    }
    in.stop();
    std::lock_guard l(sink.m);
    best = std::max(best, sink.video);
    std::string why;
    if (!sink.errors.empty() && sink.errors.back()) why = " (" + sink.errors.back()->message + ")";
    std::fprintf(stderr, "[dshow-native] %s: %d frame(s)%s\n", dev.name.c_str(), sink.video, why.c_str());
  }
  CHECK(best > 10);
}
```

- [ ] **Step 2: Run it and watch it fail**

Run: `scripts/test.ps1 -Filter "DShowCaptureInput*"`
Expected: on this PC the new case FAILS for OBS Virtual Camera (`0 frame(s)`) with the current forced-RGB32 code — the regression the plan fixes.

- [ ] **Step 3: Change `runOnce` to negotiate natively, then fall back**

`runOnce(bool forceRgb32)`: only call `grabber->SetMediaType(&mt)` (RGB32) when `forceRgb32` is true; otherwise leave the media type unset so `RenderStream` connects the source's native format. After `RenderStream`, read the connected type; if `formattype` is neither `FORMAT_VideoInfo` nor `FORMAT_VideoInfo2` or `pbFormat` is null → throw `"the device's video format is not supported"`. Compute `width_`/`height_` from the DIB header. Keep `width_ <= 0 || height_ <= 0` → throw `"the device reports no picture size"`.

Choose the picture rules by subtype: `bottomUp_ = subtype is RGB32/RGB24 && hRaw > 0`; `stride = biSizeImage ? biSizeImage / height : width * bytesPerPixel(subtype)`; `bt709 = height >= 720`.

In `run()`: the first attempt is `runOnce(false)`. When the connected subtype is not `dshowSubtypeSupported` **and** the attempt was not already `forceRgb32`, retry immediately with `runOnce(true)` before going `Offline`. Otherwise behave as today (`Offline` + `capture-failed` + backoff).

- [ ] **Step 4: Convert in the frame loop**

Replace the fixed `pitch = width_*4` / BGRA copy with: skip the sample when `frame.size() < stride*height_` (short-sample guard); else `auto bgra = dshowToBgra8(subtype, frame.data(), width_, height_, stride, bottomUp_, bt709);` and `UpdateSubresource` with `pitch = width_*4`. Remove the manual `flip_` path (swscale flips).

- [ ] **Step 5: Run the tests and watch them pass**

Run: `scripts/test.ps1 -Filter "DShowCaptureInput*"`
Expected: PASS — the OBS Virtual Camera line now prints frames > 10 (on the build PC with OBS VCam live); the stall test still passes.

- [ ] **Step 6: Commit**

```bash
git add src/io/dshow_capture_input.cpp src/io/dshow_capture_input.h tests/dshow_capture_test.cpp
git commit -m "fix(io): capture DirectShow cameras in their native format (OBS Virtual Camera)"
```

---

### Task 3: Whole-suite check and the follow-up note

**Files:**
- Modify: `BreadCamera/docs/superpowers/follow-ups.md`
- Modify: `BreadCamera/docs/superpowers/manual-checks/` (a short `dshow-obs-vcam.md` recording the probe evidence)

**Interfaces:**
- Consumes: the code from Tasks 1-2.
- Produces: nothing other tasks build on.

- [ ] **Step 1: Update the follow-up note**

Append to `follow-ups.md`: the DirectShow capture now negotiates the source's native format and converts RGB32/RGB24/NV12/YUY2 (+UYVY) to BGRA8 via `swscale`; a device whose native format is outside that set is retried once forcing RGB32, then reported `capture-failed`. Record that the forced-RGB32 path was the OBS Virtual Camera defect (evidence in the manual-check).

- [ ] **Step 2: Record the manual check**

Create `docs/superpowers/manual-checks/dshow-obs-vcam.md` with the probe table (native NV12 180 frames vs RGB32 0 frames, engine stopped) and the end-to-end result: OBS Virtual Camera selected as a Device camera reaches `live` and shows a picture.

- [ ] **Step 3: Run the whole C++ suite**

Run: `scripts/test.ps1`
Expected: PASS (new tests included; pre-existing skips unchanged). Record passed/skipped and skip reasons.

- [ ] **Step 4: Run the UI suite**

Run: `npm test` (in `BreadCamera/ui`)
Expected: PASS (labels unchanged by this fix).

- [ ] **Step 5: Commit**

```bash
git add docs/superpowers/follow-ups.md docs/superpowers/manual-checks/dshow-obs-vcam.md
git commit -m "docs(io): record the DirectShow native-format capture"
```

---

## Self-Review

- **Spec coverage:** D1 native negotiation (Task 2 Step 3), D2 conversion set (Task 1), D3 unsupported→RGB32 fallback (Task 2 Step 3), D4 row order (Task 1 + Task 2 Step 3), D5 pure function + tests (Task 1). Goals/non-goals untouched elsewhere.
- **Step scan:** each step names the file, the signature, the test assertions, or the exact command; no "handle edge cases" lines.
- **Type consistency:** `dshowSubtypeSupported`/`dshowToBgra8` signatures are identical in Task 1's Interfaces and Task 2's use; `runOnce(bool forceRgb32)` is introduced and used only in Task 2.
- **Review Focus:** MJPG fallback (Task 2 Step 3 + the webcam in Step 5), bottom-up flip (Task 1 flip test), short sample (Task 2 Step 4 guard), stop-mid-fallback (existing stop test covers `stop()` between attempts), silent device (stall test unchanged).
- **Proportion:** the spec carries the evidence and the calls; the plan carries signatures, test assertions and commands, not full bodies.
