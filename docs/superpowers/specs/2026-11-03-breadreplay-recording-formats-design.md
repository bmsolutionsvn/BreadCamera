# BreadReplay: record compressed sources as they arrive, pictures as VMX — Design

Date: 2026-11-03. Status: description approved by the owner in chat (2026-11-03). Backlog item 8
(`docs/superpowers/2026-11-03-data-path-backlog.md`).

## 1. Why

BreadReplay runs on two kinds of weak setup, sometimes both at once:
- **an HDD**, which cannot take many VMX streams (VMX at 1080p30 is about 43–130 Mbps per input);
- **a weak PC**, which cannot run many encodes.

Today every non-OMT source is decoded and VMX-encoded on the CPU (`Recorder`, 2 encoder threads per input), even when
it arrived already compressed (OMT-HX, SRT). GPU encoders are not the answer for everything either: GeForce cards
allow 8 encode sessions per PC whatever the card (RTX 3060, 5070 and 5090 alike; NVENC Application Note, SDK 13), and
AMD's encoder handles about 4–5 1080p60 streams on the build PC (owner's observation). BreadMedia does not have the
NDI Advanced SDK, so NDI arrives as decoded pictures.

## 2. The rule (the owner's)

**Compressed sources are stored exactly as they arrive. Picture sources are encoded to VMX.**

| Input | Arrives as | Stored as | Work while recording |
|---|---|---|---|
| SDI (DeckLink/AJA) | pictures (UYVY) | VMX | CPU VMX encode |
| NDI, full and HX (standard SDK) | pictures (UYVY) | VMX | CPU VMX encode |
| Plain OMT | VMX | VMX, as received (today's `pushCoded`) | none |
| OMT-HX | HEVC/H.264 access units | HEVC/H.264, as received | none |
| SRT | H.264/HEVC in MPEG-TS | H.264/HEVC, remuxed | none |
| SRT with another codec (MPEG-2, …) | coded | VMX | decode + CPU VMX encode |

Recording uses **no GPU encoder session** by default. Nothing is decoded and re-encoded unless the operator asks.

Per-input override, in the input's settings:
- **Auto** (default): the rule above.
- **VMX:** for a compressed source with long keyframe spacing that operators scrub a lot. Decode + CPU encode.
- **HEVC (GPU):** for a picture source when the CPU or disk cannot take VMX. Uses one encoder session.

Before recording, the show is checked against two estimates and warns if it does not fit (it never blocks):
- **CPU:** VMX encodes × the measured per-frame encode time (`Recorder` stats);
- **disk:** the sum of the inputs' measured bitrates vs the drive's measured write rate (`WriteRateMeter`).

## 3. Goals, non-goals, anti-goals

Goals
- Compressed sources record with no decode and no encode, at their own (small) size.
- Scrubbing, slow motion, reverse and frame-accurate cueing work on every input, whatever it is stored as.
- Each input's tile says how it scrubs: "instant" (VMX), or "seek ≈ N ms" (long-GOP, measured).

Non-goals
- The NDI Advanced SDK (compressed NDI). NDI stays a picture source.
- Changing VMX recording, or the store's segment and retention model, beyond what the new codecs need.
- Transcoding stored material afterwards.

Anti-goals
- A cue or a take that lands on the wrong frame, or shows garbage, because a long-GOP frame was decoded without its
  keyframe.
- Reverse or slow motion that stutters on long-GOP inputs at 1080p60 on the target GPUs.
- An HDD show that silently drops frames. The pre-record check warns, and the recorder's dropped-frame count stays
  visible.

## 4. Design

### 4.1 Store

- `SegmentHeader` gains `codec` (`vmx` | `h264` | `hevc`), as segment format version 2. Version 1 segments read as
  VMX.
- `FrameRecord::reserved` becomes `flags`, with bit 0 = keyframe. VMX frames are all keyframes.
- H.264/HEVC frames are stored as Annex-B access units. Parameter sets go in band on every keyframe (OMT-HX's
  encoder already prepends them, `encoder.cpp`). A remuxed SRT keyframe gets them from the stream's extradata when
  they are missing. `previewLen` is 0 for these codecs.
- A segment starts on a keyframe. A recorder that joins mid-GOP drops frames until the first keyframe, and asks an
  OMT-HX source for one (`requestKeyframe`).
- Audio stays float32 interleaved: OMT audio is stored as it is; SRT audio (AAC/MP2/Opus) is decoded once to float.

### 4.2 Recording

- OMT-HX input: a `Coded` receiver. HX frames go straight into the store (no decode). Plain OMT is unchanged.
- SRT input: demux only (no decode) when the video is H.264/HEVC. Timestamps are mapped onto the engine clock the
  same way decoded SRT frames are today.
- For OMT-HX sources, ask for a keyframe every 0.5 s (*builder's call on the mechanism:* the sender's keyframe
  policy, or periodic `requestKeyframe`). The spacing of other sources is whatever the sender uses.
- "HEVC (GPU)" override: the picture is encoded with OMT-HX's encoder settings (the same quality and keyframe
  spacing), and frames are stored as above.

### 4.3 Playback (`FrameSource`, `Renderer`)

- HEVC/H.264 frames are decoded on the GPU (D3D11VA) **on BreadReplay's own D3D11 device**, so the decoded NV12
  surface is used in place. `DecodedFrame` carries either today's CPU UYVY (VMX) or an NV12 texture. `Renderer`
  converts NV12 on the GPU (BreadCamera's `Converter` NV12 path).
- Seek: decode from the keyframe at or before the target, forward to the target.
- Cache: a per-input cache of decoded frames (GPU textures) around the playhead, sized to one GOP and capped by a
  VRAM budget (e.g. 64 frames per input, 1 GB total; builder's call). It serves reverse, slow motion and nearby
  cues without decoding again.
- Angle thumbnails (today VMX's DC-prefix preview, `vmxPreviewLength`): for HEVC/H.264, decode the nearest keyframe
  and downscale on the GPU, at most a few times a second per input.
- The tile's "seek ≈ N ms" is measured from real seeks and shown once known.

### 4.4 Data path

Follows the BreadMedia rules: no CPU colour conversion on the HEVC/H.264 path, the decoded picture never leaves the
GPU, and VMX stays as today.

## 5. Verification

- Store: write and read back v2 segments for each codec. v1 segments still play. Keyframe flags are right.
  Recovery after a crash mid-segment still works.
- Recording: an OMT-HX source and an H.264 SRT source record with zero decodes and zero encodes (counters). The
  stored bytes equal the received access units.
- Playback: frame-accurate cue on a long-GOP SRT recording (2 s GOP) matches a reference decode, frame by frame.
  Reverse and 0.25× slow motion at 1080p60 run without stutter on the RX 6600 (and the RTX 5070 when available).
- Mixed show: 4 SDI + 2 NDI + 2 OMT-HX + 2 SRT records and replays. The CPU, disk and session counts match §2's
  table (no encoder sessions used).
- The pre-record check warns on an HDD-sized write rate.

## 6. Order and dependencies

- After the BreadReplay UX work and the native-preview plan (`BreadCamera/docs/superpowers/plans/2026-11-02-native-video-previews-breadreplay.md`).
  Both touch `previewLoop`, `FrameSource` and the tiles.
- Uses OMT-HX's `Coded` receiver and the NV12 conversion from BreadCamera's lean-paths work.
- One plan, in two phases:
  1. store v2 + OMT-HX/SRT as-received recording + GPU-decode playback with seek and cache;
  2. the overrides, the pre-record check and the tile's scrub label.

## 7. Calls made (owner to check)

- 0.5 s keyframe spacing requested from OMT-HX sources.
- A decoded-frame cache of about one GOP per input, capped by VRAM.
- Angle thumbnails for compressed inputs come from keyframes, not every frame.
- The pre-record check warns and never blocks.
