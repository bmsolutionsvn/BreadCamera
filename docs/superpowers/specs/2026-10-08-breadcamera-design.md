# BreadCamera — Design

Date: 2026-10-08
Status: approved (2026-10-08). Amended 2026-10-08 for the build machine (§7.2) and M0 result (§7.1).
Amended 2026-10-08 after the UI mockup review (owner): English-only UI with Unicode names, native pro-tool
layout with a docked Inspector (§6.4), effects as per-camera add-ins (§5.4), one-time chart calibration instead
of reference matching (§5.3), show resolution and per-output format (§3.2, §4.3), three-state input health (§6.1).
Reference mockups: `docs/design/mockups/index.html`; Multiview brief: `docs/design/multiview.md`.

## 1. Purpose

BreadCamera is a native Windows application for event production crews. It runs
on the production PC next to OBS/vMix. It takes in several cameras, processes
them on the GPU, and sends each one out to the switcher or to a display.

The processing in v1 is:

- AI background removal without a green screen.
- Background blur or replacement.
- One-time colour calibration of each camera against a colour chart, so all
  cameras match.

The pipeline is built so that more processing stages can be added later.

It is a new product in the BM Solutions ecosystem, alongside BreadStream,
BreadCall and BreadOverlay. It follows the ecosystem principle *the broadcast
does not fail*: a camera's output never goes black because of BreadCamera.

### 1.1 Success criteria

- One RTX 4070-class PC handles **4 cameras at 1080p60**, every camera running
  background removal plus colour correction, with no tick overruns.
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
| Colour | One-time calibration per camera against a colour chart (printed ColorChecker Classic 24, or a tablet showing the same chart), then locked |
| UI | Native Windows pro-tool layout: title bar with menus, toolbar, Multiview with a docked Inspector, status bar. English only; names the operator types (cameras, outputs, shows) can be in any language |
| Show format | One show-wide frame rate and resolution; each output may send a smaller size or half the frame rate |
| Stack | C++20 engine. UI in React + `@bmsolutionsvn/design` hosted in WebView2 |
| Process model | Headless engine process plus a UI shell process that supervises it |

### 1.3 Assumptions

- Standalone tool: no Kinde login and no billing in v1.
- Windows 11 is required, because the virtual camera uses `MFCreateVirtualCamera`.
- Every camera runs at a single show-wide frame rate (50, 59.94 or 60) and
  resolution (1280×720 or 1920×1080).

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
- The same pass scales to the show resolution. An input of another size or
  aspect is scaled to fit and letterboxed; it is never stretched.

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

**Output format.** By default an output sends the show format. Each output
may override it:

- **Resolution:** same as show, 1280×720 or 960×540 (never larger than the
  show). The scale is one GPU pass per distinct size, shared by every output
  of that camera that uses the same size.
- **Frame rate:** same as show, or **half** (every second tick, e.g. 29.97
  from 59.94). No other rate conversion is offered, because it judders.

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

### 5.3 Colour calibration (one-time, chart-based)

Each camera is calibrated once, before the show, under show lighting. The
result is locked; the operator calibrates again only if the lighting changes.

**Charts**

- **Printed card:** an X-Rite / Calibrite ColorChecker Classic 24. The target
  is the card's published reference values.
- **Tablet screen:** a tablet showing BreadCamera's chart image (the same 6×4
  layout, sRGB values) at full brightness. A tablet's colours are not absolute,
  so the target is the **first camera calibrated with that tablet in this
  show**: every other camera is matched to that camera's reading of the same
  tablet.

**Flow (in the Inspector, §6.4)**

1. The operator clicks **Calibrate** on the camera's Colour effect. Multiview
   shows that camera alone; every other camera and every output keeps running.
2. The operator holds the chart in front of the camera.
3. The engine finds the chart and places its four corners automatically
   (OpenCV `mcc` ColorChecker detector). The operator drags a corner only if
   it is wrong; with no chart found, the operator places all four by hand.
4. **Capture:** the engine averages about 1 s of frames, before the matte
   stage, and samples the centre of each of the 24 patches.
5. **Solve:** in linear light, a 3×3 colour matrix plus per-channel 1D tone
   curves (monotonic, smoothed, clamped) fitted by least squares to minimise
   the mean ΔE2000 over the 24 patches. The result is baked into a **33³ 3D
   LUT** for that camera and locked.
6. The result is shown in plain words: **Good match** (mean ΔE2000 ≤ 2),
   **Fair** (≤ 4) or **Poor** (> 4, with "check the lighting and glare"). The
   numbers are in a tooltip.

**Also included:** **Undo** (back to the previous LUT) and **Clear**.

### 5.4 Adding stages

- Every stage implements `process()` and has a parameter schema and a bypass.
- The UI builds each stage panel from that schema.
- Adding a new effect means writing one stage class plus registering it.
- In v1 the stage order is fixed. A node graph is a later sub-project.
- **Effects are per-camera add-ins.** The operator adds an effect to a camera
  (Add effect) and removes it again. An effect that is not added is skipped
  and costs nothing. Every registered stage appears in Add effect
  automatically, so a new effect needs no UI work beyond its schema.
- A stage's schema declares its dependencies (Background needs Matte). Add
  effect explains a missing dependency instead of hiding the effect.

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
  - add or remove a camera, input or output (with its output format);
  - add or remove a stage on a camera, and set stage parameters;
  - colour calibration: find the chart, set corners, capture, undo, clear;
  - set the show format (frame rate, resolution);
  - save or load a show.
- When a client connects, the engine pushes a full state snapshot. After that
  it sends patches.
- Stats are pushed at 1 Hz:
  - fps;
  - dropped frames;
  - GPU time per stage;
  - end-to-end latency;
  - health of each output.
- Every output uses the UX-contract signal states, in this order: Ready,
  Connecting, Live, Offline.
- Every input reports its health as one of:
  - **receiving** — frames arriving cleanly (shown green);
  - **degraded** — a gap of more than 100 ms between frames at any time in
    the last 10 s, or reconnecting (shown yellow). Ordinary frame repeats,
    such as a 30 fps camera on a 60 fps show, do not count;
  - **down** — no frames; holding the last frame or showing the slate (shown
    red, with the hold countdown or `SLATE`);
  - **offline** — not started (shown grey).
- Errors are not one of the signal states. They are reported separately, as
  persistent error blocks.

### 6.2 Show file

- A single JSON file stored in `%APPDATA%\BreadCamera\shows\`. It holds:
  - cameras;
  - inputs;
  - the stages added to each camera, and their parameters;
  - LUTs and the chart readings they came from;
  - outputs, with their format overrides;
  - show settings, including the show format.
- Autosave is debounced and runs after every change. A last-known-good copy is
  kept, and the engine loads it after a crash.
- Secrets such as SRT passphrases are encrypted with **DPAPI**. They are never
  stored as plain text.
- The file carries a schema version so that it can be migrated later.

### 6.3 Previews

- A hardware H.264 encoder (§7.2 order) encodes a low-resolution preview for each camera: about 480×270
  at 30 fps.
- The preview is sent over the WebSocket as binary frames and decoded in the UI
  with WebCodecs into a canvas.
- Only tiles that are visible are streamed.
- The camera detail view and the color-match view get a 720p preview.

### 6.4 UI

The reference is the approved mockup `docs/design/mockups/index.html`; the
Multiview strip and colour rules are in `docs/design/multiview.md`.

**Stack and language**

- React + `@bmsolutionsvn/design` tokens (colours, mono numbers), built with
  Vite and embedded in the shell exe, on the WebView2 Evergreen runtime.
- **English only.** No language setting and no translation tables.
- **Names are Unicode end to end.** Camera, output and show names can be in
  any language (e.g. "Sân khấu trái"). They are UTF-8 in the engine, the API
  and the show file, and are converted to UTF-16 only at Win32/COM boundaries
  (window title, virtual-camera friendly name, message boxes). NDI source
  names are sent as UTF-8.

**Look: a native Windows pro tool**

- The Cyan Blueprint palette, Segoe UI Variable at 12 px, JetBrains Mono for
  numbers, 26 px controls, label-left property rows.
- The ecosystem rules still hold: signal states and errors are separate bands
  and shapes; errors are persistent blocks, never toasts; destructive actions
  go through a confirm dialog whose button names the action.

**Window**

- **Title bar** (custom, dark): menus File, Show, Camera, Output, View, Help;
  the show name; minimise, maximise and close. Close hides to the tray.
- **Toolbar:** Add camera · Settings · Stop all outputs…. Adding an output
  or an effect belongs to a camera, so it is done in that camera's Inspector
  (the + on Effects and on Outputs) or from the Camera menu, never from the
  toolbar.
- **Centre: Multiview.** 1–8 tiles (layout Auto, 2×2, 3×2, 4×2), each a
  16:9 picture over a **one-row state strip**: input chip (filled; green
  receiving, yellow degraded, red down with the countdown or `SLATE`, grey
  offline) · `CAM n` · name (cut with an ellipsis) · effect icons · → ·
  output pills (outlined, signal state; a failed output is a rose chip) · a
  mark for an unseen recovered fault. Clicking a tile selects it; nothing on
  a tile changes an output.
- **Right: Inspector** (docked, resizable) for the selected camera: Input,
  Effects (Add effect, the chain, one group per added effect generated from
  its schema, with on/off and Remove), Outputs (Add output, one row per
  output with its format), Remove camera. **Calibrate** on the Colour effect
  runs §5.3 in place: Multiview shows that camera alone until Capture or
  Cancel.
- **Status bar:** engine state and uptime, show format, GPU and matte
  backend, tick overruns, outputs live and retrying, last save.

**Dialogs**

- Settings (Show: frame rate, resolution, hold time, slate; Engine: GPU,
  matte backend, preview encoder; About).
- Add camera, Add output (type, keyed or composited, name, resolution, frame
  rate), Add effect.
- Confirms: Stop all outputs, change of show format, remove camera, remove
  output, clear calibration.

## 7. Repo and build

- Location: `F:\Coding\BreadMedia\BreadCamera`, a git repo of its own.
- Toolchain: C++20, Visual Studio 2026 (MSVC v14.5x, the build machine's only VS), and the CMake and vcpkg bundled with it.
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
| NVIDIA Maxine VFX SDK + AI Green Screen model | NVIDIA Software License Agreement, Product-Specific Terms for NVIDIA AI Products, NVIDIA Community Model License | **M0 done (2026-10-08):** NGC states the Virtual Background features "are available for commercial/non-commercial use". Downloads need a free NVIDIA Developer Program (NGC) account. Ship only the runtime files the SLA marks redistributable. |
| Blackmagic DeckLink SDK | Blackmagic SDK EULA | Redistribute only the DLLs that are allowed. |
| AJA NTV2 | MIT | Attribution. |
| libomt | MIT | Attribution. |

### 7.2 Build machine and optional SDKs (amendment 2026-10-08)

The machine that builds and tests BreadCamera overnight (Hermes) has an **AMD Radeon RX 6600** and no NVIDIA GPU. The owner chose to stay NVIDIA-first anyway, so:

- Every vendor or hardware SDK (CUDA, Maxine, NDI, DeckLink, AJA NTV2, libomt) is a CMake option `BC_WITH_<NAME>` that defaults to ON when the SDK is found and switches itself OFF with a status message when it is not. A missing SDK never fails the build.
- A test that needs an SDK, a card or an NVIDIA GPU is tagged with a doctest decorator and **skips with a printed reason** when the requirement is missing. A skip is never reported as a pass.
- Code that cannot run on the build machine (CUDA interop, Maxine, NVENC) is written, compiled when its SDK is present, and listed as **"untested on hardware"** in `docs/superpowers/follow-ups.md` until someone runs it on an RTX machine.
- Shader and D3D11 tests run on the real GPU when there is one, else on the WARP software adapter, so CI covers them too.
- H.264 encoding (previews and SRT out) goes through FFmpeg and picks the first encoder that opens, in this order: `h264_nvenc`, `h264_amf`, `h264_qsv`, `h264_mf`. NVIDIA stays first; the others cost nothing and keep the previews working on the build machine.

## 8. Testing

### 8.1 Unit tests

- Framework: doctest.
- Run in CI.
- Cover:
  - frame-sync repeat and drop;
  - the output queue drop policy;
  - the signal and error state machines;
  - saving, loading and migrating the show file, and the DPAPI round-trip;
  - **the calibration solver.** The test renders a synthetic ColorChecker,
    applies a known colour transform, runs the solver and checks that the
    mean ΔE2000 is below 2;
  - **chart detection** on synthetic chart images: rotated, scaled, partly
    lit, and in the tablet style;
  - **Unicode names:** a camera named "Sân khấu trái – Cam 2" round-trips
    through the API, the show file, the NDI source name and the
    virtual-camera name unchanged.

### 8.2 GPU tests

These run locally on an RTX machine.

- Golden-image comparisons, with a tolerance, for:
  - the RGBA conversion;
  - the LUT;
  - output scaling and half-rate output;
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
| **M0** | ~~Confirm the Maxine license~~ **Done 2026-10-08** (§7.1): commercial use allowed. | — |
| **M1** | End-to-end passthrough: SRT input → decode → tick → NDI and fullscreen outputs, with the show format and per-output format. Also the control API, show file, supervisor and the native UI shell (title bar, toolbar, Multiview, Inspector, status bar). | 4× 1080p60 passthrough with the input-loss hold and slate working. |
| **M2** | Matte (batched Maxine) and the background modes keyed, blur and replace. Alpha over NDI. | All 4 cameras matted within budget. Keyed output works in vMix and OBS. |
| **M3** | Colour calibration: chart detection, the solver, the Calibrate flow in the Inspector. | The solver and detection tests pass. Calibration works on real cameras with a printed card and with a tablet. |
| **M4** | The remaining I/O: NDI input, DeckLink, AJA (experimental), UVC, OMT, SRT output, virtual camera, SDI output. | Every adapter works except AJA. AJA compiles and is labeled experimental. |
| **M5** | Hardening: the soak and fault-injection tests, the installer, the Settings dialog and the error text. | §8.3 and §8.4 pass. A clean install works on a fresh Windows 11 machine. |
| **M6** | Non-NVIDIA path: the ONNX matte backend (the AMF/QSV/MF encoders already come with §7.2). | The 4-camera load runs on an AMD or Intel GPU at a documented, possibly lower, quality and frame rate. |

## 10. Out of scope (later sub-projects)

- LiveKit input that subscribes to BreadCall rooms directly.
- Scopes: waveform and vectorscope.
- Genlock to an external sync reference.
- Video backgrounds.
- A user-reorderable node graph of stages.
- Remote control, for example from the BreadCall director page.
- Continuous colour matching.
- Manual colour trims and `.cube` LUT import.
- UI languages other than English.
- A 3840×2160 show format.
- More than one GPU.

### 10.1 Add-in roadmap (owner, 2026-10-08)

The owner approved the add-ins below for later sub-projects. Each one ships as an add-in effect (§5.4). Each runs on NVIDIA Maxine when an RTX card is present and otherwise falls back to ONNX on DirectML (the M6 path), or it is marked RTX-only.

In priority order:

1. **Denoise and low-light enhancement.** It also removes WebRTC compression artifacts. It targets remote guests on webcams.
2. **Super resolution.** It upscales a source below the show resolution (for example a 720p guest) to the show format.
3. **Colour grading.** It adds manual trims and `.cube` LUT import to the Colour effect. AI relighting comes later.
4. **Audio noise suppression and room echo removal.** This needs an audio stage type, since today's stages are video only. The stage reports the delay it adds, and the camera's video is delayed by the same amount so lips stay in sync.
5. **Live speech-to-text captions.** These are per show, not per camera. They go out as their own output, either as text with a transparent background (fill and key) or as caption data, for the switcher to lay over the picture. Expected delay is 1–3 s. Vietnamese accuracy must be confirmed before the work is committed. Captions can be in any language; the rule that the UI is English only does not apply to them.

Not on the roadmap:

- **Maybe later, not scheduled:** subtle skin smoothing, and opt-in eye-contact correction for each camera.
- **Not planned:** frame interpolation (it adds delay and artifacts; frame repeat covers 25/30 fps sources) and head-pose correction.
- **Never:** face or body reshaping, virtual makeup, age or gender morphing, and voice changing. The one exception is a future identity-protection pack (voice disguise with face blur), and only if a customer asks for it.
