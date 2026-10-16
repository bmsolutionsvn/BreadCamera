# BreadCamera — DirectShow camera input (OBS Virtual Camera) Design

Date: 2026-10-16

## Intent

BreadCamera's "device camera" input must be able to see and capture from
**DirectShow-only virtual cameras**, beginning with **OBS Virtual Camera**.

Today `listCaptureDevices()` (`src/io/mf_capture_input.cpp`) uses only
`MFEnumDeviceSources` with `MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID`.
Media Foundation enumerates video capture devices through the kernel‑streaming /
PnP path plus Windows‑11 FrameServer virtual cameras — so physical USB webcams
and NDI Webcam appear, but **OBS Virtual Camera does not and cannot be opened**.
OBS Virtual Camera registers only as a DirectShow filter under
`CLSID_VideoInputDeviceCategory` (`{860BB310-5D01-11D0-BD3B-00A0C911CE86}`) and
exposes no PnP camera interface. Microsoft's docs and the OBS project both
confirm MF and DirectShow do not bridge here.

Evidence gathered on the build PC: the filter `{A3FCE0F5-3493-419F-958A-ABA1250EC20B}`
= "OBS Virtual Camera" is registered in the DirectShow video‑input category;
`Get-PnpDevice -Class Camera` lists only `HD WebCam`.

## Goals

- The device list shows every video capture source the OS offers, including
  DirectShow-only virtual cameras (OBS Virtual Camera), so it truly "shows all".
- No duplicates: a source visible to both MF and DirectShow appears **once**.
- A chosen DirectShow source captures video into the existing `InputSink`
  (BGRA8 texture) exactly as the MF path does.
- Rename the input-type label `USB or built-in camera` to `Device camera`, and
  adjust the copy that says `USB and built-in cameras are video only, no audio.`
  — the list is no longer USB/built-in only.

## Non-goals

- Audio capture from DirectShow devices (the MF path is video only too).
- Resolution / frame-rate selection for DirectShow devices in v1 (the device
  default is used; the MF path keeps its own selection).
- Replacing the MF path. MF stays primary for physical cameras and FrameServer
  virtual cameras.
- Rebuilding FFmpeg with `avdevice`. The vcpkg FFmpeg in this project has no
  `libavdevice`, so its `dshow` input device is unavailable; enabling it would
  force every builder and CI to rebuild FFmpeg. Not taken.

## Decisions (and why)

- **Enumeration:** DirectShow `ICreateDevEnum` +
  `CLSID_VideoInputDeviceCategory`; read `FriendlyName` and `DevicePath` from each
  moniker's property bag. New file `src/io/dshow_devices.{h,cpp}`.
- **List id scheme:** MF ids are unchanged (the MF symbolic link), so existing
  shows keep working. A DirectShow-only id is `dshow:<moniker display name>`
  (`IMoniker::GetDisplayName`) — stable across device renames, unlike the friendly
  name — with the friendly name carried for display. **My call**, from the review:
  the display name is the durable identifier, so a renamed OBS device does not
  orphan a saved show.
- **Merge & dedupe:** the MF list stays first; a DirectShow device is skipped when
  its moniker `DevicePath` (case-insensitive) matches an MF id, or — as the
  fallback for software filters that expose no `DevicePath` — when its friendly
  name matches an MF name. DirectShow also wraps physical cameras, so without this
  the list would double. The merge is a **pure function**
  (`mergeCaptureDevices(mf, dshow)`) unit-testable without hardware. **My call**,
  from the review: key on `DevicePath` first (it *is* the KS symbolic link for
  physical cameras) so the result does not depend on the two stacks agreeing on a
  display string.
- **Routing:** the factory routes a `uvc` device id beginning with `dshow:` to the
  DirectShow input; every other id stays on `MfCaptureInput`.
- **Capture:** a DirectShow filter graph through the in-box **`SampleGrabber`**
  filter (`CLSID_SampleGrabber`) fixed to `MEDIASUBTYPE_RGB32`. `SampleGrabber` is
  deprecated but still shipped (`qedit.h` was removed from the SDK, so the
  interface and GUIDs are declared locally). **My call:** it is by far the smallest
  robust path; a hand-written `IMemInputPin` sink filter is the fallback if
  `SampleGrabber` is ever withdrawn.
- **Frame hand-off:** a `SampleGrabber` sample is valid **only during the
  callback**, so the callback `memcpy`s it into a single-slot owned buffer
  (drop-to-newest) under a mutex and signals a condition variable; the input's own
  thread copies that slot into a BGRA8 texture, stride/flip-aware, exactly where
  `MfCaptureInput` uploads. **My call**, from the review: the `MfReaderCallback`
  pattern does not transfer verbatim (no ref-counted sample to retain), so one
  buffer copy is explicit; it stays inside the frame budget and keeps GPU work off
  the DirectShow streaming thread, so `stop()` stays prompt.
- **Threading:** one thread per input; the graph, capture filter, grabber and null
  renderer are all created, run and torn down on that thread under MTA.

## Constraints

- Windows only; C++20, MSVC, `/W4 /permissive-`.
- New link deps: `quartz` and `strmiids` (Windows SDK DirectShow). `ole32` /
  `oleaut32` already linked.
- `start()`/`stop()` never block for long (spec §4.1); inputs never block the tick.
- A missing, busy or unplugged device reports `offline` with code `capture-failed`
  and retries with backoff — same contract as `MfCaptureInput`.
- UK English error copy; conventional commits (`feat(io):`).

## Risks / open follow-ups

- `SampleGrabber` is legacy; if it disappears, replace with a custom sink filter.
- No resolution/frame-rate negotiation for DirectShow devices in v1.
- Two DirectShow devices sharing a friendly name resolve to the first.
- Graph teardown deadlock: the graph must be stopped then aborted on the input
  thread before the thread joins.

## Left to the builder

- The exact SampleGrabber interface / GUID declarations.
- The precise graph build order and error strings.
