# BreadCamera — DirectShow camera format negotiation — Design

Date: 2026-10-16

## Intent

`DShowCaptureInput` (the DirectShow-only camera path added for OBS Virtual
Camera, `docs/superpowers/specs/2026-10-16-dshow-camera-input-design.md`) does
not work for OBS Virtual Camera: the camera connects, then reports
`capture-failed` / "the device stopped sending frames" forever. Physical
cameras work. Make DirectShow-only virtual cameras actually deliver frames,
without regressing physical cameras.

## Root cause (measured)

`src/io/dshow_capture_input.cpp:177-181` forces the `SampleGrabber` to
`MEDIASUBTYPE_RGB32` before `RenderStream`. OBS Virtual Camera's DirectShow
filter accepts an RGB32 connection but **never pushes a sample in that mode**;
it only streams in its native **NV12**.

An independent probe (a standalone DirectShow program, OBS Virtual Camera live,
the engine stopped so it was the sole consumer) captured for 3 s per device:

| Requested subtype | HD WebCam (physical) | OBS Virtual Camera |
|---|---|---|
| native (no forcing) | 89 frames (negotiated MJPG) | **180 frames (negotiated NV12)** |
| RGB32 | 89 frames | **0 frames — `SampleCB` never called** |
| RGB24 | 89 frames | 0 frames |
| YUY2 | 29 frames | `RenderStream` fails `VFW_E_CANNOT_CONNECT` (0x80040209) |

So no single forced subtype suits both a normal webcam (wants RGB32/RGB24/YUY2)
and OBS Virtual Camera (only NV12 streams). Forcing RGB32 is the defect.

## Measured connected media types

The same probe, with native negotiation (`formattype` = `FORMAT_VideoInfo`
(`05589f80`) and a non-null `pbFormat` for every device):

| Device | subtype | width×height | `biSizeImage` | `biBitCount` | delivered bytes/frame |
|---|---|---|---|---|---|
| HD WebCam | `MJPG` (`47504a4d`) | 1280×720 | 2764800 | 24 | ≈199 067 (compressed; `biSizeImage` is a nominal max) |
| OBS Virtual Camera | `NV12` (`3231564e`) | 1920×1080 | 3110400 | 12 | 3 110 400 exactly (`w*h*1.5`; stride 1920, no padding) |
| NDI Webcam (idle) | `YUY2` (`32595559`) | 1920×1080 | 4147200 | 16 | none (no source); `w*h*2`, stride 3840 |

So for the plain RGB/YUV formats `biSizeImage == width*bytesPerPixel*height` and
the delivered sample length matches it — no row padding observed. `biHeight` is
positive for RGB and YUV alike. (`FORMAT_VideoInfo2` was not seen; the code
accepts both.)

## Goals

- OBS Virtual Camera (and other DirectShow-only virtual cameras whose native
  format is a plain YUV format) captures frames into the existing `InputSink`
  as BGRA8.
- Physical cameras that worked before keep working.
- A device that truly sends nothing still reports `offline` with code
  `capture-failed` and retries — never a hang (unchanged contract).

## Non-goals

- Resolution / frame-rate selection for DirectShow devices (the device default
  is still used).
- Audio capture.
- Decoding compressed formats (MJPEG/MJPG). Those are handled by fallback
  (D3), not by a decoder.
- Replacing Media Foundation as the primary path for physical cameras.
- Selecting the format through `IAMStreamConfig` on the source.

## Decisions (and why)

| # | Decision | Why |
|---|---|---|
| D1 | **Negotiate the source's native format** — do not set the `SampleGrabber` media type; let `RenderStream` connect the source's default, then read `GetConnectedMediaType` for subtype, size and stride. | The forced type is the bug: OBS VCam connects in RGB32 but does not stream. Native negotiation makes it stream (NV12). |
| D2 | **Convert the connected subtype to BGRA8 in code.** Support at least `RGB32`, `RGB24`, `NV12` and `YUY2` (add `UYVY` if trivial). | These are the formats the observed devices negotiate; the converter must not depend on a codec. |
| D3 | **Any native subtype outside the supported raw set — compressed or unknown — falls back to the previous behaviour:** tear the graph down and rebuild forcing `MEDIASUBTYPE_RGB32`. If that also delivers nothing, report `capture-failed` and retry. | Physical webcams negotiate MJPG natively but all stream RGB32; making the fallback cover the whole unsupported class (not only MJPG) keeps the code one branch and covers future virtual cameras with other formats. |
| D4 | **RGB row order is `biHeight > 0` ⇒ bottom-up; YUV (`NV12`/`YUY2`) is top-down regardless of the sign.** | The existing `bottomUp_ = hRaw > 0` rule is a DIB convention; applying it to YUV would flip every OBS VCam frame vertically. |
| D5 | **The conversion is a pure function** (`convertToBgra8(subtype, src, width, height, stride, bottomUp) -> bytes`), unit-tested with synthetic frames, hardware-free. | The graph itself needs a device; the pixel maths does not, and it is the part most likely to be wrong. |

## Constraints

- Windows only; C++20, MSVC, `/W4 /permissive-`.
- `start()`/`stop()` never block for long; inputs never block the tick.
- A missing/busy/unplugged device still reports `offline`, code
  `capture-failed`, and retries with backoff.
- Same `InputSink` contract, BGRA8 texture, as `MfCaptureInput`.
- UK English copy; conventional commits (`fix(io):`).

## Risks / open follow-ups

- **Row order for YUV.** Resolved (D4): RGB is bottom-up when `biHeight > 0`;
  YUV is top-down.
- **Stride.** Resolved (measured): for RGB/YUV, `biSizeImage` equals the packed
  size and the delivered sample matches it; use `biSizeImage / height` and keep
  the short-sample guard. MJPG never reaches the converter (D3).
- **Chroma siting / range** for NV12 (Rec.601 vs Rec.709, limited vs full).
  The colour is inspected by eye; a small matrix correction can follow if
  needed.
- **A filter that advertises a format it does not stream** is still possible;
  D1 removes the known case (OBS VCam RGB32) but not the class. `capture-failed`
  + retry remains the safety net.
- The MJPG fallback only helps a device that also supports RGB32; a
  compressed-only device stays unsupported (acceptable; none observed).

## Left to the builder

- The exact sample-subtype constants and the conversion maths.
- Where the fallback decision lives (inside `runOnce`, or a small retry in
  `run`).
- The exact new error strings, if any.
