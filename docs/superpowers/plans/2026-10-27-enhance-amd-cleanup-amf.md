# Enhance on AMD: clean-up with AMF VQ Enhancer — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. In BreadCamera this plan is handed to OpenCode: follow the per-task procedure, verification command, stop conditions and human gates in `docs/superpowers/plans/2026-10-11-remaining-milestones-opencode.md`, plus the stop conditions in this file.

**Goal:** On AMD Radeon cards, Enhance removes compression blockiness at the same size (AMD AMF VQ Enhancer), and when it also resizes it cleans first and then upscales with the existing FSR 1 path.

**Architecture:** A new `AmfEnhancer` (`src/gpu/amf_enhancer.{h,cpp}`, behind `BC_WITH_AMF`) loads the AMD driver's AMF runtime, initialises an AMF context on BreadCamera's own D3D11 device and runs `AMFVQEnhancer` on 8-bit RGBA textures with no CPU round-trip, like `RtxVideo`. `Camera::enhance` uses it only when `tools_.rtx` is null. The pure helpers `enhancePlan`/`enhanceNote` from the Enhance plan gain an `amf` flag so the decision and the note texts stay testable without a GPU.

**Tech Stack:** MSVC C++20, D3D11, AMD AMF 1.5.3 headers (vcpkg `amd-amf`, local overlay port `ports/amd-amf`; runtime `amfrt64.dll` from the AMD driver), doctest, CMake/vcpkg.

**Spec:** `docs/superpowers/specs/2026-10-18-webcam-cleanup-upscale-colour-match-design.md` with its `Amendment 2026-10-27 — Enhance` (E1–E9, written by the Enhance plan's Task 4), amended by A1–A7 below (Task 2 writes them into the spec). Builds on `docs/superpowers/plans/2026-10-27-enhance-merge-cleanup-into-upscale.md`.

**Branch:** `bcam-enhance-amd`, cut from `main` **after** `bcam-enhance` is merged into it. Before Task 1, confirm `main` has `Converter::toBgra8(..., bool swapRb)`, `bc::enhancePlan`, `bc::enhanceNote`, `bc::vsrLevel` and `Camera::upscaleQuality_`; if any is missing, stop (the Enhance plan is not done). One commit per task. OpenCode merges this branch into `main` itself when green (`CLAUDE.md`, owner 2026-10-27); no extra worktrees.

## Decisions (owner approved 2026-10-27)

- **A1.** On a PC with no RTX device and a working `AmfEnhancer`, Enhance runs the AMF VQ Enhancer **at the input size, before any resize**. A resize then goes through the existing FSR 1 → fit chain. AMF HQScaler is **not** used: its `VIDEOSR1_0` algorithm is FSR 1 (which `Upscaler` already is) and `VIDEOSR1_1` is "exposed purely for experimental use" in AMD's 1.5.3 docs.
- **A2.** The AMF pass runs when the input height is ≤ **1080** (the E4 limit). Above 1080 lines with no resize, nothing runs.
- **A3.** Quality → `AMF_VE_FCR_ATTENUATION`: fast **0.04**, balanced **0.07**, high **0.1** (AMF's default), ultra **0.2**; anything else 0.1. `AMF_VE_FCR_RADIUS` is not set: AMD's 1.5.3 VQ Enhancer guide marks it deprecated with no effect. The four values live in one table (`amfAttenuation`) so the owner can tune them.
- **A4.** Note texts (exact; replaces the no-RTX part of E7):
  - RTX present: E7 unchanged.
  - AMF present, resize → none.
  - AMF present, no resize, input ≤ 1080 lines → `Clean-up only`.
  - AMF present, no resize, input above 1080 lines → `Input is above 1080p`.
  - Neither, `native` → `Same size needs an NVIDIA RTX or AMD Radeon card` (was `Same size needs an NVIDIA RTX card`). Neither, 720/1080 target at or above the input → `Input is already 720p` / `Input is already 1080p` (unchanged). A 4K `native` camera with neither says `Same size needs an NVIDIA RTX or AMD Radeon card`.
- **A5.** E1 description becomes: `Sharpens and removes compression blockiness, and raises a low-resolution camera to 720p or 1080p. AI on NVIDIA RTX cards; AMD's video enhancer and a sharp standard upscaler on AMD Radeon cards; upscaling only on other GPUs.`
- **A6.** The AMF runtime is the driver's `amfrt64.dll`, loaded from System32 only; BreadCamera ships no AMF binary. `BC_WITH_AMF` turns on when the AMF headers are found; a missing header never fails the build. On an NVIDIA or Intel PC `AmfEnhancer::create` returns null.
- **A7.** E6 holds: the AMF pass and the FSR 1 pass after it are one guarded, timed block on `upscaleGuard_`/`upscaleTimer_` with the `period / 4` budget. A failing or overrunning AMD pass bypasses that camera only: resize → plain fit at `want`; same size → the input unchanged.

## Global Constraints

- MSVC C++20 `/W4 /permissive- /utf-8`; the tick never blocks; the main path never copies a full frame to the CPU.
- Every SDK behind `BC_WITH_<NAME>`; a missing SDK never fails the build. No AMF DLL, sample or AMF source file is committed (headers come from vcpkg).
- Use the header macros for AMF property names, never string literals (`AMF_VIDEO_ENHANCER_ENGINE_TYPE` expands to the misspelt `L"AMF_VIDEI_ENHANCER_ENGINE_TYPE"`).
- Never black (A7). An Enhance camera on an RTX PC behaves exactly as after the Enhance plan.
- UI copy: English, UK spelling, exact strings from A4, A5.
- Conventional commit prefixes; never `git add -A`; never `--no-verify`. AMD-only tests use `BC_REQUIRES(bc::test::Req::Amf)`; they **run** on the RX 6600 build PC.

## Stop conditions (in addition to the OpenCode procedure)

- **S1 — AMF refuses VQ Enhancer on RDNA2.** If `AmfEnhancer: starts on this PC's AMD card` fails on the RX 6600, stop the whole plan: commit nothing, do not start Task 2. Report the `whyNot` text (it names the failing call and the `AMF_RESULT`), the runtime version `AmfEnhancer::create` logs, and the driver version (`Get-CimInstance Win32_VideoController | Select Name,DriverVersion`).
- **S2 — no frame on the same call.** If `run()` throws `AMF VQ Enhancer returned no frame` in the colour test, stop and report: a one-frame-late design is the owner's call.
- **S3 — no visible effect.** If the blocking test shows the AMF output no smoother than its input, or ultra (0.2) no smoother than fast (0.04), stop and report the four step numbers. Do not change the assertions or reverse the A3 table.

## Review Focus

1. **Red and blue swapped through AMF** (the surface is wrapped as RGBA; the pipeline's default 8-bit order is BGRA). Pinned in Task 1 (`AmfEnhancer: keeps size and colour`).
2. **An AMD driver without VQ Enhancer** (older Adrenalin, GCN cards): `create` returns null with a reason, the engine prints it once, Enhance behaves as on any non-RTX GPU with the A4 "neither" note. Pinned in Task 1 (WARP case) and Task 2 (`enhanceNote` with `amf = false`; the existing no-card engine test, which runs without `amf` on the RX 6600).
3. **A webcam that renegotiates from 1280×720 to 640×360 and back on AMD**: each size gets its own component, outputs keep the input size, no throw. Pinned in Task 1 (the size loop in `AmfEnhancer: keeps size and colour`).
4. **A 4K camera at Same size on AMD**: nothing runs, note `Input is above 1080p`, picture unchanged. Pinned in Task 2 via `enhancePlan`/`enhanceNote`.
5. **The AMF pass overrunning `period / 4` at 1080p60** on the RX 6600: the guard bypasses that camera, the picture passes through. The mechanism is the existing `StageGuard`; Task 1 records the real 1080p GPU time in the acceptance case so the owner sees the margin.

---

### Task 1: `AmfEnhancer` on the shared D3D11 device

**Files:**
- Modify: `vcpkg.json` (add `"amd-amf"` to `dependencies`; it is already installed through `ffmpeg[amf]`, the overlay port in `ports/amd-amf` keeps applying), `CMakeLists.txt`, `src/gpu/device.h`, `tests/support/requires.h`, `tests/support/requires.cpp`
- Create: `src/gpu/amf_enhancer.h`, `src/gpu/amf_enhancer.cpp`, `tests/amf_enhancer_test.cpp`

**Interfaces:**
- Consumes: `TextureRef Converter::toBgra8(const GpuTexture& linear, uint32_t outW, uint32_t outH, bool swapRb = true)` (Enhance Task 1; `false` writes true RGBA), `Converter::toLinear(..., PixelLayout::BGRA8, true)` (reads through the SRV, so a true-RGBA `R8G8B8A8_UNORM` texture reads correctly).
- Produces:
  - `bool GpuDevice::isAmd() const { return info_.vendorId == 0x1002; }`
  - `class AmfEnhancer` in `src/gpu/amf_enhancer.h`, SDK-free header (pimpl like `RtxVideo`):
    - `static std::unique_ptr<AmfEnhancer> create(GpuDevice& gpu, TexturePool& pool, Converter& conv, std::string& whyNot);`
    - `TextureRef run(const GpuTexture& linear, float attenuation);` — linear RGBA16F in and out, same size. Throws `std::runtime_error` on any AMF failure or when no frame comes back.
    - non-copyable, destructor releases components, then the context, then `FreeLibrary`.
  - `bc::test::Req::Amf` — missing when `BC_WITH_AMF == 0`, or the hardware device is WARP or not AMD (`reason = "needs BC_WITH_AMF (AMD AMF headers) and an AMD GPU"`). It deliberately does **not** call `create`, so an AMF refusal fails loudly (S1) instead of skipping.
  - CMake option `BC_WITH_AMF`, compile definition `BC_WITH_AMF=0|1` PUBLIC on `bc_gpu`.

`whyNot` strings (exact; stderr and tests only, never the UI — E9):
- not AMD, or WARP (checked first, whatever the build): `Needs an AMD Radeon card.`
- `BC_WITH_AMF == 0`: `This build has no AMD AMF support.`
- `amfrt64.dll` missing or `AMFInit` failing: `Needs an AMD Radeon driver with AMF.`
- any later call failing: `AMF VQ Enhancer refused to start: <call> returned <AMF_RESULT as int>.` with `<call>` one of `CreateContext`, `InitDX11`, `CreateComponent`, `Init`.

- [ ] **Step 1: Write the failing tests** in `tests/amf_enhancer_test.cpp` (add to `bc_tests` in `CMakeLists.txt` next to `tests/rtx_video_test.cpp`). Copy the `Rig` and `centreRgb` helpers from `tests/rtx_video_test.cpp`.

```cpp
TEST_CASE("AmfEnhancer: not created on WARP, with the plain reason") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Warp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  std::string why;
  CHECK(bc::AmfEnhancer::create(*gpu, pool, conv, why) == nullptr);
  CHECK(why == "Needs an AMD Radeon card.");
}

TEST_CASE("AmfEnhancer: starts on this PC's AMD card" BC_REQUIRES(bc::test::Req::Amf)) {
  Rig r;  // Pref::HardwareOrWarp; Req::Amf guarantees an AMD hardware device
  std::string why;
  auto amf = bc::AmfEnhancer::create(*r.gpu, r.pool, r.conv, why);
  INFO(why);
  REQUIRE(amf != nullptr);  // S1 if this fails on the RX 6600
}

TEST_CASE("AmfEnhancer: keeps size and colour" BC_REQUIRES(bc::test::Req::Amf)) {
  Rig r;
  std::string why;
  auto amf = bc::AmfEnhancer::create(*r.gpu, r.pool, r.conv, why);
  REQUIRE(amf != nullptr);
  // Strongly red, so an R/B swap moves the centre by ~140 levels. Sizes change like a
  // renegotiating webcam; every call must return its own frame (S2).
  for (auto [w, h] : {std::pair{1280u, 720u}, {640u, 360u}, {1280u, 720u}}) {
    auto in = r.conv.solid(w, h, {0.6f, 0.1f, 0.05f, 1});
    const auto expected = centreRgb(r.conv, in);
    for (int i = 0; i < 10; ++i) {
      auto out = amf->run(*in, 0.1f);
      CHECK(out->width == w);
      CHECK(out->height == h);
      const auto got = centreRgb(r.conv, out);
      for (int c = 0; c < 3; ++c) CHECK(std::abs(got[c] - expected[c]) <= 3);
    }
  }
}

TEST_CASE("AmfEnhancer: smooths 8x8 blocking, more at a higher strength" BC_REQUIRES(bc::test::Req::Amf)) {
  // 640x360 grey image of flat 8x8 blocks, value(bx, by) = 32 + 2*bx + by (bx = x/8, by = y/8),
  // uploaded with conv.uploadBgra8 and taken to linear with toLinear(BGRA8, true).
  // step(t) = mean |G[x] - G[x-1]| over every row and every x > 0 with x % 8 == 0,
  //           on readback(toBgra8(t)).
  // const double in = step(linear);                      // ~2.0 after the 8-bit round trip
  // const double def = step(amf->run(*linear, 0.1f));
  // const double fast = step(amf->run(*linear, 0.04f));  // A3 fast
  // const double ultra = step(amf->run(*linear, 0.2f));  // A3 ultra
  // MESSAGE the four numbers; CHECK(def < in); CHECK(ultra < fast);   // S3 if either fails
}

TEST_CASE("AmfEnhancer acceptance: 1080p clean-up GPU time" BC_REQUIRES(bc::test::Req::Amf)) {
  // 120 runs of a 1920x1080 frame at 0.1f, each between bc::GpuTimer begin()/end(), polled each run.
  // MESSAGE the median and the max ms. CHECK only the output size. The owner judges the numbers
  // against period / 4 (4.17 ms at 60 fps, 8.33 ms at 30 fps).
}
```

- [ ] **Step 2: Run them and watch them fail** — verification command. Expected: compile error (`gpu/amf_enhancer.h` not found, `Req::Amf` not declared).

- [ ] **Step 3: Implement.**
  - `CMakeLists.txt`, next to the RTX block: `find_path(AMF_INCLUDE_DIR NAMES AMF/components/VQEnhancer.h)`; `AMF_FOUND` from it; `option(BC_WITH_AMF "Build the AMD AMF clean-up for Enhance" ${AMF_FOUND})` with the same "ON but not found; turning it off" warning and `message(STATUS "BC_WITH_AMF=...")` as the other options; add `src/gpu/amf_enhancer.cpp` to `bc_gpu` (compiles to a null path when off); `target_include_directories(bc_gpu PRIVATE ${AMF_INCLUDE_DIR})` and `BC_WITH_AMF=1` when on, `BC_WITH_AMF=0` otherwise (both PUBLIC so `requires.cpp` sees it).
  - `amf_enhancer.cpp`: AMF includes (`AMF/core/Factory.h`, `AMF/components/VQEnhancer.h`) inside `#pragma warning(push, 0)` / `pop`. Use the header's smart pointers (`amf::AMFContextPtr`, `AMFComponentPtr`, `AMFSurfacePtr`, `AMFDataPtr`). AMF types, enums and helpers are named below without their namespace; qualify them the way the 1.5.3 headers declare them (most live in `namespace amf`, e.g. `amf::AMF_DX11_0`).
  - `create`: card check → build check → `LoadLibraryExW(AMF_DLL_NAME, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)` → `AMF_QUERY_VERSION_FUNCTION_NAME` (print the runtime version to stderr once as `[breadcamera] AMF runtime <major>.<minor>.<release>`) → `AMF_INIT_FUNCTION_NAME` with `AMF_FULL_VERSION` → `CreateContext` → `InitDX11(gpu.device(), AMF_DX11_0)` → build and `Init` one 1280×720 component as a probe (kept in the cache below). Any failure returns null with the A-string above.
  - Components: one per input size, in a `std::map<std::pair<uint32_t, uint32_t>, AMFComponentPtr>` created on first use. Before `Init(amf::AMF_SURFACE_RGBA, w, h)`: `AMF_VIDEO_ENHANCER_ENGINE_TYPE = amf::AMF_MEMORY_DX11`, `AMF_VIDEO_ENHANCER_OUTPUT_SIZE = AMFConstructSize(w, h)`, `AMF_VE_FCR_ATTENUATION = attenuation`. Keep each component's last attenuation and `SetProperty` it only when `run` is called with a different value. Add `// ponytail: one component per size, never evicted; add an LRU if renegotiating webcams pile up sizes`.
  - `run`: `in8 = conv_.toBgra8(linear, linear.width, linear.height, /*swapRb=*/false)`; then under `gpu_.lock()` (AMF records on the shared immediate context): `CreateSurfaceFromDX11Native(in8->tex.Get(), &surf, nullptr)` → `SubmitInput` → `QueryOutput` while it returns `AMF_REPEAT`, yielding, for at most 2 ms (then throw `AMF VQ Enhancer returned no frame`) → plane 0 `GetNative()` as `ID3D11Texture2D*` → `CopyResource` into `pool_.acquire(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE)`. Outside the lock: `return conv_.toLinear(*copy, PixelLayout::BGRA8, true);`. Any `AMF_RESULT` other than `AMF_OK` throws `std::runtime_error("AMF VQ Enhancer <call> failed (<code>)")`.
  - `requires.h/.cpp`: add `Amf` to `Req` and its `case` as in Interfaces.

- [ ] **Step 4: Run the tests and watch them pass** — verification command, then `./build/release/bc_tests.exe --test-case="AmfEnhancer*"` (this one includes the acceptance case). Expected on the RX 6600: all five cases run (none skipped) and pass; record the blocking-test numbers and the 1080p median/max ms in the commit body. Any of S1–S3 → stop as written above. The WARP case passes on every PC.

- [ ] **Step 5: Commit** — `feat(gpu): AMD AMF VQ Enhancer on the shared D3D11 device`

---

### Task 2: Enhance cleans with AMF on AMD cards

**Files:**
- Modify: `src/core/upscale_size.h`, `src/core/stage_params.cpp` (A5 description), `src/engine/gpu_tools.h`, `src/engine/camera.cpp`, `src/engine/engine_main.cpp`, `docs/superpowers/specs/2026-10-18-webcam-cleanup-upscale-colour-match-design.md`, `docs/superpowers/manual-checks/webcam-fx.md`
- Test: `tests/upscaler_test.cpp`, `tests/engine_upscale_test.cpp`

**Interfaces:**
- Consumes: `AmfEnhancer::create/run` (Task 1); from the Enhance plan: `EnhancePlan`, `enhancePlan`, `enhanceNote`, `vsrLevel`, `Camera::upscaleQuality_` (1..4).
- Produces (in `src/core/upscale_size.h`, pure):
  - `float amfAttenuation(int level);` — A3 by `vsrLevel` value: 1 → 0.04f, 2 → 0.07f, 3 → 0.1f, 4 → 0.2f, else 0.1f.
  - `struct EnhancePlan { FrameSize want; bool resize; bool vsr; bool vq = false; };` — `vq`: run the AMF pass at the input size.
  - `EnhancePlan enhancePlan(uint32_t inW, uint32_t inH, uint32_t targetH, bool rtx, bool amf = false);` — `vq = !rtx && amf && inH <= 1080` (A1, A2); everything else as E4.
  - `std::optional<std::string> enhanceNote(uint32_t inW, uint32_t inH, uint32_t targetH, bool rtx, bool amf = false);` — A4.
  - `GpuTools::amf` — `AmfEnhancer* amf = nullptr;` (comment: the AMD clean-up for Enhance, used only when `rtx` is null).

- [ ] **Step 1: Write the failing tests**

`tests/upscaler_test.cpp`, new case:
```cpp
TEST_CASE("enhancePlan/enhanceNote: AMD cleans at the input size, then FSR resizes") {
  auto p = bc::enhancePlan(1920, 1080, 0, false, true);    // native, 1080p
  CHECK_FALSE(p.resize); CHECK_FALSE(p.vsr); CHECK(p.vq); CHECK(p.want.w == 1920);
  p = bc::enhancePlan(640, 360, 1080, false, true);        // clean, then resize
  CHECK(p.resize); CHECK(p.vq); CHECK(p.want.h == 1080);
  p = bc::enhancePlan(3840, 2160, 0, false, true);
  CHECK_FALSE(p.vq); CHECK_FALSE(p.resize);
  p = bc::enhancePlan(1920, 1080, 0, true, true);          // RTX wins
  CHECK(p.vsr); CHECK_FALSE(p.vq);
  CHECK(*bc::enhanceNote(1920, 1080, 0, false, true) == "Clean-up only");
  CHECK(*bc::enhanceNote(1280, 720, 720, false, true) == "Clean-up only");
  CHECK_FALSE(bc::enhanceNote(640, 360, 1080, false, true));
  CHECK(*bc::enhanceNote(3840, 2160, 0, false, true) == "Input is above 1080p");
  CHECK(bc::amfAttenuation(1) == doctest::Approx(0.04)); CHECK(bc::amfAttenuation(2) == doctest::Approx(0.07));
  CHECK(bc::amfAttenuation(3) == doctest::Approx(0.1));  CHECK(bc::amfAttenuation(4) == doctest::Approx(0.2));
  CHECK(bc::amfAttenuation(9) == doctest::Approx(0.1));
}
```
In the Enhance plan's `enhanceNote: exact texts` case and in `Camera: Enhance at Same size without an RTX card passes the picture through and says why`, change `"Same size needs an NVIDIA RTX card"` to `"Same size needs an NVIDIA RTX or AMD Radeon card"` (three places; this is A4, not a loosened check).

`tests/engine_upscale_test.cpp`: an `AmdRig` like `Rig` plus `std::string why; std::unique_ptr<bc::AmfEnhancer> amf = bc::AmfEnhancer::create(*gpu, pool, conv, why);` passed as `.amf = amf.get()` (declare `amf` before `engine`). Both cases `BC_REQUIRES(bc::test::Req::Amf)` and `REQUIRE(amf)`:
- `Camera: Enhance at Same size on an AMD card cleans with AMF` — `target: native`, fake input 1280×720, engine run as the existing Upscale tests do: last output 1280×720, `/cameras/a/stages/upscale/note == "Clean-up only"`, `state == "active"`, `gpuMs` a number > 0.
- `Camera: Enhance 1080p on an AMD card cleans then upscales` — `target: 1080`, input 640×360: output 1920×1080, `note` is null, `state == "active"`.

- [ ] **Step 2: Run them and watch them fail** — verification command. Expected: compile errors (`vq`, the fifth parameter, `amfAttenuation`, `GpuTools::amf`); the three renamed note asserts fail.

- [ ] **Step 3: Implement**
  - The helpers and the `GpuTools::amf` member as in Interfaces; A5 description in the `upscale` schema in `stage_params.cpp`.
  - `Camera::enhance`: `plan = enhancePlan(..., tools_.rtx != nullptr, tools_.amf != nullptr)`; the early return becomes `!plan.resize && !plan.vsr && !plan.vq`. After the RTX branch (`plan.vq` implies no RTX device), when `plan.vq && upscaleGuard_.shouldRun(now)`: one block timed exactly like the RTX branch (GPU timer when present, else CPU time against `upscaleBudget_`) that does `frame = tools_.amf->run(*in, amfAttenuation(upscaleQuality_))` and, when `plan.resize`, `frame = tools_.up ? tools_.up->apply(*frame, want.w, want.h) : tools_.conv.fit(*frame, want.w, want.h)`. On a throw: `upscaleGuard_.failed(e.what(), now)` and `frame = plan.resize ? tools_.conv.fit(*in, want.w, want.h) : in` (A7). Otherwise the existing resize chain / pass-through runs unchanged.
  - `publishStatus`: pass `tools_.amf != nullptr` to `enhanceNote`.
  - `engine_main.cpp`: after `rtx`, `std::string amfWhy; auto amf = rtx ? nullptr : bc::AmfEnhancer::create(*gpu, pool, conv, amfWhy);`; when null and `gpu->isAmd()`, print `[breadcamera] Enhance clean-up is unavailable: <amfWhy>` to stderr once; pass `.amf = amf.get()` into `GpuTools`. Declare `amf` before `engine` so it outlives it.
  - Spec: append `## Amendment 2026-10-27b — Enhance on AMD` with A1–A7 verbatim; in the E1/E7 lines of the earlier amendment, add "superseded by A4/A5" after the replaced texts.
  - `manual-checks/webcam-fx.md`, AMD PC section: replace "Same size shows the E7 note and the picture passes through" with — Same size on a 1080p webcam, or an SRT input sent at about 1.5 Mb/s 1080p: note `Clean-up only`, an Enhance GPU time in the Inspector, blockiness visibly lower than with Enhance off; Fast against Ultra visibly different; switching Enhance on and off never shows black. A 720p webcam at 1080p: no note, output 1920×1080.

- [ ] **Step 4: Run the tests and watch them pass** — verification command. Expected: PASS on the RX 6600 with both `AmdRig` cases run (not skipped); on WARP they are skipped and the rest pass.

- [ ] **Step 5: Commit** — `feat(engine): Enhance cleans up with AMD AMF on Radeon cards`

**Human gate (owner, RX 6600 build PC):** run the AMD steps in `manual-checks/webcam-fx.md` and judge the 1080p GPU time from Task 1's commit body. If Fast and Ultra look the same or Ultra smears detail, retune A3 (one table). This gate is not a merge condition: OpenCode lists it as open in its report.
