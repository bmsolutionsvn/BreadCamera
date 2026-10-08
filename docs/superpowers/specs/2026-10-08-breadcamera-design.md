# BreadCamera — Design

Date: 2026-10-08
Status: approved in brainstorming, awaiting written-spec review

## 1. Purpose

BreadCamera is a native Windows application for event production crews. It runs
on the production PC next to OBS/vMix. It takes in several cameras, processes
them on the GPU, and sends each one out to the switcher or to a display.

The processing in v1 is:

- AI background removal without a green screen.
- Background blur or replacement.
- One-shot color matching between cameras.

The pipeline is built so that more processing stages can be added later.

It is a new product in the BM Solutions ecosystem, alongside BreadStream,
BreadCall and BreadOverlay. It follows the ecosystem principle *the broadcast
does not fail*: a camera's output never goes black because of BreadCamera.

### 1.1 Success criteria

- One RTX 4070-class PC handles **4 cameras at 1080p60**, every camera running
  background removal plus color matching, with no tick overruns.
- BreadCamera adds **≤ 50 ms** end-to-end latency per camera.
- No failure of one component blacks out an output:
  - An input drop leads to hold, then a slate.
  - A stage failure leads to that stage being bypassed.
  - An output failure affects only that output.
  - An engine crash leads to a restart into the same show in under 3 s.
- An 8-hour soak run passes with flat memory usage.

### 1.2 Decisions taken during brainstorming

| Topic | Decision |
|---|---|
| Users | Event production crews, on the Windows production PC next to OBS/vMix |
| GPU target | NVIDIA first. Vendor-neutral layers (D3D11, ONNX Runtime, d3d11va) so AMD/Intel can follow |
| Load | 4× 1080p60 with all effects |
| Inputs | SRT (which covers BreadCall per-feed SRT), NDI, DeckLink, AJA, USB/UVC. LiveKit later |
| Outputs | NDI, OMT, SRT, Windows virtual camera, SDI (DeckLink/AJA), fullscreen display |
| Background removal | NVIDIA Maxine VFX SDK AI Green Screen, plus an ONNX fallback behind the same interface |
| Color matching | One-shot auto-match to a reference camera, then locked, with manual trims on top |
| Stack | C++20 engine. UI in React + `@bmsolutionsvn/design` hosted in WebView2 |
| Process model | Headless engine process plus a UI shell process that supervises it |

### 1.3 Assumptions

- Standalone tool: no Kinde login and no billing in v1.
- Windows 11 is required, because the virtual camera uses `MFCreateVirtualCamera`.
- Every camera runs at a single show-wide frame rate (50, 59.94 or 60).

## 2. Architecture

```
┌──────────────────── BreadCamera.exe (shell) ─────────────────────┐
│ WebView2 UI (React + bm-design)   tray icon   engine supervisor  │
└───────────────▲──────────────────────────────────────────────────┘
                │ WebSocket 127.0.0.1 + per-launch token
┌───────────────┴──────── breadcamera-engine.exe ──────────────────┐
│ Inputs ─▶ Decode ─▶ Master tick ─▶ Stage chains ─▶ Output fan-out │
│ (threads)  (d3d11va)  (show rate)   (one D3D11 device)  (threads) │
│ Control API · Show store · Stats · Preview encoder (NVENC)        │
└───────────────────────────────────────────────────────────────────┘
```

- **Engine (`breadcamera-engine.exe`)**
  - Headless.
  - Owns every input, the GPU pipeline, every output and the show state.
  - Runs whether or not the UI is open.
- **Shell (`BreadCamera.exe`)**
  - Hosts the UI in WebView2 and owns the tray icon.
  - Starts the engine and restarts it when needed:
    - when the engine process exits;
    - when there is no heartbeat for 3 s.
  - Closing the window hides the UI to the tray. The outputs keep running.
  - Stopping everything is a separate, explicit action. It opens a confirm
    dialog whose primary button reads "Dừng tất cả đầu ra".

Approach chosen over the alternatives:

- **A single process** was rejected: a crash in the UI or in any SDK would take
  down every output.
- **A process per camera** was rejected: it breaks batched inference and
  cross-camera color matching, and it adds per-frame cross-process sync.

## 3. Frame pipeline (engine)

### 3.1 GPU memory model

- Everything runs on **one D3D11 device** with multithread protection on.
- Frames are GPU textures. The main path never copies them to the CPU.
- Maxine and TensorRT access the same textures through CUDA–D3D11 interop.
- D3D11 was chosen over D3D12 because NDI, DeckLink, the MF virtual camera, the
  swapchain and the Maxine samples all work with D3D11 natively.

### 3.2 Per-camera chain

```
Input ─▶ Decode ─▶ To RGBA ─▶ Color ─▶ Matte ─▶ Background ─▶ Output fan-out
```

**Decode**

- Compressed inputs (SRT) are decoded by FFmpeg with the `d3d11va` hwaccel.
  This works on NVIDIA, AMD and Intel.
- Uncompressed inputs (DeckLink, AJA, UVC, NDI) are uploaded to a texture.

**To RGBA**

- A compute shader converts NV12, P010, UYVY or BGRA into RGBA16F, in linear
  BT.709.

**Stages**

- The stage order is fixed: **Color → Matte → Background**.
- Color comes first so that the matte model sees the matched image.

**Frame object**

- A ref-counted texture taken from a per-camera pool.
- Carries a timestamp, a has-alpha flag and the associated audio buffer.
- Outputs hold references to frames, so fan-out never copies.

### 3.3 Clocking and frame sync

- A single **master tick** runs at the show rate. At each tick, every camera
  takes its newest decoded frame:
  - If the input is slower than the show rate, the frame is repeated (for
    example, a 30 fps input).
  - If the input is faster, frames are dropped.
  - If the input has stalled, the last frame is repeated.
- Outputs only ever see tick frames, so an input can never stall an output.
- Locking the tick to an external sync reference (genlock) is out of scope for
  v1.

### 3.4 Batching and latency

- The Matte stage is a **shared service**. It collects every camera's frame for
  a tick and makes one batched Maxine call.
- Processing of tick N overlaps with output of tick N−1.
- The latency budget is about 2–3 frames (35–50 ms), made up of:

  | Part | Frames |
  |---|---|
  | Jitter buffer | 1 |
  | Processing | 1 |
  | Pipelining | 1 |

### 3.5 Audio

- Audio passes through untouched.
- It is delayed by the measured video pipeline latency to keep lip-sync.
- It goes out on every output that carries audio: NDI, OMT, SRT and SDI.

### 3.6 Failure rules

| Failure | Behavior |
|---|---|
| A stage throws an error or overruns its share of the tick budget | That stage is bypassed for that camera only. The camera shows an error badge. The stage is retried after 5 s. |
| An input drops | Hold the last frame for N s (configurable, default 5 s), then show the slate image. Reconnect with backoff. |
| An output fails | The output goes into the error state and retries with backoff. Other outputs are unaffected. |
| The GPU device is lost or the engine crashes | The engine exits, the shell restarts it, and it reloads the last-known-good show. |

## 4. Inputs and outputs

### 4.1 Adapter model

- There are two interfaces, `Input` and `Output`.
- Every adapter runs on its own thread. Blocking SDK calls never run on the
  tick thread.
- Outputs receive frames through a **bounded queue of 2**:
  - When an output falls behind, it drops its own oldest frame and counts the
    drop.
  - A slow output can never slow down the engine.
- Outputs that need frames in CPU memory (NDI, OMT, virtual camera) share one
  GPU→CPU readback per camera per tick.
- SDI and fullscreen outputs read directly from GPU textures.

### 4.2 Inputs (v1)

| Adapter | Built on | Notes |
|---|---|---|
| SRT | FFmpeg + libsrt | Caller or listener mode. Passphrase, latency and `streamid` settings. A BreadCall feed is an SRT caller pointed at the node agent's per-feed URL. |
| NDI | NDI SDK `NDIlib_recv` | Video (alpha included) and audio. |
| DeckLink | Blackmagic DeckLink SDK | SDI/HDMI input, embedded audio, input format auto-detection. |
| AJA | AJA NTV2 SDK (MIT) | SDI/HDMI input. **Experimental**: there is no AJA hardware to test on yet, and the UI labels it "not verified on hardware". |
| USB/UVC | Media Foundation | Magewell, Elgato, AVerMedia, webcams. |

### 4.3 Outputs (v1)

Every camera can have several outputs. Each output sends one of two things:

- **keyed**: fill plus alpha;
- **composited**: the finished frame.

| Adapter | Built on | Alpha | Notes |
|---|---|---|---|
| NDI | NDI SDK | ✅ UYVA | |
| OMT | libomt (MIT) | ✅ | |
| SRT | NVENC (H.264/HEVC) → MPEG-TS → libsrt | ❌ | Caller or listener mode |
| Virtual camera | `MFCreateVirtualCamera` + COM media source DLL | ❌ | One device per camera that uses it |
| SDI | DeckLink, AJA (experimental) | ✅ key+fill | Fill and key go out on two SDI ports |
| Fullscreen | Borderless D3D11 swapchain on a chosen monitor | ❌ | LED walls, projectors, confidence monitors |

## 5. Effects

### 5.1 Matte (background removal)

**Backends**

`MatteBackend` is an interface with two implementations. The engine picks one
at startup based on the GPU.

- **Maxine AI Green Screen** (NVIDIA RTX, Turing or newer):
  - batched across cameras;
  - temporal state kept per camera;
  - a quality or performance mode that the operator can select.
- **ONNX Runtime**:
  - TensorRT execution provider on NVIDIA, DirectML on AMD/Intel;
  - uses a permissively licensed person-matting model (Apache-2.0/MIT class);
  - ships in M6.

**Operator matte controls** (shader-based, per camera)

| Control | Default |
|---|---|
| Choke (shrink or grow the mask) | 0 px |
| Feather (soften the edge) | 1 px |
| Threshold | 0.5 |

**Known limits** (shared with NVIDIA Broadcast)

- The model is trained on people. It does not reliably keep held objects or
  furniture.
- Edges on hair and during fast motion are not as clean as a real green screen.

### 5.2 Background stage

Each camera uses one of these modes:

- **Keyed** — fill plus alpha goes to the outputs, and the switcher composites
  it.
- **Blur** — a masked separable Gaussian blur with a strength setting. The blur
  is weighted by (1 − alpha) so that the person does not bleed into the blurred
  background as a halo.
- **Replace** — the person is composited over a still image or a solid color.

Notes:

- The blur is BreadCamera's own shader, not Maxine's blur effect, so it works
  the same way on both backends.
- Video backgrounds are out of scope for v1.

### 5.3 Color match (one-shot)

**Steps**

1. The operator picks a reference camera and one or more target cameras.
2. Optionally, the operator draws a region box on each camera over shared
   content: a face, a shirt or a grey card. With no box, the whole frame is
   used.
3. The operator clicks **Match**. The engine averages about 1 s of frames from
   each camera, before the matte stage.
4. The engine solves the correction in three passes:
   1. White-balance and exposure gains from the means of the regions.
   2. Mean and spread transfer in Lab color space.
   3. Per-channel tone-curve matching (CDF matching). The curves are clamped
      and smoothed so they cannot cause banding or extreme shifts.
5. The result is baked into a **33³ 3D LUT** for each target camera. The LUT
   stays **locked** until the next Match.

**Trims on top of the LUT**

- exposure;
- temperature and tint;
- lift, gamma and gain;
- saturation.

**Also included**

- **.cube LUT import**, using the same LUT texture.
- **Undo** (one step back to the previous LUT) and **reset**.

### 5.4 Adding stages

- Every stage implements `process()` and has a parameter schema and a bypass.
- The UI builds each stage panel from that schema.
- Adding a new effect means writing one stage class plus registering it.
- In v1 the stage order is fixed. A node graph is a later sub-project.

## 6. Control API, show file, UI

### 6.1 Control API

**Transport and auth**

- WebSocket bound to **127.0.0.1 only**.
- The shell generates a random token at each launch and passes it to the
  engine.
- Every connection must present that token. Connections without it are
  refused.

**Messages**

- Commands:
  - add or remove a camera, input or output;
  - set stage parameters;
  - run color match;
  - save or load a show.
- When a client connects, the engine pushes a full state snapshot. After that
  it sends patches.
- Stats are pushed at 1 Hz:
  - fps;
  - dropped frames;
  - GPU time per stage;
  - end-to-end latency;
  - health of each output.
- Every input and output uses the UX-contract signal states, in this order:
  Ready, Connecting, Live, Offline.
- Errors are not one of the signal states. They are reported separately, as
  persistent error blocks.

### 6.2 Show file

- A single JSON file stored in `%APPDATA%\BreadCamera\shows\`. It holds:
  - cameras;
  - inputs;
  - stage parameters;
  - LUTs;
  - outputs;
  - show settings.
- Autosave is debounced and runs after every change. A last-known-good copy is
  kept, and the engine loads it after a crash.
- Secrets such as SRT passphrases are encrypted with **DPAPI**. They are never
  stored as plain text.
- The file carries a schema version so that it can be migrated later.

### 6.3 Previews

- NVENC encodes a low-resolution H.264 preview for each camera: about 480×270
  at 30 fps.
- The preview is sent over the WebSocket as binary frames and decoded in the UI
  with WebCodecs into a canvas.
- Only tiles that are visible are streamed.
- The camera detail view and the color-match view get a 720p preview.

### 6.4 UI

**Stack**

- React + `@bmsolutionsvn/design` (MUI), built with Vite and embedded in the
  shell exe.
- Runs on the WebView2 Evergreen runtime.
- Follows the shared UX contract: the shell layout, one primary action per
  screen, the signal states, confirm dialogs and persistent error blocks.
- Vietnamese first, English second, using the glossary terms exactly.

**Screens**

- **Multiview** — a grid of all cameras showing status, effect badges and
  errors.
- **Camera detail** — input settings, the stage panels (generated from each
  stage's schema) and the output list.
- **Color match** — the reference and target cameras side by side, region
  boxes, the Match button and the trims.
- **Settings** — show frame rate, GPU and matte backend, input-loss hold time
  and slate image.

## 7. Repo and build

- Location: `F:\Coding\BreadMedia\BreadCamera`, a git repo of its own.
- Toolchain: C++20, MSVC 2022, CMake and vcpkg.
- vcpkg dependencies:
  - FFmpeg, built with **LGPL features only** (no x264/x265 or other GPL
    parts);
  - libsrt;
  - ONNX Runtime.
- `third_party/` holds the SDKs that are not in vcpkg: NDI, Maxine VFX,
  DeckLink, AJA NTV2 and libomt. A fetch script pulls them at pinned versions.
- `ui/` is a Vite package. Its build output is embedded into the shell exe.
- The installer is **Inno Setup**. It installs the engine and the shell,
  registers the virtual camera's COM media source, and installs the runtimes.
- CI runs on a GitHub Actions Windows runner, which has no GPU. CI builds
  everything and runs the CPU tests.

### 7.1 Third-party licenses

| Component | License | Obligation |
|---|---|---|
| FFmpeg (LGPL build) | LGPL-2.1+ | Link dynamically. Ship the source offer and the license text. |
| libsrt | MPL-2.0 | Publish file-level changes if we modify it. |
| ONNX Runtime | MIT | Attribution. |
| NDI SDK | NDI SDK license | Follow the NDI branding and attribution requirements. |
| NVIDIA Maxine VFX models | NVIDIA Community Model License or NVIDIA Open Model License, depending on the model | **Must be verified first (M0)**. |
| Blackmagic DeckLink SDK | Blackmagic SDK EULA | Redistribute only the DLLs that are allowed. |
| AJA NTV2 | MIT | Attribution. |
| libomt | MIT | Attribution. |

## 8. Testing

### 8.1 Unit tests

- Framework: doctest.
- Run in CI.
- Cover:
  - frame-sync repeat and drop;
  - the output queue drop policy;
  - the signal and error state machines;
  - saving, loading and migrating the show file, and the DPAPI round-trip;
  - **the color-match solver.** The test applies a known transform to a test
    image, runs Match and checks that the mean ΔE2000 is below 2.

### 8.2 GPU tests

These run locally on an RTX machine.

- Golden-image comparisons, with a tolerance, for:
  - the RGBA conversion;
  - the LUT;
  - blur;
  - choke and feather.
- A matte smoke test on a sample clip.

### 8.3 Soak test

**Setup**

- 4× 1080p60 SRT test sources generated by FFmpeg.
- All effects on and every output type active.
- Runs for 8 hours.

**Pass criteria**

- Zero tick overruns.
- Memory usage stays flat.
- Latency stays at 50 ms or below, measured with a flash-frame test.

### 8.4 Fault injection

| Action | Expected result |
|---|---|
| Kill an input | Hold, then slate. The input reconnects when it comes back. |
| Kill an NDI receiver | All other outputs are unaffected. |
| Force a stage to fail | That stage is bypassed for that camera only. |
| Kill the engine | The shell restarts it into the same show in under 3 s. |

## 9. Milestones

There is one implementation plan per milestone. They are written in order.

| # | Delivers | Done when |
|---|---|---|
| **M0** | Confirm the license terms of the Maxine Green Screen model for commercial redistribution. | Terms are recorded in this spec. If they don't allow it, M2 uses the ONNX backend and M6 moves earlier. |
| **M1** | End-to-end passthrough: SRT input → decode → tick → NDI and fullscreen outputs. Also the control API, show file, supervisor and a minimal multiview UI. | 4× 1080p60 passthrough with the input-loss hold and slate working. |
| **M2** | Matte (batched Maxine) and the background modes keyed, blur and replace. Alpha over NDI. | All 4 cameras matted within budget. Keyed output works in vMix and OBS. |
| **M3** | Color match, trims and .cube import. | The solver test passes. Matching works on real cameras. |
| **M4** | The remaining I/O: NDI input, DeckLink, AJA (experimental), UVC, OMT, SRT output, virtual camera, SDI output. | Every adapter works except AJA. AJA compiles and is labeled experimental. |
| **M5** | Hardening: the soak and fault-injection tests, the installer and the Vietnamese and English strings. | §8.3 and §8.4 pass. A clean install works on a fresh Windows 11 machine. |
| **M6** | Non-NVIDIA path: the ONNX matte backend and AMF/QSV encoders. | The 4-camera load runs on an AMD or Intel GPU at a documented, possibly lower, quality and frame rate. |

## 10. Out of scope (later sub-projects)

- LiveKit input that subscribes to BreadCall rooms directly.
- Scopes: waveform and vectorscope.
- Genlock to an external sync reference.
- Video backgrounds.
- A user-reorderable node graph of stages.
- Remote control, for example from the BreadCall director page.
- Continuous color matching.
- More than one GPU.
