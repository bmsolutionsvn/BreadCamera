# OMT copy paths: the input leak, HX output on the GPU, HX input as NV12 — Design

Date: 2026-11-03. Status: implemented; measured 2026-11-03
(`docs/superpowers/2026-11-03-copy-path-audit.md`, "Measured after the fixes"), plan
`docs/superpowers/plans/2026-11-03-omt-copy-paths.md`.

Background and measurements: `docs/superpowers/2026-11-03-copy-path-audit.md` (findings 1, 2 and 4).

## 1. Why

On the build PC (RX 6600), with 4 cameras at 1080p30:

- **Every OMT input leaks about one BGRA picture per received frame** (~240 MB/s at 1080p30), for plain OMT
  (VMX) and HX sources alike. A show with one OMT camera runs out of memory in about a minute.
- **An OMT HX output costs about 2.3 CPU threads** (+56.7 % machine CPU for 4). Each frame goes GPU → CPU (the
  tick's readback) → CPU BGRA→NV12 in OMT-HX → GPU encoder.
- **An OMT HX input decodes on the GPU, then comes down to the CPU and back up as 4-byte BGRA**: +9 % CPU and
  +14 % 3D for 4 inputs, compared with SRT inputs.

## 2. Where the leak is (what is known)

- OMT-HX and the original libomt alone do not leak. A ctypes probe reading through OMT-HX's `omthx.dll` stays
  flat for plain OMT and HX sources, with and without the per-frame zero-timeout audio poll BreadCamera
  makes. `omthx-recv` (rebuilt from current OMT-HX source) stays at 76 MB.
- BreadCamera's engine loads a byte-identical `libomt.dll`. A plain-OMT source, which never touches FFmpeg,
  still leaks there.
- So the leak is in BreadCamera's code around the OMT receiver: `OmtInput::run` (`src/io/omt_input.cpp`), its
  per-frame texture upload, or how the engine consumes that input. The exact line is not pinned down. Finding it
  is plan task 1, with a test that fails first.

## 3. Goals, non-goals, anti-goals

Goals
- The OMT input's memory stays flat while receiving (a test proves it).
- An OMT **HX** output without alpha sends from the GPU: no tick readback for it, no CPU colour conversion, the
  encoder reading the picture in video memory.
- An OMT **HX** input reaches the camera as NV12 and converts on the GPU. That means no CPU colour conversion and
  1.5 bytes a pixel uploaded instead of 4.

Non-goals
- **Plain OMT (Full, VMX) is not changed**, input or output. It must stay byte-for-byte the standard Open Media
  Transport that vMix and other OMT tools send and receive. (A later option for the Full output, a UYVY
  readback, changes only how the picture reaches libomt, not the wire. It is not in this design.)
- The HX wire format does not change either. Only how BreadCamera feeds and reads OMT-HX changes.
- Sharing BreadCamera's D3D11 device with OMT-HX's decoder (true zero copy on input). That is a later OMT-HX API
  change.
- SRT, DeckLink, AJA, NDI and vcam copy paths (audit, later).

Anti-goals
- A colour shift on any OMT path. Check it with a solid-colour round trip.
- A keyed (alpha) OMT HX output that loses its alpha. It keeps the CPU path.
- A tick that waits for an encoder.

## 4. Design

### 4.1 Leak (BreadCamera)

Reproduce it in a test first, then fix the root cause there (no workaround such as a periodic receiver
restart). The test, in its final form: receive ≥ 300 frames of a 1920×1080 source through `OmtInput` into the
real `Camera` path, and assert that private bytes grow by less than 150 MB between frame 60 and frame 300.

### 4.2 HX output from the GPU

- **OMT-HX** (`src/d3d11.cpp`, `BgraToNv12::Impl::convert`): `Sender::sendVideoD3D11` also accepts
  `DXGI_FORMAT_R8G8B8A8_UNORM` (and its `_SRGB` variant), with `B8G8R8A8` still accepted. It checks the format
  with `ID3D11VideoProcessorEnumerator::CheckVideoProcessorFormat` (input support) and throws
  `std::runtime_error("the video processor cannot read <format>")` when the device cannot. The header comment on
  `sendVideoD3D11` and the C API comment say "BGRA or RGBA". OMT-HX is the owner's library, so this is a normal
  change there.
- **BreadCamera:**
  - `Output` gains `virtual bool wantsGpuRgba8() const { return false; }`. `OutFrame` gains
    `TextureRef rgba8;` — the frame as `R8G8B8A8_UNORM` in true RGBA order (`Converter::toBgra8(frame, w, h,
    /*swapRb=*/false)`), made once per tick per camera on the tick thread, only when some output wants it.
  - `OmthOutput` takes the output's `keyed` flag. Its GPU path is on when mode is Hx, it is not keyed, and the
    GPU path has not been refused. While on, `needsCpu()` is false and `wantsGpuRgba8()` is true, so a camera
    whose only CPU consumer was this output stops reading back.
  - `deliver`: on the GPU path, `send_->sendVideoD3D11(format, f.rgba8->tex.Get(), ts)`. An odd width or height,
    or a throw from it, switches this output to the CPU path for good: an atomic flag, plus a one-line state
    warning `/outputs/<id>/warning = "OMT HX is sending through memory: <reason>"`. The frame that threw is
    dropped. From the next tick, `needsCpu()` is true again.
  - Threads: `deliver` runs on the output's own thread. Do **not** hold `GpuDevice::lock()` across
    `sendVideoD3D11`: an encode can take milliseconds and would stall the tick. The device's immediate context is
    multithread-protected (BreadCamera's device and OMT-HX both ensure it). The video processor blit and the
    encoder's input copy do not change the compute bindings the tick's sequences rely on. If the tests or the
    acceptance show corruption or a device-removed error, the builder stops and records it (do not
    "fix" it by taking the lock without measuring the tick).
  - The `TextureRef` in `OutFrame` keeps the texture out of the pool until `deliver` returns, which is all OMT-HX
    needs ("the texture can be reused once this returns").

### 4.3 HX input as NV12

- `OmtInput` opens its picture receiver as `PixelFormat::Nv12`. On the first picture,
  `receiver->stats().mode == Mode::Full` (a plain OMT source) reopens it as `PixelFormat::Bgra` and keeps today's
  BGRA path for that source, so plain OMT stays as it is (and keeps its 4:2:2 chroma and alpha). The
  existing coded-receiver path for recorders is unchanged.
- Upload: two pooled textures, `R8_UNORM` w×h (Y) and `R8G8_UNORM` w/2×h/2 (UV), each filled with
  `UpdateSubresource` from `v.nv12Y` / `v.nv12UV` (row stride = width, as OMT-HX documents) under the GPU lock.
  They are wrapped in one `GpuTexture` whose `srvLuma`/`srvChroma` are the two textures' SRVs and whose
  `width`/`height` are the picture's. A helper in `src/gpu/` makes the pair and keeps both pooled refs alive for
  the wrapper's lifetime. The frame goes out as `PixelLayout::NV12`, `fullRange = true`.
- Colour: OMT-HX's NV12 is full-range **BT.709 at every size**. `Converter::toLinear` picks the matrix by height
  (`yuvToRgbMatrix`: BT.601 below 720). `VideoFrameIn` gains `bool bt709 = false`. `OmtInput` sets it, and the
  camera passes it to a new `toLinear(src, layout, fullRange, bool forceBt709)` overload. Every other input
  keeps today's rule.
- An HX source with alpha loses its alpha on this path (OMT-HX's NV12 receiver has none). If BreadCamera's
  camera pipeline uses an input's alpha anywhere, the builder keeps `PixelFormat::Bgra` for an HX source whose
  first frame has `format.alpha`, and says so in a `Ruling:`.

## 5. Verification

- OMT-HX: a new test mirroring "sendVideoD3D11" in `tests/link_test.cpp`, with an `R8G8B8A8` texture: white
  left half, red right half; decoded white > 240 and red within the same bounds as the existing test; encoder
  name contains `(d3d11)`. The existing BGRA test stays green.
- BreadCamera:
  - The leak test (§4.1): fails before the fix, passes after.
  - `OmthOutput`: `needsCpu()`/`wantsGpuRgba8()` for Hx, Full and keyed. An HX output fed `OutFrame`s with a solid
    `rgba8` texture is received by an `omthx::Receiver` with the right colour (±8 per channel).
  - The camera does not read back when its only CPU-capable output is a GPU-path HX output.
  - `OmtInput`: an HX source arrives as `PixelLayout::NV12` and converts to the sent colour (±8). A plain OMT
    source arrives as BGRA, as before.
- Measurement: rerun the audit's P1 (4 HX outputs) and P3 (4 HX inputs) cases. Record CPU, GPU and private memory
  at the start and end of a 30 s window, next to the audit's numbers.

## 6. Calls made (owner to check)

- Plain OMT is untouched in both directions, for compatibility with vMix and every other OMT tool.
- OMT-HX learns RGBA textures, instead of BreadCamera rendering a BGRA copy. It is a smaller change, and
  OMT-HX's other users get it too.
- No `GpuDevice::lock()` around `sendVideoD3D11`. It relies on D3D11 multithread protection, checked by the tests
  and the acceptance.
- HX inputs use NV12 now. Sharing the device with OMT-HX's decoder (no copy at all) is later.
- A GPU-path failure falls back to the CPU path for that output, with a visible warning. It never goes black.
