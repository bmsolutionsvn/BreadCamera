# OMT copy paths Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop the OMT input's memory leak, send OMT HX outputs straight from the GPU, and bring OMT HX inputs in as NV12 — without changing plain OMT (VMX) in either direction.

**Architecture:** BreadCamera finds and fixes the leak behind a failing test. OMT-HX's `sendVideoD3D11` learns RGBA textures, and BreadCamera hands HX outputs a per-tick RGBA texture instead of a CPU readback. `OmtInput` receives HX sources as NV12 planes, uploads them as two small textures, and converts them on the GPU with an explicit BT.709 matrix.

**Tech Stack:** C++20 (MSVC, VS 2026), D3D11, FFmpeg (via OMT-HX), doctest; two repos: BreadCamera and OMT-HX.

**Spec:** `docs/superpowers/specs/2026-11-03-omt-copy-paths-design.md` (binding). Background: `docs/superpowers/2026-11-03-copy-path-audit.md`.

## Global Constraints

- Repos and branches:
  - BreadCamera `F:\Coding\BreadMedia\BreadCamera`: branch `bcam-omt-copy-paths` from `main`.
  - OMT-HX `F:\Coding\BreadMedia\OMT-HX`: branch `omthx-rgba-d3d11` from `main`, merged into OMT-HX `main` (`--no-ff`) when its tests pass, **before** BreadCamera Task 3 (BreadCamera builds whatever `../OMT-HX` has checked out).
  - Never push. Never use `git worktree`.
- OMT-HX's working tree has the owner's uncommitted work: `omt/libomtnet` (modified submodule), `dist/`, `scripts/link-check.ps1`. Never stage, commit, stash, reset or discard them. If `scripts/build.ps1`'s `git submodule update --init` fails because of them, stop and report.
- **Plain OMT (Full / VMX) must not change**, input or output, on the wire or in BreadCamera's handling, for compatibility with vMix and other OMT tools. The HX wire format does not change either.
- BreadCamera verification command (every BreadCamera task), from `docs/superpowers/plans/2026-10-11-remaining-milestones-opencode.md`:
  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1
  ./build/release/bc_tests.exe '--test-case-exclude=*acceptance*,ControlServer: refuses a wrong token*,ControlServer: hostile messages*,FfmpegInput: an SRT listener*,FfmpegInput: stop() returns promptly*'
  ```
- OMT-HX verification: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1` (builds, then runs `omthx_tests.exe`).
- Never hold `GpuDevice::lock()` across `omthx::Sender::sendVideoD3D11` (spec §4.2).
- Colour checks: ±8 per 8-bit channel against the colour sent.
- Tests that need OMT and a GPU encoder use `BC_REQUIRES(bc::test::Req::Omt)` and `BC_REQUIRES(bc::test::Req::HardwareGpu)`.

## Review Focus

1. **The leak fix holds with the real engine**, not only in a unit harness: memory flat over 300+ frames of a 1080p source through `Camera`. Task 1 test, Task 5 measurement (private memory at window start and end).
2. **A keyed HX output, a Full output, an odd-sized frame and a `sendVideoD3D11` throw** each end on the CPU path with the picture intact. None goes black for more than the one dropped frame. Task 3 tests.
3. **A camera with an HX GPU output plus another CPU output** (NDI, SRT) still reads back for the other output, and the HX output still uses the GPU path. Task 3 test.
4. **An HX input below 720p** converts with BT.709, not BT.601. A plain OMT source stays BGRA. Task 4 tests.
5. **The tick under load:** 4 HX GPU outputs must not add tick overruns compared with the CPU path (`/engine` overruns in the snapshot before and after). Task 5.

---

### Task 1: Find and fix the OMT input leak (gate)

**Files:**
- Test: `tests/omt_input_test.cpp`
- Modify: wherever the root cause is (expected in `src/io/omt_input.cpp` or the engine path that consumes it).

**Interfaces:**
- Produces: nothing new. The behaviour change is "memory stays flat".

- [ ] **Step 1: Write the failing test** `OmtInput: memory stays flat while a camera receives 300 frames` (`BC_REQUIRES(Omt)`, `BC_REQUIRES(HardwareGpu)`):
  - an in-process `omthx::Sender("bc-leak-test", omthx::Mode::Full, omthx::Quality::Medium)` sends a solid 1920×1080 BGRA picture every 33 ms on a thread;
  - an `Engine` (as `tests/engine_test.cpp` builds one, with the real `DefaultAdapterFactory`) runs a show with one camera whose input is `{"kind":"omt","params":{"source": sender.address()}}`;
  - wait until the camera has taken 60 frames, then read private bytes (`GetProcessMemoryInfo`, `PROCESS_MEMORY_COUNTERS_EX::PrivateUsage`);
  - wait for 240 more frames, read again, and `CHECK(grownBytes < 150 * 1024 * 1024)`.
- [ ] **Step 2: Run** it and watch it fail with growth near 240 frames × 8.3 MB. If it does **not** fail, record that in the ledger and repeat with `omthx::Mode::Hx`. If neither reproduces in `bc_tests`, reproduce with `breadcamera-engine.exe` and one `omthx-send` source (the audit's method) and continue from there, keeping the test.
- [ ] **Step 3: Find the root cause** with superpowers:systematic-debugging. Known facts (spec §2):
  - OMT-HX + libomt alone do not leak.
  - A plain-OMT source, which never touches FFmpeg, leaks in the engine.
  - SRT inputs (hardware decode) do not leak.

  Narrow it by removing one thing at a time inside `OmtInput::run` (the texture upload, `sink_->onVideo`, the audio drain, the per-iteration `omthx::VideoFrame`), and record each result in the ledger. Write the root cause in the commit body.
- [ ] **Step 4: Fix it at the root.** No periodic receiver restart, no cap that hides growth.
- [ ] **Step 5: Run** the test (passes), then the full verification command.
- [ ] **Step 6: Commit** — `fix(io): OMT input no longer keeps a picture per received frame`.
- **Stop condition:** if the root cause is not found after two rounds of hypotheses, commit the failing test as `test(io): OMT input memory growth (fails; root cause not found)` with the investigation in the body. Write `DONE` in the ledger and stop the run. Tasks 3 and 4 touch the same paths.

### Task 2: OMT-HX accepts RGBA textures in `sendVideoD3D11` (OMT-HX repo)

**Files:**
- Modify: `src/d3d11.cpp` (`BgraToNv12::Impl::convert`, the format check; input-format support check), `include/omthx/omthx.h` and `include/omthx/omthx_c.h` (comments on `sendVideoD3D11` / `omthx_c_sender_video_d3d11`)
- Test: `tests/link_test.cpp`

**Interfaces:**
- Produces: `omthx::Sender::sendVideoD3D11` accepts `DXGI_FORMAT_R8G8B8A8_UNORM` / `_SRGB` as well as `B8G8R8A8_UNORM` / `_SRGB`. It throws `std::runtime_error("the video processor cannot read <format name>")` when `CheckVideoProcessorFormat` reports no input support.

- [ ] **Step 1: Write the failing test** `sendVideoD3D11 takes an RGBA texture`: a copy of the existing D3D11 test with `desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM` and the pixel bytes in R, G, B, A order (white left half, red right half, green outside the picture). The same assertions: format equal, encoder name contains `(d3d11)`, white > 240, red channel > 235 with the others < 20.
- [ ] **Step 2: Run** `scripts/test.ps1 -Filter "*RGBA*"`. Expected: FAIL (`std::invalid_argument`, "the texture must be BGRA").
- [ ] **Step 3: Implement.** Widen the format check. After `setup`, check the input format once per format with `enumerator->CheckVideoProcessorFormat` (`D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT`) and throw as above when unsupported. Update both header comments to "BGRA or RGBA".
- [ ] **Step 4: Run** `scripts/test.ps1` (the full OMT-HX suite passes, including the old BGRA D3D11 test).
- [ ] **Step 5: Commit** — `feat: sendVideoD3D11 also takes RGBA textures`. Then `git checkout main && git merge --no-ff omthx-rgba-d3d11 -m "merge: RGBA textures for sendVideoD3D11" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"`. Leave OMT-HX on `main`.

### Task 3: BreadCamera sends HX outputs from the GPU

**Files:**
- Modify: `src/io/output.h` (`OutFrame::rgba8`, `Output::wantsGpuRgba8`), `src/io/omt_output.h` / `.cpp`, `src/engine/adapter_factory.cpp` (pass `keyed`), `src/engine/camera.cpp` (`endTick`: make `rgba8` once when wanted; `anyCpu` already follows `needsCpu()`)
- Test: `tests/omt_output_test.cpp`, `tests/engine_test.cpp`

**Interfaces:**
- Consumes: Task 2's RGBA `sendVideoD3D11`; `Converter::toBgra8(linear, w, h, /*swapRb=*/false)`.
- Produces:
  ```cpp
  // output.h
  struct OutFrame { /* … */ TextureRef rgba8; };  // R8G8B8A8_UNORM, true RGBA, the frame's size; set when any output wantsGpuRgba8()
  class Output { /* … */ virtual bool wantsGpuRgba8() const { return false; } };
  // omt_output.h
  OmthOutput(std::string name, omthx::Mode mode, omthx::Codec codec, bool keyed);
  bool needsCpu() const override;       // mode == Full || keyed_ || gpuRefused_
  bool wantsGpuRgba8() const override;  // !needsCpu()
  ```
  State warning when the GPU path is refused: `/outputs/<output id>/warning` = `"OMT HX is sending through memory: <reason>"`. If outputs have no state path of their own, use `OutputStatus::error` with code `omt-gpu-refused` and keep the state `Live`, and record a `Ruling:`.

- [ ] **Step 1: Write the failing tests:**
  - `OmthOutput: the HX output without alpha uses the GPU path`: `OmthOutput("t", Hx, Hevc, false)` → `needsCpu() == false`, `wantsGpuRgba8() == true`. `Full` → `needsCpu() == true`, `wantsGpuRgba8() == false`. Hx with `keyed = true` → `needsCpu() == true`.
  - `OmthOutput: an HX output sends the GPU frame` (`Omt`, `HardwareGpu`): start the output and submit 120 `OutFrame`s at 33 ms with `rgba8` = a 640×360 `R8G8B8A8` texture of RGB (230, 40, 20) and no `bgra`. An `omthx::Receiver(output.address())` gets a picture whose centre is (230, 40, 20) ±8, and the receiver's `stats().encoder` contains `(d3d11)`.
  - `OmthOutput: an odd-sized frame falls back to the CPU path`: submit a 641×360 `rgba8` frame → `needsCpu()` turns true and the status carries the warning. Then frames with `bgra` set are sent and received.
  - `Camera: an HX GPU output alone does not read back` (`engine_test.cpp`, with a fake output reporting `needsCpu() = false` and `wantsGpuRgba8() = true`): after 30 ticks the camera's `tickCpu()` is null and the output received `OutFrame`s with `rgba8` set. With a second fake output where `needsCpu() = true`, `tickCpu()` is set and both outputs receive their frames.
- [ ] **Step 2: Run** `bc_tests.exe -tc="OmthOutput*,Camera: an HX GPU output*"`. Expected: build fails or tests fail.
- [ ] **Step 3: Implement** per spec §4.2. In `endTick`, `rgba8` is made only when some output `wantsGpuRgba8()`, under the existing GPU lock, once per tick. `deliver` uses `sendVideoD3D11` when `f.rgba8` is set, the frame has no alpha, the size is even and the path is not refused. Otherwise it keeps today's `sendVideo(bgra)`. Catch `std::exception` from `sendVideoD3D11` → refuse (atomic) + warning, and drop that frame.
- [ ] **Step 4: Run** the new tests (all pass), then the full verification command.
- [ ] **Step 5: Commit** — `perf(io): OMT HX outputs send from the GPU, without the tick readback`.

### Task 4: OMT HX inputs arrive as NV12

**Files:**
- Create: `src/gpu/nv12_upload.h`, `src/gpu/nv12_upload.cpp` (add to `bc_gpu`)
- Modify: `src/io/input.h` (`VideoFrameIn::bt709`), `src/io/omt_input.cpp`, `src/gpu/converter.h` / `.cpp` (`toLinear` overload), `src/engine/camera.cpp` (pass `bt709`)
- Test: `tests/omt_input_test.cpp`, `tests/gpu_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  // nv12_upload.h — a GpuTexture whose srvLuma/srvChroma are an R8 (w x h) and an R8G8 (w/2 x h/2) pooled texture,
  // filled from tightly packed planes (stride = width). Holds both pooled refs. Caller holds GpuDevice::lock().
  TextureRef uploadNv12Planes(GpuDevice& gpu, TexturePool& pool, const uint8_t* y, const uint8_t* uv, uint32_t w, uint32_t h);
  // converter.h
  TextureRef toLinear(const GpuTexture& src, PixelLayout layout, bool fullRange, bool forceBt709);
  // input.h
  struct VideoFrameIn { /* … */ bool bt709 = false; };  // force BT.709 for YUV at any height
  ```

- [ ] **Step 1: Write the failing tests:**
  - `gpu_test.cpp`, `Converter: NV12 planes convert with BT.709 at any size`: full-range BT.709 planes for RGB (200, 50, 100) at 640×360, uploaded with `uploadNv12Planes`, then `toLinear(…, NV12, true, true)` → `toBgra8` → readback. The centre is (200, 50, 100) ±8. With `forceBt709 = false` at the same size, the result differs by more than 8 in at least one channel (proving the matrix switch).
  - `omt_input_test.cpp`, `OmtInput: an HX source arrives as NV12` (`Omt`, `HardwareGpu`): an in-process `omthx::Sender` (Hx, HEVC) sends 640×360 solid (200, 50, 100). The fake sink's frame has `layout == PixelLayout::NV12`, `fullRange`, `bt709`, and converts to the sent colour ±8.
  - `OmtInput: a plain OMT source stays BGRA`: the same with `Mode::Full` → `layout == PixelLayout::BGRA8`, colour ±8.
- [ ] **Step 2: Run** `bc_tests.exe -tc="Converter: NV12 planes*,OmtInput: an HX source*,OmtInput: a plain OMT source*"`. Expected: build fails.
- [ ] **Step 3: Implement** per spec §4.3. `OmtInput` opens the picture receiver as `PixelFormat::Nv12`. On the first picture with `receiver->stats().mode == omthx::Mode::Full`, it reopens as `PixelFormat::Bgra`, the same way the coded→picture switch reopens today. NV12 frames go through `uploadNv12Planes` under the GPU lock, with `layout = NV12`, `fullRange = true` and `bt709 = true`. The camera's `toLinear` call passes `frame.bt709`. Check the alpha rule in spec §4.3 and log a `Ruling:` either way.
- [ ] **Step 4: Run** the new tests and the Task 1 leak test (all pass), then the full verification command.
- [ ] **Step 5: Commit** — `perf(io): OMT HX inputs arrive as NV12 and convert on the GPU`.

### Task 5: Measure and record

**Files:**
- Modify: `docs/superpowers/2026-11-03-copy-path-audit.md` (an "After" section), `docs/superpowers/specs/2026-11-03-omt-copy-paths-design.md` (status line)

- [ ] **Step 1: Run the audit's cases** with `.superpowers/omt-bench.ps1` (copied there by the planner; engine only, 4 cameras at 1080p30, 30 s each): P0, P1 (HX out), P2 (Full out, must be within noise of before), P3 (HX in). Also record the engine's overrun count from the snapshot at the end of each case. If the script needs a fix to run, fix it in `.superpowers/` only and log a `Ruling:`.
- [ ] **Step 2: Write the "After" table** under the audit's "Measured" table: same columns, plus private memory at the window's start and end. Then one line per finding saying what changed.
- [ ] **Step 3: Update the spec's status line** ("implemented; measured <date>").
- [ ] **Step 4: Commit** — `docs: OMT copy paths measured after the fixes`.
