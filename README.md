# BreadCamera

Multi-camera GPU processing for live production on Windows. BreadCamera takes several cameras in,
cleans each one up on the GPU, and sends it on to your switcher (vMix, OBS, a hardware switcher)
or to a display.

<!-- Add a 20-30 s GIF here: four cameras in the Multiview, background removal on, output in vMix. -->

> **Status: early development (0.2.x).** One developer builds and tests it on two PCs: an AMD
> Radeon RX 6600 desktop and an NVIDIA GTX 1650 laptop. Several hardware paths have never run on
> real hardware. Read [What has been tested](#what-has-been-tested) before you use it on a show.

## What it does

Each camera is one input, a chain of effects you add to it, and any number of outputs.

**Inputs**

- SRT, as listener or caller, with hardware decode (d3d11va), audio and automatic reconnect
- NDI, with source discovery and reconnect; NDI|HX in low-bandwidth mode
- OMT and OMT-HX
- USB and UVC cameras through Media Foundation or DirectShow, including OBS Virtual Camera
- BreadCall guests: each guest slot in a BreadCall room becomes a camera
- Blackmagic DeckLink SDI, with automatic format detection
- AJA SDI (experimental, off by default)

**Effects** (per camera, in any combination)

- **Background removal**: an AI person cut-out with no green screen. It uses ONNX Runtime with
  DirectML (MODNet) on any DirectX 12 GPU, or NVIDIA Maxine AI Green Screen when its SDK is present.
- **Background**: blur, replace, or keyed output (fill + key) for the switcher to composite.
- **Colour**: calibrate each camera once against a ColorChecker Classic 24 (printed, or shown on a
  tablet) so every camera matches, or match a camera to another one by skin tone.
- **Auto frame**: keeps the speaker's face in the shot (YuNet face detection), with no camera operator.
- **Enhance**: sharpens, removes compression blockiness and raises a low-resolution camera to 720p
  or 1080p. AI (NVIDIA RTX Video) on RTX cards, AMD FSR 1 on other GPUs.
- The Inspector shows how many milliseconds of GPU time each effect costs, measured with D3D11
  timestamp queries.

**Outputs**

- NDI
- OMT, plain or OMT-HX (hardware H.264/HEVC, see [OMT-HX](https://github.com/bmsolutionsvn/OMT-HX))
- SRT: hardware H.264 and AAC in MPEG-TS
- Windows 11 virtual camera (up to 4 cameras, 1080p30): other apps see it as a webcam
- DeckLink SDI: fill, or fill and key on two connectors for an external keyer
- AJA SDI, fill only (experimental)
- Fullscreen on any monitor

**When something fails**

- The engine is a separate, headless process, so if the UI crashes the outputs keep running.
  Closing the window asks first, then quits and stops everything.
- When an input drops, the camera holds its last frame and then shows a slate. Outputs never go black.
- A failing effect is bypassed for that camera only. A failing output affects only itself.
- The shell supervises the engine and restarts it if it exits.

## What has been tested

| Area | Status |
|---|---|
| Automated tests | 337 doctest cases. On the AMD build PC 316 pass and 21 are skipped: those need hardware or an SDK that PC does not have, and each skip prints its reason. Engine end-to-end tests push SRT through effects and read the outputs back. |
| Full engine run: 4 SRT inputs, Colour + Background removal + Background, then SRT, OMT, NDI and fullscreen outputs | Run on the GTX 1650 laptop, with every OMT output read back by an independent receiver ([report](docs/superpowers/manual-checks/m1-real-e2e.md)). That laptop does not keep up with four cameras with every effect on. |
| Load on the RX 6600: 4 SRT inputs at 1080p30, hardware decode, no outputs | 14.4 % CPU, 25.4 % GPU, 860 MB of memory, flat over 30 s ([audit](docs/superpowers/2026-11-03-copy-path-audit.md)) |
| OBS Virtual Camera as an input | Checked by hand ([notes](docs/superpowers/manual-checks/dshow-obs-vcam.md)) |
| Native previews: the engine draws the Multiview straight to the window | Implemented; the owner's acceptance check is still to do |
| BreadCall guests | Automated test against a local LiveKit server; the check with two real guests is still to do |
| NVIDIA Maxine, RTX Video, NVENC previews | Compiled, **untested on hardware** |
| DeckLink, AJA, Windows virtual camera output | **Untested on hardware** ([checklist](docs/superpowers/manual-checks/m4b-hardware.md)) |
| 4 cameras at 1080p60 with every effect, at most 50 ms added latency, on an RTX 4070-class PC | Design target, **not measured yet** |

Known shortcuts and open items, one line each: [docs/superpowers/follow-ups.md](docs/superpowers/follow-ups.md).

## Requirements

- Windows 11, x64. The virtual camera relies on an API that only Windows 11 has.
- A GPU with DirectX 12 support. Background removal runs through DirectML on AMD, Intel and NVIDIA.
- Optional, for each feature: the NDI Runtime for NDI, Blackmagic Desktop Video for DeckLink, and
  an NVIDIA RTX card for Maxine and RTX Video.

## Install

<!-- Publish BreadCamera-Setup-<version>.exe as a GitHub Release, then keep this section. -->
Download `BreadCamera-Setup-<version>.exe` from [Releases](https://github.com/bmsolutionsvn/BreadCamera/releases)
and run it. The installer adds the Visual C++ runtime and registers the virtual camera, so it asks
for administrator rights. Shows are saved in `%APPDATA%\BreadCamera\shows` and are kept on uninstall.

## Build from source

You need:

- Visual Studio 2022 or newer with the "Desktop development with C++" workload. Its bundled CMake,
  Ninja and vcpkg are used.
- Node.js 24, git and PowerShell.
- For OMT support, [OMT-HX](https://github.com/bmsolutionsvn/OMT-HX) cloned beside this repository
  with its submodules (`..\OMT-HX`). Without it, OMT is left out of the build.

```powershell
git clone https://github.com/bmsolutionsvn/BreadCamera
cd BreadCamera
powershell -File scripts\fetch-sdks.ps1   # ONNX Runtime, DirectML, MODNet and YuNet models, hash-checked
powershell -File scripts\build.ps1        # UI (npm) + engine and shell (CMake, Ninja, vcpkg)
powershell -File scripts\test.ps1         # the doctest suite
```

The app is `build\release\BreadCamera.exe`. `scripts\package.ps1` builds the installer and needs
Inno Setup.

<!-- Decide: publish @bmsolutionsvn/design publicly, or keep this paragraph. -->
The UI uses the `@bmsolutionsvn/design` package from GitHub Packages, which is not public yet.
Until it is, building the UI needs a token with access to it. The engine and the tests build
without it: `scripts\build.ps1 -SkipUi`.

Optional SDKs are picked up when present. Without them, those features are not built:

| Feature | SDK | Where CMake looks |
|---|---|---|
| NDI input and output | NDI 6 SDK | `NDI_SDK_DIR`, or `C:\Program Files\NDI\NDI 6 SDK` |
| DeckLink | Blackmagic DeckLink SDK | `DECKLINK_SDK_DIR` |
| Maxine background removal | NVIDIA Maxine VFX SDK | `VFXSDK_ROOT` |
| RTX Video enhance | NVIDIA RTX Video SDK | `RTXVIDEO_ROOT` |
| AJA (experimental) | libajantv2 | configure with `-DBC_WITH_AJA=ON` |

## How it is built

```
┌──────────────────── BreadCamera.exe (shell) ─────────────────────┐
│ WebView2 UI (React)              tray icon     engine supervisor │
└───────────────▲──────────────────────────────────────────────────┘
                │ WebSocket on 127.0.0.1 + a per-launch token
┌───────────────┴──────── breadcamera-engine.exe ──────────────────┐
│ Inputs ─▶ Decode ─▶ Effect chains ─▶ Output fan-out               │
│ (threads)  (d3d11va)  (one D3D11 device)  (threads, own queues)   │
│ Control API · Show store · Stats · Previews                       │
└───────────────────────────────────────────────────────────────────┘
```

- C++20. The engine owns every input, the GPU pipeline, every output and the show. The UI is only a
  client of its control API.
- Every camera's effects run on one Direct3D 11 device. Each output has its own thread and bounded
  queue, so a slow output drops its own frames instead of stalling the others.
- Each camera runs at its input's own size and frame rate. The project frame rate only sets the
  time budget for the effects.
- The engine presents the Multiview previews from its own D3D11 device straight into the window,
  under the WebView2 page that draws the labels and menus. A preview costs no readback, no encode
  and no network traffic, and the number of tiles is limited only by GPU memory. When that is not
  available (a plain browser, for example), previews fall back to H.264 over the control socket.
- The full design: [docs/superpowers/specs/2026-10-08-breadcamera-design.md](docs/superpowers/specs/2026-10-08-breadcamera-design.md).

## Licence

MIT, see [LICENSE](LICENSE). Bundled third-party components keep their own licences:
[installer/THIRD_PARTY_NOTICES.md](installer/THIRD_PARTY_NOTICES.md). The NVIDIA, NDI and
Blackmagic SDKs are not part of this repository.

BreadCamera is part of [BM Solutions](https://bmsolutions.vn), alongside BreadStream and BreadCall.
