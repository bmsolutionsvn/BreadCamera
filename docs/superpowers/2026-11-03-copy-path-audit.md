# Copy-path audit: where frames cross between GPU and CPU (2026-11-03)

Started from OMT-HX, as the owner asked. Measured on the build PC (RX 6600, 16 threads), BreadCamera `main`
at cbb361a, engine only (no UI), 4 cameras at 1080p30, 30 s per case. CPU is a share of the whole machine.
GPU is the engine process's summed GPU-engine utilisation.

## Measured

| Case | Engine CPU | Engine GPU | Memory |
|---|---|---|---|
| P0: 4 SRT inputs (H.264, hardware decode), no outputs | 14.4 % | 25.4 % (codec 14.5, 3D 10.9) | 860 MB, flat |
| P1: P0 + one OMT **HX** (HEVC) output per camera | **71.1 %** (+56.7) | 70.5 % (+45: codec +27, 3D +18) | flat |
| P2: P0 + one OMT **Full** (VMX) output per camera | 42.7 % (+28.3) | 36.5 % (+11, 3D) | flat |
| P3: 4 OMT HX inputs (omthx-send, HEVC), no outputs | 23.4 % (+9 vs P0) | 54.8 % (+29: codec +15, 3D +14) | **9 GB and growing** |

Raw output: `scratchpad/omt-bench-result.txt` of the session that ran it. The script is reproduced from the
same session's `omt-bench.ps1` (show files generated per case, SRT pushers via ffmpeg lavfi, OMT sources via
`omthx-send`).

## Findings, worst first

### 1. OMT input leaks one BGRA frame per received frame (bug, critical)

- One OMT input, 1080p30: private memory grows **~240 MB/s** (1.1 GB → 3.1 GB in 8 s; 9 GB after 20 s).
  240 MB/s = 30 × 8.3 MB, exactly one 1920×1080 BGRA picture per frame.
- The same for every source type: Full (VMX), HX H.264 and HX HEVC. So it is not the HX decoder.
- OMT-HX's own `omthx-recv` (rebuilt from current OMT-HX source, same `PixelFormat::Bgra`) stays at 76 MB
  working set / 113 MB private. So the leak appears only inside BreadCamera's engine: in `OmtInput::run`
  (`src/io/omt_input.cpp`) or in how omthx behaves when built into BreadCamera (static, BreadCamera's FFmpeg).
  Differences from `omthx-recv` worth checking first: a new `omthx::VideoFrame` per loop instead of one reused
  frame; the `pool_.acquire` + `UpdateSubresource` upload per frame; the `receiveAudio` drain.
- A show with one OMT input would run the PC out of memory in about a minute. Fix before any optimisation.

### 2. OMT HX output: GPU → CPU → GPU round trip, ~2.3 threads per output

Path today: camera texture → `toBgra8` → staging copy + `Map` (the tick's readback) → CPU BGRA →
`omthx::Sender::sendVideo(bgra)` → OMT-HX packs BGRA→NV12 on the CPU (swscale) → FFmpeg uploads to
`hevc_amf` → encode.

- Cost: **+56.7 % machine CPU for 4 outputs (≈ 2.3 of 16 threads each)**, plus 3D work for the readback.
- OMT-HX already has the fix: `Sender::sendVideoD3D11(format, texture, ts)` converts BGRA→NV12 on the
  texture's own device with the video processor and encodes it in place (`src/d3d11.cpp`, `src/link.cpp`).
- Gaps to close:
  - It only accepts `B8G8R8A8` textures. BreadCamera's `toBgra8` writes `R8G8B8A8` holding swapped bytes.
    Either OMT-HX accepts `R8G8B8A8_UNORM` (the video processor takes it on current drivers) and BreadCamera
    passes true RGBA (`swapRb = false`), or BreadCamera renders a `B8G8R8A8` copy.
  - No alpha: keyed OMT outputs keep the CPU path.
  - `QueuedOutput` runs `deliver` on its own thread. OMT-HX makes the device multithread-protected and uses
    FFmpeg's D3D11 lock, not BreadCamera's `GpuDevice::lock()`. The hand-off must not interleave with the tick's
    state-set + dispatch sequences. Pass BreadCamera's lock into the FFmpeg device context, or hold it around the
    call and measure the tick.
  - `OutFrame` carries only the CPU image today. It needs the camera's texture, and the camera must stop
    forcing the readback when no remaining output needs CPU.

### 3. OMT Full (VMX) output: readback is needed, but not at 4 bytes a pixel

VMX is a CPU codec, so the picture must come to memory. Today it comes as BGRA (4 B/px), and libomt converts it
to VMX's 4:2:2 on the CPU. **+28.3 % CPU for 4.** Producing UYVY on the GPU before the readback halves the
copied bytes and removes the CPU colour conversion. `sendVideoUyvy` then hands it to VMX with no copy.

### 4. OMT HX input: decode on the GPU, then out and back in

Path: d3d11va decode on **OMT-HX's own D3D11 device** (`Decoder`, `av_hwdevice_ctx_create(nullptr)`) → download
NV12 → CPU swscale NV12→BGRA → BreadCamera uploads BGRA (4 B/px) → `toLinear`. +9 % CPU and +14 % 3D vs SRT for 4
inputs (HEVC decode itself costs more than H.264, which is part of the +15 codec).

- Contrast: BreadCamera's SRT input with hardware decode copies the decoder's NV12 texture straight into its
  own texture (`CopySubresourceRegion`, `src/io/ffmpeg_input.cpp` `emitVideo`) and converts on the GPU. That is
  the model.
- Quick fix, no OMT-HX change: `PixelFormat::Nv12` receiver → upload NV12 (1.5 B/px) → `Converter::toLinear(NV12)`.
  It removes the CPU conversion and 60 % of the upload. Alpha is dropped on that path.
- Full fix, OMT-HX change: let `Receiver` take the app's `ID3D11Device` so `PixelFormat::Gpu` pictures live on
  BreadCamera's device. Then it is a GPU copy, as for SRT.

## The same anti-pattern elsewhere in BreadCamera (from code, not measured yet)

- **Every CPU output forces the camera's full-frame readback** (`toBgra8` + staging copy, 8.3 MB at 1080p per
  camera per tick, `src/engine/camera.cpp`), and then converts again on the CPU:
  - SRT output: BGRA → YUV with swscale, then FFmpeg uploads to the hardware H.264 encoder
    (`src/io/srt_output.cpp`). This is the same round trip as finding 2. Fix: an NV12 texture into D3D11 hardware
    frames.
  - DeckLink and AJA: BGRA → UYVY with swscale (`decklink_output.cpp`, `aja_output.cpp`). Fix: produce UYVY (or
    v210) on the GPU and read back 2 B/px.
  - vcam: swscale per frame (`vcam_output.cpp`). NDI: sends BGRA, and the NDI SDK converts again internally.
- **Inputs that upload BGRA from the CPU:** software-decoded FFmpeg/SRT frames, BreadCall (WebRTC), DirectShow,
  Media Foundation, NDI, DeckLink, AJA. Uploading the native YUV (NV12/UYVY) and converting on the GPU saves the
  CPU conversion and half or more of the upload. Hardware-decoded SRT already does this.

## Suggested order

1. Fix the OMT input leak (finding 1), with a test that receives 300 frames and checks that private memory
   stays within a bound.
2. OMT HX output on the GPU (finding 2). The biggest CPU win measured.
3. OMT HX input as NV12 (finding 4, quick fix), then the shared-device version in OMT-HX.
4. The same GPU-side YUV treatment for the SRT output and DeckLink/AJA (UYVY readback), measured the same way.
