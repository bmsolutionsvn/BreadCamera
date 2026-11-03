# BreadCamera Lean Paths Phase 1: UYVY Readback for SDI and NDI Outputs — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Unkeyed DeckLink, AJA and NDI outputs get the camera's frame as UYVY made on the GPU (2 bytes a pixel read back, no CPU colour conversion, no alpha loop). DeckLink reuses its frames. The CPU converters left over reuse their swscale contexts.

**Architecture:** A compute shader `linear_to_uyvy.hlsl` packs two pixels into one `R8G8B8A8_UNORM` texel (U, Y0, V, Y1), limited range, matrix by frame height. The camera reads it back with a second `ReadbackSlot`, once per tick, only while some output wants UYVY. `Output::wantsUyvy(w, h)` picks the layout per output; `OutFrame::uyvy` carries it. Alpha frames and keyed outputs keep the BGRA path.

**Tech Stack:** C++20, D3D11 compute shaders (fxc at build time, as the other shaders), DeckLink SDK, AJA ntv2, NDI SDK, FFmpeg swscale, doctest.

**Spec:** `BreadCamera/docs/superpowers/specs/2026-11-03-breadcamera-lean-paths-design.md` (§3.1 items 1, 2 except OMT Full, and 6; phase 1)

**Runs after:** the encode-on-demand plan is merged.

## Global Constraints

- UYVY rules (today's `bgraToUyvy`/`uyvyToBgra`): BT.709 at 720 lines and above, BT.601 below, limited range (Y 16–235, C 16–240), chroma of a pair averaged.
- Gamma: the same `pow(saturate(rgb), 1/2.4)` encode `linear_to_bgra8.hlsl` uses, before the matrix.
- An output gets UYVY only when `wantsUyvy(width, height)` is true **and** the tick's frame has no alpha; otherwise it gets BGRA as today.
- DeckLink and AJA run at a fixed 1920×1080: they want UYVY only for a 1920×1080 frame, so no scaling or matrix change happens on the UYVY path. Keyed DeckLink never wants UYVY.
- An odd frame width cannot pack into UYVY: those ticks use BGRA.
- Never push. Do not touch `F:\Coding\BreadMedia\BreadReplay`.

## Review Focus

1. **Colour on SDI:** solid colours through the UYVY shader match today's BGRA → `bgraToUyvy` within ±3 per byte, at 1080 (BT.709) and 576 lines (BT.601). Test in Task 1.
2. **A frame that gains alpha mid-show** (a keyer turned on): outputs fall back to BGRA on that tick; no output gets a frame with neither picture. Test in Task 2.
3. **Mixed outputs on one camera** (NDI UYVY + OMT HX GPU + a keyed DeckLink BGRA): each gets its own layout, and each readback runs once per tick. Test in Task 2.
4. **A resolution change:** the UYVY readback never publishes the old size. Test in Task 2.
5. **DeckLink frame reuse:** a reused frame must not be one the card still holds. Task 3 checks the SDK rule before relying on it.

---

### Task 1: The UYVY shader and `Converter::toUyvy`

**Files:**
- Create: `src/gpu/shaders/linear_to_uyvy.hlsl`
- Modify: `CMakeLists.txt` (add it to the shader list, next to `linear_to_bgra8`), `src/gpu/converter.h`, `src/gpu/converter.cpp`
- Test: `tests/gpu_test.cpp`

**Interfaces:**
- Produces:
  - `TextureRef Converter::toUyvy(const GpuTexture& linear);` — an `R8G8B8A8_UNORM` texture `width / 2 × height` holding UYVY bytes (texel = U, Y0, V, Y1); null when `linear.width` is odd. Matrix: `height >= 720` → BT.709, else BT.601. Pass the RGB→YUV rows through the existing `Params` rows (add an `rgbToYuvMatrix(height)` helper next to `yuvToRgbMatrix`, limited range).
  - `CpuImage` gains `enum class CpuLayout { Bgra, Uyvy }` and `CpuLayout layout = CpuLayout::Bgra;`. For `Uyvy`, `bgra` holds UYVY rows of `width * 2` bytes and `width` is the picture's width. Say so in the struct's comment.

- [ ] **Step 1: Failing tests**
  - `"Converter: toUyvy matches bgraToUyvy at 1080 lines"` (`HardwareGpu`): a 1920×1080 solid BGRA (r, g, b) through `uploadBgra8` → `toLinear(BGRA8)` → `toUyvy` → `readback` → each of U, Y0, V, Y1 within ±3 of `bgraToUyvy` (bt709 = true) on the same BGRA. Colours: black, white, (230, 40, 20), (20, 200, 60), mid-grey (128, 128, 128).
  - `"Converter: toUyvy uses BT.601 below 720 lines"`: the same at 720×576 with `bt709 = false`.
  - `"Converter: toUyvy refuses an odd width"`: 641×360 → null.
- [ ] **Step 2: Run** `scripts/build.ps1 -SkipUi`, then `./build/release/bc_tests.exe "--test-case=Converter: toUyvy*"` → build error.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the new tests, then the GPU tests (`--test-case=Converter*,YuvMatrix*`) → green.
- [ ] **Step 5: Commit** on `bcam-lean-uyvy-outputs`: `feat(gpu): UYVY made on the GPU`.

### Task 2: The camera reads back UYVY for outputs that want it

**Files:**
- Modify: `src/io/output.h`, `src/engine/camera.h`, `src/engine/camera.cpp` (the per-tick readback block and the output loop, `git grep -n "anyCpu" src/engine/camera.cpp`)
- Modify: `tests/support/fake_output.h`
- Test: `tests/engine_test.cpp`

**Interfaces:**
- Consumes: `Converter::toUyvy`, `CpuLayout` (Task 1).
- Produces:
  - `Output`: `virtual bool wantsUyvy(uint32_t width, uint32_t height) const { return false; }` — only meaningful while `needsCpu()` is true.
  - `OutFrame`: `std::shared_ptr<const CpuImage> uyvy;  // set for an output that wantsUyvy, on ticks without alpha`
  - Camera, per tick: an output takes UYVY when `needsCpu() && wantsUyvy(frame w, h) && !hasAlpha && toUyvy` succeeded (even width). The BGRA readback runs only if some CPU output does not take UYVY (or `previewWanted`). The UYVY readback is a second slot in `ReadbackState` with the same tryFinish-before-begin order and the same "drop a completed image of the old size" rule. The output gets `uyvy` set and `bgra` null when it takes UYVY.
  - `FakeOutput`: a constructor flag for `wantsUyvy` and a record of whether each delivery carried `uyvy`, `bgra`, or both.
  - `Camera::readbackSlotCount()` counts both slots (BGRA, UYVY).

- [ ] **Step 1: Failing tests** (`engine_test`, fake inputs and outputs as the neighbouring tests use):
  - one camera, one UYVY-wanting `FakeOutput`, an opaque 1920×1080 input → after a few ticks, deliveries carry `uyvy` (layout `Uyvy`, width 1920, `bgra.size() == 1920 * 1080 * 2`) and no `bgra`; `readbackSlotCount() == 1`.
  - the same output and a plain CPU `FakeOutput` on one camera → each gets its own layout; `readbackSlotCount() == 2`.
  - an input with alpha → the UYVY-wanting output gets `bgra`, not `uyvy`.
  - an input that changes size (1920×1080 → 1280×720) → no `uyvy` delivery of the old size after the change.
- [ ] **Step 2: Run** `--test-case=*UYVY*` (name the tests so) → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the new tests, then the full verification command (§ Tests in the brief) → green.
- [ ] **Step 5: Commit** `feat(engine): one UYVY readback for outputs that want it`.

### Task 3: DeckLink, AJA and NDI outputs take UYVY; swscale contexts are reused

**Files:**
- Modify: `src/io/decklink_output.h/.cpp`, `src/io/aja_output.h/.cpp`, `src/io/ndi_output.h/.cpp`, `src/io/uyvy.cpp`
- Test: `tests/decklink_test.cpp`, `tests/aja_test.cpp`, `tests/ndi_output_test.cpp`, and a pure test for `uyvy.cpp` (add to an existing CPU test file, e.g. `tests/color_math_test.cpp`)

**Interfaces:**
- Consumes: `OutFrame::uyvy`, `wantsUyvy` (Task 2).
- Produces:
  - `DeckLinkOutput::wantsUyvy(w, h)`: `!keyed_ && w == 1920 && h == 1080`. With `f.uyvy`: frames of `bmdFormat8BitYUV`, row bytes `1920 * 2`, one `memcpy`, no alpha loop. Frames come from a pool of 3 made once in `open()` and reused round-robin. Before relying on reuse after `DisplayVideoFrameSync` returns, check the DeckLink SDK manual's rule for that call and log it as a `Ruling:`; if reuse is not safe there, keep `CreateVideoFrame` per tick for the UYVY path and say so. The BGRA path is unchanged except that its `sws_` is already cached.
  - `AjaOutput::wantsUyvy(w, h)`: `w == 1920 && h == 1080`. With `f.uyvy`: hand `f.uyvy->bgra` to `AUTOCIRCULATE_TRANSFER` directly (no `bgraToUyvy`, no copy). The BGRA path's resize uses a member `SwsContext*` with `sws_getCachedContext`, freed in `close()`.
  - `NdiOutput::wantsUyvy(w, h)`: true. With `f.uyvy`: `NDIlib_FourCC_type_UYVY`, `line_stride_in_bytes = width * 2`.
  - `uyvy.cpp`: `uyvyToBgra` and `bgraToUyvy` keep one `thread_local` context per direction through `sws_getCachedContext` (setting the colourspace details on each call is fine).

- [ ] **Step 1: Failing tests**
  - Pure (no hardware): `bgraToUyvy` and `uyvyToBgra` give the same bytes on two calls with different sizes in a row (a context reused at the wrong size would corrupt the second).
  - `ndi_output_test` (`Ndi`): an `NdiOutput` started and fed `OutFrame`s with `uyvy` set → an NDI receiver (as the existing test does) gets UYVY frames of the right size, centre luma within ±3 of the input.
  - `decklink_test` (`DeckLink`) and `aja_test` (`Aja`): `wantsUyvy(1920, 1080)` true, `(1280, 720)` false, keyed DeckLink false; with hardware, a UYVY frame is accepted (no throw) for 60 ticks. These skip here without the cards; say so in the ledger.
- [ ] **Step 2: Run** them → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the touched tests, then the full verification command → green.
- [ ] **Step 5: Commit, merge into `main`** (`feat(io): SDI and NDI outputs take UYVY from the GPU`, `merge: lean phase 1, UYVY outputs`).
