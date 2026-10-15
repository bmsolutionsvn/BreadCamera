# M1 real end-to-end — the engine binary, input → effect → OMT out

A genuine run of the shipped `breadcamera-engine.exe`: a real SRT input, real effects on the camera, and a real OMT
output, **read back by an independent OMT receiver** (`../OMT-HX/build/release/omthx-recv.exe`). This is the engine-exe
layer of the "real end-to-end with an effect" work; the in-process SRT-in → effect → SRT-out acceptance is
`tests/e2e_real_effects_test.cpp`.

The rig is `build\release\bc_soak.exe` (the M5 soak tool, run for one minute): it launches the real engine through
`ProcessLauncher`/`Supervisor`, writes a four-camera show (SRT listener inputs on 47971–47974, a `color` stage, a
`matte` stage and a `background` stage, plus an SRT output, an `omt` output and a `ndi`/`fullscreen` output), pushes a
test clip into every input, and reads the SRT outputs back. Each camera is named `Soak 1..4`, so its OMT source is
`<host> (BreadCamera - Soak <n>)` (the default `"BreadCamera - " + camera name`).

- Machine: `BACHTAM2001-LAP` · GPU: NVIDIA GeForce GTX 1650 · date 2026-10-16.

## Build — the `BC_WITH_OMT=ON` configure line

```
cmake --preset release        # scripts/build.ps1; Ninja, RelWithDebInfo, vcpkg x64-windows
cmake --build --preset release
```

`BC_WITH_OMT` is not a cache option in `CMakeLists.txt` (89–99); it is derived: when
`../OMT-HX/include/omthx/omthx.h` and `../OMT-HX/third_party/libomt/libomt.lib` both exist, CMake does
`add_subdirectory(${OMTHX_ROOT} omthx-build)` and sets `BC_WITH_OMT ON` (else `OFF`). The build therefore reports
`BC_WITH_OMT=ON`, and the artifact confirms it:

- `build/release/build.ninja` carries `-DBC_WITH_OMT=1` (172 compile entries), e.g.
  `DEFINES = -DBC_WITH_AJA=0 -DBC_WITH_DECKLINK=0 -DBC_WITH_LIVECALL=1 -DBC_WITH_NDI=0 -DBC_WITH_OMT=1 …`.
- `libomt.dll` (6.9 MB) and `libvmx.dll` are copied beside the executables by `bc_copy_runtime`.

## Run — the receiver sees real frames

The engine is discovered at `%TEMP%\bcam-soak-<pid>\engine.json`
(`{"pid":…,"port":47970,"token":"…"}`), so the same control WebSocket the tool uses can be opened for state.

```
build\release\bc_soak.exe --minutes 1
..\OMT-HX\build\release\omthx-recv.exe --list
..\OMT-HX\build\release\omthx-recv.exe --source "BACHTAM2001-LAP (BreadCamera - Soak 1)" --seconds 15 --dump %TEMP%\bcam-omt-soak1.bmp
```

`omthx-recv --list` finds all four outputs by discovery:

```
BACHTAM2001-LAP (BreadCamera - Soak 1)
BACHTAM2001-LAP (BreadCamera - Soak 2)
BACHTAM2001-LAP (BreadCamera - Soak 3)
BACHTAM2001-LAP (BreadCamera - Soak 4)
```

The receiver output (default soak configuration, one line per second; the first eight of fifteen seconds):

```
HX   hevc (d3d11va)     3 fps    10.41 Mbps  decode 27.47 ms  discarded 20  delay avg 1792093582003.50 max 1792093582058.63 ms  1920x1080
HX   hevc (d3d11va)     2 fps    10.27 Mbps  decode 16.66 ms  discarded 41  delay avg 1792093581905.83 max 1792093581911.47 ms  1920x1080
HX   hevc (d3d11va)     3 fps    10.02 Mbps  decode 24.07 ms  discarded 62  delay avg 1792093581911.60 max 1792093581917.41 ms  1920x1080
HX   hevc (d3d11va)     2 fps    10.36 Mbps  decode 27.98 ms  discarded 83  delay avg 1792093581915.57 max 1792093581916.68 ms  1920x1080
HX   hevc (d3d11va)     2 fps     9.17 Mbps  decode 21.05 ms  discarded 103 delay avg 1792093581912.16 max 1792093581918.89 ms  1920x1080
HX   hevc (d3d11va)     2 fps     7.82 Mbps  decode 25.62 ms  discarded 126 delay avg 1792093581911.77 max 1792093581912.44 ms  1920x1080
HX   hevc (d3d11va)     2 fps    10.08 Mbps  decode 28.48 ms  discarded 147 delay avg 1792093581908.34 max 1792093581911.41 ms  1920x1080
HX   hevc (d3d11va)     2 fps     8.58 Mbps  decode 24.24 ms  discarded 169 delay avg 1792093581908.27 max 1792093581912.29 ms  1920x1080
```

Real `hevc` access units decode over the real OMT transport at ~9–10 Mbps, 1920x1080, **no alpha** (no `+alpha`
suffix — the output is opaque BGRA). `--dump` wrote a full decoded frame: `bcam-omt-soak1.bmp` is 8 294 454 bytes =
1920 × 1080 × 4 + 54 (BMP header). The `delay avg` values are ~1.79e12 ms — the tool states delay is only meaningful
when the sender and receiver share a synchronised clock, so they are ignored here.

**Why the "fps" is a few per second.** The receiver counts `discarded` frames separately (link.cpp: `discard()`), and
it re-waits for a keyframe whenever `isGap()` sees an inter-frame step larger than 1.5 × the frame interval derived
from the **advertised** frame rate. The OMT output advertises the show rate (`omt_output.cpp` sets `fpsN/fpsD` from the
show's rate and stamps each frame from its tick at that rate, so 60/1 → a 16.67 ms step), while the default soak
configuration only delivers ~23 fps (`drops`/camera `fps` in the engine stats, and overruns of ~350 per 10 s). Dropped
ticks make the timestamp step 2–3 frame intervals (~33–50 ms), which looks like a gap against a 60 fps header, and the
receiver decodes the keyframe it re-requests (`deliver()` asks for one on a drop) and drops the rest: ~24 frames arrive
per second (`fps` + `discarded`), 2–3 of them are decoded to a picture. The frames that do arrive are real and full
size, as the BMP proves.

## Throughput confirmation — the same receiver at pipeline rate

A second one-minute soak run, lightened **through the real control API only** (no product code changed) so the engine
can serve the OMT output at its own rate: disable the heavy `matte` stage on every camera, drop the show to 1280x720,
and disable the four SRT outputs plus `fs0`, leaving each camera's OMT output as its only working output (the `ndi`
output stays offline in this build). The same receiver then matches the pipeline:

```
HX   hevc (d3d11va)    61 fps     7.19 Mbps  decode 16.93 ms  discarded 89  delay avg 1792093895239.25 max 1792093895254.23 ms  1280x720
HX   hevc (d3d11va)    60 fps     6.01 Mbps  decode 15.23 ms  discarded 89  delay avg 1792093895246.01 max 1792093895271.22 ms  1280x720
HX   hevc (d3d11va)    60 fps     5.07 Mbps  decode 19.18 ms  discarded 89  delay avg 1792093895241.31 max 1792093895260.35 ms  1280x720
HX   hevc (d3d11va)    59 fps     6.47 Mbps  decode 25.25 ms  discarded 89  delay avg 1792093895240.73 max 1792093895262.63 ms  1280x720
HX   hevc (d3d11va)    59 fps     5.83 Mbps  decode 22.15 ms  discarded 90  delay avg 1792093895244.95 max 1792093895260.70 ms  1280x720
HX   hevc (d3d11va)    60 fps     7.27 Mbps  decode 12.39 ms  discarded 90  delay avg 1792093895239.78 max 1792093895255.75 ms  1280x720
HX   hevc (d3d11va)    59 fps     5.86 Mbps  decode 12.25 ms  discarded 90  delay avg 1792093895242.59 max 1792093895266.33 ms  1280x720
HX   hevc (d3d11va)    60 fps     5.99 Mbps  decode 15.69 ms  discarded 90  delay avg 1792093895239.48 max 1792093895257.22 ms  1280x720
HX   hevc (d3d11va)    61 fps     5.05 Mbps  decode 12.62 ms  discarded 90  delay avg 1792093895242.16 max 1792093895264.92 ms  1280x720
HX   hevc (d3d11va)    59 fps     7.03 Mbps  decode 16.38 ms  discarded 90  delay avg 1792093895246.68 max 1792093895275.84 ms  1280x720
HX   hevc (d3d11va)    61 fps     6.75 Mbps  decode 22.35 ms  discarded 90  delay avg 1792093895243.33 max 1792093895262.87 ms  1280x720
HX   hevc (d3d11va)    60 fps     6.22 Mbps  decode 12.59 ms  discarded 90  delay avg 1792093895237.35 max 1792093895252.79 ms  1280x720
HX   hevc (d3d11va)    60 fps     5.00 Mbps  decode 21.75 ms  discarded 90  delay avg 1792093895244.09 max 1792093895261.77 ms  1280x720
HX   hevc (d3d11va)    59 fps     5.84 Mbps  decode 13.85 ms  discarded 90  delay avg 1792093895243.15 max 1792093895265.00 ms  1280x720
HX   hevc (d3d11va)    61 fps     6.10 Mbps  decode  6.79 ms  discarded 90  delay avg 1792093895241.40 max 1792093895259.96 ms  1280x720
```

(15 consecutive seconds; the first five lines of the capture ramped through 18, 43, 53, 30 and 46 fps as the decoder
started and the show reached its rate, and are omitted here.)

59–61 fps at 1280x720, `discarded` flat after the first second, and `--dump` wrote a 3 686 454-byte BMP
(1280 × 720 × 4 + 54). The engine's own overruns fell from ~350 per 10 s to 2–6 per 10 s in the same window, which is
what lets the receiver run gap-free. The overrun cost is dominated by the four SRT (H.264) outputs, not the matte stage:
a run with `matte` disabled but the SRT outputs still enabled still showed ~350 overruns per 10 s. The gap behaviour
above is a receiver-side property of the advertised-vs-delivered frame rate, not a broken OMT output.

## Engine and camera/stage state

Read from the control WebSocket (`ws://127.0.0.1:<port>/?token=<token>`, the port/token from the discovery file) with
the same `{"id":N,"cmd":…,"args":{…}}` protocol the soak tool uses; the reply is `{"id":N,"ok":true,…}` and the
snapshot arrives as a `{"type":"snapshot","state":…}` message.

Default configuration:

```
engine: gpu=NVIDIA GeForce GTX 1650  matteBackend=onnx-directml  rate=60/1  resolution=1920x1080
soak0..3: phase=live  input=live  color=active  background=active  matte=bypassed (stage-overrun: "took 15.05 ms per tick; the budget is 9.999 ms")  omt=live  srt=live
```

The `color` stage (exposure 0.1) and the `background` stage (blur) are `active` on every camera; `matte` is bypassed by
the engine's own per-tick budget guard, so the effect that reaches the OMT output in this configuration is the colour
exposure and the blurred background. The `ndi` output reports
`{"code":"output-config","message":"NDI support is not installed in this build"}` and is `offline` (expected, the build
has `BC_WITH_NDI=0`).

Lightened configuration:

```
engine: gpu=NVIDIA GeForce GTX 1650  matteBackend=onnx-directml  rate=60/1  resolution=1280x720  overruns=211
soak0..3: phase=live  input=live /receiving  color=active  background=active  matte=off  omt=live
```

Commands issued (over the control WebSocket; the JSON was base64-encoded so PowerShell could not rewrite its quotes):

```
{"cmd":"setStage","args":{"cameraId":"soak0..3","stage":"matte","params":{"enabled":false}}}
{"cmd":"setSettings","args":{"resolution":"1280x720"}}
{"cmd":"updateOutput","args":{"cameraId":"soak0..3","id":"srt0..3","enabled":false}}
{"cmd":"updateOutput","args":{"cameraId":"soak0","id":"fs0","enabled":false}}
```

## Stop

`bc_soak` shuts the engine down itself (`engine.request("shutdown")`, then `launcher.kill()` if it does not exit) and
removes its temp show directory; every run ended with no `breadcamera-engine.exe` left behind. The default run ends
`FAIL` on the soak's own criteria (`tick overruns (+~350) at t=20..60 s`), which is the soak's performance gate, not
this end-to-end check: the input → effect → OMT-output path is live throughout and the receiver reads real frames from
it. The lightened run ends `FAIL` on `liveness: 0 frames across the 4 SRT readers` because its SRT outputs were
deliberately disabled for that run; the OMT path is the subject there.

## Record

- 2026-10-16 · `BACHTAM2001-LAP` · GTX 1650 · `BC_WITH_OMT=ON` — default soak run: OMT source
  `BACHTAM2001-LAP (BreadCamera - Soak 1)` received real HEVC 1920x1080 ~9–10 Mbps, ~24 frames/s arriving (2–3 decoded,
  gap recovery), BMP dumped; every camera `phase=live`, `color`/`background` `active`. Lightened run: 59–61 fps at
  1280x720, gap-free.
- Not captured: a delivery-rate-matched header (the show rate is fixed at 50/1 or 60/1, so the receiver's gap detector
  cannot be satisfied at the default ~23 fps); audio over OMT (only video was received).