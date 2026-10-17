# DirectShow native-format capture — OBS Virtual Camera

Date: 2026-10-16 · build PC `BACHTAM2001-LAP` · OBS Virtual Camera live (OBS log
"Virtual Camera Start"). This is the hardware acceptance behind
`docs/superpowers/specs/2026-10-16-dshow-format-negotiation-design.md`.

## Symptom

Selecting OBS Virtual Camera as a **Device camera** reported `capture-failed`
("The camera could not be opened. …: the device stopped sending frames") and retried forever.

## Root cause

`DShowCaptureInput` forced the `SampleGrabber` to `MEDIASUBTYPE_RGB32`. OBS Virtual Camera's
DirectShow filter connects in that mode but never pushes a sample; it streams only in its native
`NV12`. Confirmed with a standalone probe (a plain `capture → SampleGrabber → NullRenderer` graph;
the engine stopped so the probe was the sole consumer), 3 s per device:

| Requested subtype | HD WebCam | OBS Virtual Camera |
|---|---|---|
| native | 89 frames (`MJPG`) | **180 frames (`NV12`)** |
| RGB32 | 89 | **0 — `SampleCB` never called** |
| RGB24 | 89 | 0 |
| YUY2 | 29 | `RenderStream` fails `0x80040209` (not supported) |

A second finding: once frames flowed, a graph with a reference clock delivered **one** sample then
stalled (the known "SampleCB fires once" OBS behaviour); `IMediaFilter::SetSyncSource(nullptr)`
keeps it flowing.

## Fix

- `src/io/dshow_format.cpp` converts the negotiated format (RGB32/RGB24/NV12/YUY2/UYVY) to BGRA8
  via `swscale`, with `dshowRowBytes` giving the per-subtype row stride (NV12 uses the luma stride,
  not the 1.5× frame size).
- `src/io/dshow_capture_input.cpp` negotiates the source's native format, retries **once** forcing
  RGB32 for a format it cannot convert (MJPEG and friends), and runs the graph clock-free.

## End-to-end result

`tests/dshow_capture_test.cpp` "a raw-format device delivers frames" records
`[dshow-native] OBS Virtual Camera: 15 frame(s)` (was `0`), and the operator sees OBS Virtual
Camera reach `live` with a picture in the Add-camera preview.
