# BreadCamera: a lean GPU path, and pure pass-through when nothing changes — Design

Date: 2026-11-03. Status: description approved by the owner in chat (2026-11-03). Project 3 of the BreadMedia
data-path backlog (`docs/superpowers/2026-11-03-data-path-backlog.md`, items 4, 5, 6, 10). Builds on the OMT
copy-paths work (`BreadCamera/docs/superpowers/specs/2026-11-03-omt-copy-paths-design.md`) and projects 1–2.

## 1. Why

BreadCamera is also a gateway. The owner's example, not yet run in a show: guests from BreadCall come in, and go out
on DeckLink or AJA with no effects. On that route each frame is decoded in software, converted to BGRA on the CPU,
uploaded, read back as BGRA, copied into a newly allocated DeckLink frame, looped over to force alpha, and converted
back to YUV by the card. That is about five CPU passes and two PCIe crossings per frame, almost all of it colour
conversion back and forth.

Some routes cannot be pure pass-through even without effects. BreadCall → SDI changes the codec (H.264 →
uncompressed), the size (WebRTC changes guest resolution mid-call; SDI is fixed) and the timing (jittery arrival vs
the card's clock). Those need the GPU and the engine tick, done lean. Routes where nothing changes need neither.

## 2. Goals, non-goals, anti-goals

Goals
- **Lean GPU path** (any route that changes codec, size or timing): decode on the GPU where possible, keep YUV until
  the GPU, and leave the GPU as UYVY at 2 bytes a pixel. No per-pixel CPU work on the way in or out.
- **Pure pass-through** (nothing changes): coded network → network of the same family, and SDI UYVY → SDI, NDI or
  plain OMT in the same mode, without touching the GPU except for a visible preview.
- Every input uploads with the leak-safe upload (DYNAMIC + `Map(WRITE_DISCARD)`, as the OMT fix did).

Non-goals
- Changing what effects do, or the engine tick for cameras with effects.
- SRT output's GPU encode (NV12 into D3D11 hardware frames). It is a later item, like the OMT HX output.
- 10-bit (v210) I/O.

Anti-goals
- A black frame, or a frame from the wrong camera, when a camera switches between the pass-through and GPU paths
  (an effect turned on, or the input changing size).
- A colour shift on SDI. UYVY must match today's `bgraToUyvy`/`uyvyToBgra` rules: BT.709 at 720 lines and
  above, BT.601 below, limited range.
- A keyed (fill + key) output losing its key. Keyed outputs keep the BGRA path.

## 3. Design

### 3.1 Lean GPU path

1. **Shared UYVY readback** (owner-approved backlog item 5):
   - The camera makes UYVY on the GPU (a compute shader packing two pixels into one `R8G8B8A8` texel: U, Y0, V, Y1)
     at the frame size, and reads it back asynchronously with the existing `ReadbackSlot` machinery.
   - `Output` gains `virtual bool wantsUyvy() const { return false; }` and `OutFrame` gains
     `std::shared_ptr<const CpuImage> uyvy`. `CpuImage` gains a layout tag.
   - The camera reads back BGRA only for outputs that still need it. Both readbacks happen only while both kinds of
     output are present.
2. **Outputs moving to UYVY** (unkeyed):
   - DeckLink: `bmdFormat8BitYUV`; frames from a small reused pool, not one allocation per tick; no alpha loop.
   - AJA: take UYVY directly, with no `bgraToUyvy`.
   - Plain OMT (VMX): `sendVideoUyvy`, which libomt takes with no conversion. The wire is unchanged.
   - NDI: `NDIlib_FourCC_type_UYVY`.

   Before switching OMT Full, measure how much of its cost is conversion and how much is VMX encode (the owner's
   condition), and check with a vMix receiver.
3. **UYVY inputs** (DeckLink, AJA, NDI when it delivers UYVY): upload the UYVY as-is (2 B/px) into a `w/2 × h`
   `R8G8B8A8` texture with `Map(WRITE_DISCARD)`, then unpack and convert on the GPU (`PixelLayout::UYVY`, a new
   `Converter` path, limited range, matrix by height as today). No `uyvyToBgra` on the CPU.
4. **BreadCall input:** decode H.264 on the GPU (D3D11VA, the way `FfmpegInput` already does) and copy the decoder's
   NV12 surface on the GPU (`CopySubresourceRegion`). If hardware decoding is not available, upload the decoded
   planes (I420 → NV12 by interleaving U/V on the CPU, which is cheap) with the NV12 plane upload from the OMT work.
5. **Every other `UpdateSubresource`-per-frame input** (DirectShow, Media Foundation, software-decoded FFmpeg,
   `Converter::uploadBgra8`) moves to the leak-safe upload. Keep native YUV where the source has it.
6. **Leftover CPU converters** (`uyvy.cpp`, the AJA resize) reuse their swscale contexts instead of making one per
   frame.

### 3.2 Pure pass-through

- **When:**
  - the camera has no enabled stages;
  - the output's size and rate equal the input's;
  - the output is not keyed;
  - the pair is in the table below.

  Otherwise the camera runs the GPU path.
- **Pairs:**

  | Input | Output | How |
  |---|---|---|
  | OMT (Full or HX) | OMT of the same mode | coded frames: `Receiver(Coded)` → `Sender::sendCoded`, with keyframe requests relayed |
  | SRT | SRT | the input's packets re-muxed, with no decode |
  | DeckLink/AJA UYVY | DeckLink/AJA in the same mode, NDI (UYVY), plain OMT (`sendVideoUyvy`) | the input's UYVY buffer handed on in RAM |

- **Timing:**
  - Network outputs take frames as they come.
  - SDI → SDI across two cards needs frame sync for clock drift: repeat or drop on a one-slot buffer, the same rule
    as the engine's `FrameSync`, in RAM.
- **Signal loss:** the last frame is held in RAM. The slate is rendered once to the output's format and reused.
- **Switching paths** (an effect turned on, an input size change): the switch happens at a tick boundary, and the
  last frame is repeated until the new path delivers. Never black.
- **Previews:**
  - On the coded path, decode only while a preview is visible (OMT-HX `FrameDecoder` from project 2, or FFmpeg).
  - On the UYVY path, upload UYVY only while a preview is visible.

## 4. Phases (each a plan)

1. The UYVY readback plus DeckLink, AJA and NDI outputs on UYVY, the DeckLink frame pool, and the swscale context
   reuse (§3.1 items 1, 2 except OMT Full, and 6).
2. Measure OMT Full's conversion vs encode split; move OMT Full to UYVY if it pays (§3.1 item 2).
3. UYVY inputs and the leak-safe upload for every input (§3.1 items 3, 5).
4. BreadCall GPU decode (§3.1 item 4).
5. Pure pass-through (§3.2), as three plans: 5a coded OMT → OMT, 5b SDI UYVY in RAM, 5c SRT → SRT remux.

Plans (2026-11-03): `BreadCamera/docs/superpowers/plans/2026-11-03-lean-uyvy-outputs.md`, `…-lean-omt-full-uyvy.md`,
`…-lean-uyvy-inputs.md`, `…-lean-breadcall-gpu-decode.md`, `…-lean-pass-through-omt.md`, `…-lean-pass-through-uyvy.md`,
`…-lean-pass-through-srt.md`.

Corrections made while planning:
- DeckLink and AJA run at a fixed 1920×1080, so they take UYVY only for a 1920×1080 frame (`wantsUyvy(w, h)`); other
  sizes keep the BGRA path and its CPU scaling, so the UYVY path never needs a matrix change.
- The Maxine and ONNX mattes also write a texture per frame with `UpdateSubresource`; phase 3 moves them too.
- Pass-through runs per output, not per camera: outputs that cannot pass through keep the tick path beside them.

## 5. Verification

- Colour: solid-colour round trips through each new path match today's within ±8 per channel, for 720p and
  1080p (BT.709) and 576 lines (BT.601).
- Leaks: the OMT leak test's method (private memory flat over 300 frames) for each input moved to the new upload.
- Pass-through:
  - an OMT → OMT camera with no preview never decodes (decoder stats);
  - turning an effect on mid-stream gives no black frame (a frame-by-frame check);
  - an SDI → SDI loop holds sync over 10 minutes (no growth in repeats or drops beyond clock drift).
- Measured like the audit: engine CPU, GPU and memory per route, before and after. The owner's BreadCall → DeckLink
  route is part of acceptance once it runs.

## 6. Calls made (owner to check)

- The UYVY packing is a compute shader into `R8G8B8A8` texels, read back with the existing async readback.
- I420 from a software BreadCall decode is interleaved to NV12 on the CPU (half a pass), rather than adding an
  I420 GPU path.
- SRT output's GPU encode is left for a later item.
- Pass-through repeats the last frame during a path switch. A one-frame repeat is accepted; black is not.
