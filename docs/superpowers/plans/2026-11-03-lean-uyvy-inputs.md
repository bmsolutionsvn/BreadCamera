# BreadCamera Lean Paths Phase 3: UYVY Inputs and the Leak-Safe Upload Everywhere — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** DeckLink, AJA and NDI inputs upload UYVY as it arrives (2 bytes a pixel) and unpack it on the GPU, with no `uyvyToBgra` on the CPU. No per-frame upload anywhere in `src/` uses `UpdateSubresource` (each one keeps a private copy until the GPU work is submitted, and BreadCamera's engine never presents, as the OMT input's 240 MB/s leak showed).

**Architecture:** The OMT fix's `fillPlane` (`src/gpu/nv12_upload.cpp`) becomes the one way to write a texture each frame: DYNAMIC texture, `Map(WRITE_DISCARD)`, row copy. The file becomes `src/gpu/upload.{h,cpp}` with three uploads: NV12 planes (moved), UYVY (an `R8G8B8A8_UNORM` texture `width/2 × height`), and BGRA. A new `PixelLayout::UYVY` and shader `uyvy_to_linear.hlsl` turn UYVY into linear RGBA16F in `Converter::toLinear`.

**Tech Stack:** C++20, D3D11 compute shaders (fxc), DeckLink SDK, AJA ntv2, NDI SDK, doctest.

**Spec:** `BreadCamera/docs/superpowers/specs/2026-11-03-breadcamera-lean-paths-design.md` (§3.1 items 3 and 5, phase 3); backlog item 10.

**Runs after:** lean phase 2 (`2026-11-03-lean-omt-full-uyvy.md`) has ended (merged, or stopped by its rule).

## Global Constraints

- UYVY in: limited range, matrix by height (BT.709 at 720 lines and above, else BT.601), as today's `uyvyToBgra`. Chroma of a pair is used for both of its pixels.
- After this plan, `git grep -n "UpdateSubresource" src/` shows only one-time setup writes (a texture filled once at creation); each one left gets a comment saying so.
- Every per-frame texture write goes through the helpers in `src/gpu/upload.h`, under `GpuDevice::lock()` as today.
- The CPU picture an input hands to `onCpuVideo` (BreadReplay's recorder tap) is unchanged.
- Never push. Do not touch `F:\Coding\BreadMedia\BreadReplay`.

## Review Focus

1. **Colour of UYVY inputs:** solid colours through `uploadUyvy` → `toLinear(UYVY)` → `toBgra8` → readback match today's `uyvyToBgra` within ±6 per channel, at 1080 and 576 lines. Test in Task 1.
2. **Memory stays flat:** 300 uploads of 1920×1080 per helper (UYVY, BGRA, NV12) in an engine that never presents grow private bytes by less than 64 MB. Test in Task 1.
3. **A DeckLink card that switches to RGB 4:4:4** (rowBytes = width × 4) still works (BGRA upload). Task 2 keeps that branch and tests the helper; the card case is noted as hardware-only.
4. **NDI with alpha:** a UYVY-preferring NDI receiver hands BGRA for alpha sources; the input must branch on the frame's FourCC per frame. Test in Task 2.
5. **Matte textures** (Maxine, ONNX) written per frame move to the helper too; their pictures must not change. Covered by the existing matte tests.

---

### Task 1: Upload helpers, `PixelLayout::UYVY`, the shader

**Files:**
- Rename: `src/gpu/nv12_upload.h/.cpp` → `src/gpu/upload.h/.cpp` (`git mv`; update includes and `CMakeLists.txt`)
- Create: `src/gpu/shaders/uyvy_to_linear.hlsl` (add to the shader list in `CMakeLists.txt`)
- Modify: `src/gpu/converter.h/.cpp` (`PixelLayout`, `toLinear`, `uploadBgra8`)
- Test: `tests/gpu_test.cpp`

**Interfaces:**
- Produces, in `upload.h` (caller holds `GpuDevice::lock()`):
  - `void writeTexture(GpuDevice& gpu, ID3D11Texture2D* dynamicTex, const uint8_t* src, uint32_t rowBytes, uint32_t rows);` — today's `fillPlane`, public.
  - `TextureRef uploadNv12Planes(...)` — unchanged signature, moved.
  - `TextureRef uploadUyvy(GpuDevice&, TexturePool&, const uint8_t* uyvy, uint32_t rowBytes, uint32_t w, uint32_t h);` — `w` even (else `invalid_argument`); DYNAMIC `R8G8B8A8_UNORM` `w/2 × h`; the returned `GpuTexture` has the picture's `width = w`, `height = h` and its `srv`.
  - `TextureRef uploadBgra(GpuDevice&, TexturePool&, const uint8_t* bgra, uint32_t rowBytes, uint32_t w, uint32_t h);` — DYNAMIC `B8G8R8A8_UNORM`.
- `enum class PixelLayout { NV12, P010, BGRA8, UYVY };` `toLinear(src, UYVY, fullRange, forceBt709)` dispatches `uyvy_to_linear.hlsl` with `yuvToRgbMatrix(height, fullRange, forceBt709)`; for output pixel x, texel `x / 2` gives U = r, V = b, Y = g (even x) or a (odd x); then `pow(saturate(rgb), 2.4)` as `yuv_to_linear.hlsl`.
- `Converter::uploadBgra8` keeps its signature and uses `uploadBgra` (taking `gpu_.lock()` as it does now).

- [ ] **Step 1: Failing tests**
  - `"Converter: UYVY in matches uyvyToBgra"` (`HardwareGpu`): solid UYVY made by `bgraToUyvy` from black, white, (230, 40, 20), (20, 200, 60) → `uploadUyvy` → `toLinear(UYVY, false)` → `toBgra8` → `readback` → centre pixel within ±6 of `uyvyToBgra` of the same bytes; 1920×1080 (BT.709) and 720×576 (BT.601).
  - `"uploadUyvy refuses an odd width"`.
  - `"per-frame uploads keep memory flat"` (`HardwareGpu`): 300 × each of `uploadUyvy`, `uploadBgra`, `uploadNv12Planes` at 1920×1080, releasing each `TextureRef`; `PROCESS_MEMORY_COUNTERS_EX::PrivateUsage` grows by less than 64 MB (the OMT leak test's method).
- [ ] **Step 2: Run** (`scripts/build.ps1 -SkipUi`, then `bc_tests.exe "--test-case=Converter: UYVY*,uploadUyvy*,per-frame uploads*"`) → build errors.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the new tests and `--test-case=Converter*,OmtInput*` → green.
- [ ] **Step 5: Commit** on `bcam-lean-uyvy-inputs`: `feat(gpu): leak-safe uploads, UYVY in on the GPU`.

### Task 2: Every input uses them

**Files:** Modify `src/io/decklink_input.cpp`, `src/io/aja_input.cpp`, `src/io/ndi_input.cpp`, `src/io/dshow_capture_input.cpp`, `src/io/mf_capture_input.cpp`, `src/io/ffmpeg_input.cpp` (software branch), `src/io/breadcall_input.cpp`, `src/engine/maxine_matte.cpp`, `src/engine/onnx_matte.cpp`; keep `uyvyToBgra` in `src/io/uyvy.h/.cpp` even if BreadCamera stops using it, because BreadReplay's `Renderer` calls it; tests `tests/ndi_input_test.cpp` and the input tests that already exist for each adapter.

**Interfaces:**
- Consumes: Task 1's helpers and `PixelLayout::UYVY`.
- Behaviour:
  - DeckLink: UYVY → `uploadUyvy`, `layout = UYVY`, `fullRange = false`; RGB 4:4:4 (rowBytes ≥ w × 4) → `uploadBgra`.
  - AJA: `uploadUyvy`, `layout = UYVY`, `fullRange = false`.
  - NDI: receive with `NDIlib_recv_color_format_UYVY_BGRA`; per frame, `NDIlib_FourCC_type_UYVY` → `uploadUyvy` (limited range); BGRA/BGRX → `uploadBgra` (full range). The `onCpuVideo` frame reports the format it really is (`CpuPixel::UYVY` or `BGRA`).
  - DirectShow, Media Foundation, FFmpeg software decode, BreadCall: `uploadBgra` in place of `UpdateSubresource` (the BreadCall decode itself changes in phase 4).
  - Maxine and ONNX mattes: their per-frame R8 writes use DYNAMIC textures and `writeTexture`. A one-time fill (e.g. ONNX's opaque texture) may stay, with a comment.

- [ ] **Step 1: Failing tests**
  - `ndi_input_test` (`Ndi`): an NDI sender of a UYVY frame (no alpha) → the input's `VideoFrameIn.layout == PixelLayout::UYVY`; a BGRA frame with alpha → `BGRA8`. Centre colour after `toLinear` → `toBgra8` → readback within ±6 of the sent colour.
  - A source check, as a test: `"no per-frame UpdateSubresource"` is a review step, not a test. Run `git grep -n "UpdateSubresource" src/` and paste the output in the ledger; every line left is a commented one-time write.
- [ ] **Step 2: Run** the NDI test → fails (layout is `BGRA8`).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** each touched adapter's tests (those needing missing hardware skip; list which in the ledger), then the full verification command → green.
- [ ] **Step 5: Commit, merge into `main`** (`feat(io): inputs upload what they receive, leak-safe`, `merge: lean phase 3, UYVY inputs and safe uploads`).
