# BreadCamera — Third-party notices

BreadCamera 0.1 — BM Solutions (<https://bmsolutions.vn>).

This file lists every third-party component that BreadCamera links, bundles or ships, with the
version used for this build, its licence, where to obtain it, and either the licence text or a
pointer to the text shipped beside this file.

Most libraries come from **vcpkg**, at the baseline recorded in `vcpkg.json`
(`builtin-baseline: 2750401336fb7c95f6619657a46a7e798661341c`). Versions below are the exact
versions vcpkg installed for this build (`build/release/vcpkg_installed/.../share/<port>/vcpkg.spdx.json`).
Components marked **"ships only when built"** are optional build features (CMake `BC_WITH_*`
options):

- optional SDKs default to ON only when the SDK is found on the build machine and silently turn
  themselves OFF otherwise;
- the binaries in this installer only contain the features that were ON for the release build;
- when such a component is *not* built, its section is retained so the licence obligation is
  recorded for builds that do include it.

---

## FFmpeg

- **Version:** 9.0.2#1 (vcpkg port `ffmpeg`, baseline `2750401336fb7c95f6619657a46a7e798661341c`)
- **Licence:** GNU Lesser General Public License v2.1 or later (LGPL-2.1+)
- **URL:** <https://ffmpeg.org/>
- **Shipped as:** `avcodec-*.dll`, `avformat-*.dll`, `avutil-*.dll`, `swscale-*.dll`,
  `swresample-*.dll` (dynamically linked), built with the LGPL features only
  (`avcodec`, `avformat`, `swresample`, `swscale`, plus the `srt`, `nvcodec` and `amf` features).
  No GPL parts (`--enable-gpl`, x264/x265, …) are enabled.

The full LGPL-2.1 text is the `COPYING.LGPLv2.1` section of FFmpeg's own `LICENSE.md`, and the
source-availability statement required by LGPL §6 is in `LGPL-source-offer.txt` next to this file:

> BreadCamera uses FFmpeg libraries under the GNU Lesser General Public License v2.1 or later,
> linked dynamically (avcodec-*.dll, avformat-*.dll, avutil-*.dll, swscale-*.dll, swresample-*.dll).
> The exact FFmpeg source used for this build is the vcpkg port at the baseline recorded in vcpkg.json.
> On request we provide the complete corresponding source code: contact bmsolutions.vn.
> You may replace these DLLs with your own compatible builds.

FFmpeg's own licence summary (`LICENSE.md`, MIT/X11/BSD bits combined into LGPL-2.1+) is shipped
in the vcpkg package:

    Most files in FFmpeg are under the GNU Lesser General Public License version 2.1
    or later (LGPL v2.1+). ... Some other files have MIT/X11/BSD-style licenses.
    In combination the LGPL v2.1+ applies to FFmpeg.

FFmpeg's optional features used here pull in two more permissive components, both linked in and
covered by the FFmpeg build above:

- **NVENC headers (`ffnvcodec`)**, version 13.1.15.0 — MIT — <https://github.com/FFmpeg/nv-codec-headers>
- **AMD AMF (`amd-amf`)**, version 1.5.3 — MIT — <https://github.com/GPUOpen-LibrariesAndSDKs/AMF>

## libsrt

- **Version:** 1.5.7 (vcpkg port `libsrt`)
- **Licence:** Mozilla Public License 2.0 (MPL-2.0)
- **URL:** <https://github.com/Haivision/srt>
- **Shipped as:** `srt.dll`

Full MPL-2.0 text is the `LICENSE` section of the vcpkg `libsrt` copyright file. MPL-2.0 requires
that if we modify libsrt we publish those file-level changes; **BreadCamera ships libsrt
unmodified**. The required notice is:

    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at http://mozilla.org/MPL/2.0/.

libsrt additionally contains the UDT BSD-3-Clause notice (University of Illinois) and public-domain
files, reproduced in the same vcpkg copyright file.

## ixwebsocket

- **Version:** 12.0.1 (vcpkg port `ixwebsocket`)
- **Licence:** BSD 3-Clause
- **URL:** <https://github.com/machinezone/IXWebSocket>
- **Shipped as:** statically linked into `BreadCamera.exe` / `breadcamera-engine.exe`

    Copyright (c) 2018 Machine Zone, Inc. All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.

    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.

    3. Neither the name of the copyright holder nor the names of its contributors
       may be used to endorse or promote products derived from this software
       without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.

## nlohmann-json

- **Version:** 3.12.0#2 (vcpkg port `nlohmann-json`)
- **Licence:** MIT
- **URL:** <https://github.com/nlohmann/json>
- **Shipped as:** statically linked into `BreadCamera.exe` / `breadcamera-engine.exe`

    MIT License

    Copyright (c) 2013-2025 Niels Lohmann

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## Windows Implementation Library (WIL)

- **Version:** 1.0.260126.7 (vcpkg port `wil`)
- **Licence:** MIT
- **URL:** <https://github.com/microsoft/wil>
- **Shipped as:** header-only, compiled into `BreadCamera.exe`

    MIT License

    Copyright (c) Microsoft Corporation. All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE

## Microsoft Edge WebView2 SDK

- **Version:** 1.0.4258.31 (vcpkg port `webview2`)
- **Licence:** Microsoft Software License Terms for the WebView2 SDK
- **URL:** <https://learn.microsoft.com/microsoft-edge/webview2/>
- **Shipped as:** `WebView2Loader.dll` (and headers at build time)

The SDK is redistributed under the Microsoft Software License Terms that accompany the WebView2
SDK (the `LICENSE.txt` in the SDK package). The WebView2 *Runtime* itself is a Microsoft
component installed on the end-user machine, not redistributed by BreadCamera. The vcpkg package's
`copyright` file carries Microsoft's BSD-3-Clause style notice for the loader:

    Copyright (C) Microsoft Corporation. All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

      * Redistributions of source code must retain the above copyright notice,
        this list of conditions and the following disclaimer.
      * Redistributions in binary form must reproduce the above copyright notice,
        this list of conditions and the following disclaimer in the documentation
        and/or other materials provided with the distribution.
      * The name of Microsoft Corporation, or the names of its contributors may not
        be used to endorse or promote products derived from this software without
        specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" ...

## NDI SDK runtime

- **Version:** NDI 6 SDK (runtime DLL `Processing.NDI.Lib.x64.dll`)
- **Licence:** NDI SDK licence
- **URL:** <https://ndi.video/>
- **Ships only when built:** `BC_WITH_NDI=ON` (an NDI 6 SDK found at `C:\Program Files\NDI\NDI 6 SDK`
  or `%NDI_SDK_DIR%`). Off for this build.

Redistribution of the NDI runtime is subject to the NDI SDK licence; the installer ships only the
runtime file the licence marks redistributable. Required attribution:

> **NDI® is a registered trademark of Vizrt NDI AB.**

## NVIDIA Video Effects SDK (Maxine) runtime and AI Green Screen model

- **Version:** NVIDIA Video Effects SDK (Maxine) runtime — the `nvvfx` proxy sources and the
  `nvvfxgreenscreen` feature model from the installed SDK
- **Licence:** NVIDIA Software License Agreement; Product-Specific Terms for NVIDIA AI Products;
  NVIDIA Community Model License
- **URL:** <https://developer.nvidia.com/nvidia-video-effects-sdk>
- **Ships only when built:** `BC_WITH_MAXINE=ON` (SDK at `%VFXSDK_ROOT%`). Off for this build.

Per the M0 licence review (spec §7.1, 2026-10-08): NGC states the Virtual Background features are
available for commercial/non-commercial use; downloads require a free NVIDIA Developer Program
(NGC) account. **Only the runtime files the SLA marks redistributable are shipped.** When enabled,
`nvVideoEffects.dll` and the AI Green Screen model files accompany the executables.

## OpenCV (core, imgproc, dnn, and the contrib `mcc` module)

- **Version:** 4.14.0 (vcpkg port `opencv4`, features `contrib`, `dnn`)
- **Licence:** Apache License 2.0
- **URL:** <https://opencv.org/>
- **Ships only when built:** chart detection (`BC_WITH_CHART_DETECT=ON`, i.e. the contrib `mcc`
  module found). When enabled, `opencv_core4.dll`, `opencv_imgproc4.dll`, `opencv_dnn4.dll`,
  `opencv_calib3d4.dll`, `opencv_features2d4.dll`, `opencv_flann4.dll` and `opencv_mcc4.dll`
  are shipped.

The full Apache-2.0 text is the `LICENSE` section of the vcpkg `opencv4` copyright file.

## Blackmagic DeckLink SDK

- **Version:** Blackmagic DeckLink SDK 16.0
- **Licence:** Blackmagic Design SDK End User License Agreement
- **URL:** <https://www.blackmagicdesign.com/developer/products/capture-and-playback>
- **Ships only when built:** `BC_WITH_DECKLINK=ON` (the SDK's `DeckLinkAPI.idl` found via
  `%DECKLINK_SDK_DIR%`). Off for this build — the SDK is not installed where CMake looks.

The EULA accompanies the SDK (`End User License Agreement.pdf` in the SDK root). Only the DLLs the
licence permits are redistributed; BreadCamera links the DeckLink API by COM (`CoCreateInstance`),
so the adapter code in `breadcamera-engine.exe` is our own and the Blackmagic runtime stays with
the user's Desktop Video installation.

## AJA libajantv2 (NTV2)

- **Version:** `ntv2_18_0_0` (fetched from GitHub by CMake `FetchContent`)
- **Licence:** MIT
- **URL:** <https://github.com/aja-video/libajantv2>
- **Ships only when built:** `BC_WITH_AJA=ON` (default OFF, experimental — see `CMakeLists.txt`).
  Off for this build.

Standard MIT licence text (Copyright (c) AJA Video Systems, Inc.); the full text ships in the
libajantv2 repository's `LICENSE` file.

## libomt / libvmx (Open Media Transport)

- **Version:** libomt `bb4b67be8e5964f1017592b9fc5406495e704e5d`, libomtnet
  `7d29031ff3f355ffa1f0cd08ee24d02e258445dd`, Open Media Transport binaries v1.0.0.19
  (pinned in `OMT-HX/scripts/build-libomt.ps1`)
- **Licence:** MIT (libomt, libomtnet and libvmx)
- **URL:** <https://github.com/openmediatransport/libomt>
- **Shipped as:** `libomt.dll`, `libvmx.dll`

    MIT License

    Copyright (c) 2025 Open Media Transport Contributors

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## VCamSample (Windows 11 virtual camera)

- **Version:** vendored from `github.com/smourier/VCamSample` at commit `58b6d4b`
- **Licence:** MIT
- **URL:** <https://github.com/smourier/VCamSample>
- **Shipped as:** `breadcamera-vcam.dll` (Windows 11 only; see the installer)

    MIT License

    Copyright (c) 2024-2026 Simon Mourier

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

(The full text is kept in `vcam/LICENSE` in the source tree.)

## MODNet portrait-matting model (M6)

- **Version:** `Xenova/modnet` at commit `fa2fa546052fba4c08921230a26cc69a333fca12`
  (`onnx/model.onnx`, Apache-2.0; pinned SHA-256
  `07c308cf0fc7e6e8b2065a12ed7fc07e1de8febb7dc7839d7b7f15dd66584df9`)
- **Licence:** Apache License 2.0
- **URL:** <https://huggingface.co/Xenova/modnet>
- **Ships only when built:** ONNX matte backend (`BC_WITH_ONNX=ON`, M6). Shipped as
  `models/modnet.onnx` when enabled.

Full Apache-2.0 text: <https://www.apache.org/licenses/LICENSE-2.0>.

## ONNX Runtime and DirectML (M6)

- **Versions:** ONNX Runtime `1.24.4` (NuGet `Microsoft.ML.OnnxRuntime.DirectML`), DirectML
  `1.15.4` (NuGet `Microsoft.AI.DirectML`)
- **Licence:** MIT (both)
- **URLs:** <https://github.com/microsoft/onnxruntime>, <https://github.com/microsoft/DirectML>
- **Ships only when built:** `BC_WITH_ONNX=ON` (M6). Shipped as `onnxruntime.dll` and
  `DirectML.dll` when enabled.

Standard MIT licence text (Copyright (c) Microsoft Corporation). Full texts:
<https://github.com/microsoft/onnxruntime/blob/main/LICENSE> and
<https://github.com/microsoft/DirectML/blob/master/LICENSE>.

---

## Bundled transitive dependencies

These vcpkg libraries are linked or shipped (`{#BuildDir}\*.dll`) but are not project-level
choices. Each is used unmodified, under a permissive licence:

| Component | Version (vcpkg) | Licence | URL |
|---|---|---|---|
| OpenSSL (`libcrypto-3-x64.dll`) | 3.6.5#1 | Apache-2.0 | <https://www.openssl.org/> |
| Protocol Buffers (`libprotobuf.dll`) | 6.33.4#2 | BSD-3-Clause | <https://protobuf.dev/> |
| zlib (`z.dll`) | 1.3.2#2 | Zlib | <https://zlib.net/> |
| Abseil (`abseil_dll.dll`) | 20260107.1#3 | Apache-2.0 | <https://abseil.io/> |

The licence texts for these are the corresponding sections of their vcpkg `copyright` files.