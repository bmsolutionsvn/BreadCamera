# Bread OMT Screen: design

A sister app to Bread OMT Camera (`F:\Coding\BreadMedia\opencamera-omthx`). It sends an Android phone's screen and game sound to the production as an OMT-HX source.

## Why

Mobile game shows capture each player's phone with phone cast (screen mirroring). That makes the capture drop frames, and worse, it slows down the game the player is playing **(owner)**. This app replaces phone cast.

## Who and when

- The crew, at events with **2 phones now and up to 10 later** **(owner)**.
- Android first. iPhone is hard to install on and is out of scope **(owner)**.
- It works like Open Camera **(owner)**: install the app, open it, tap Go Live, accept Android's "Start sharing?" prompt, then switch to the game. The phone appears on the network as an OMT source.
- It must work with **any game**; nothing may depend on a particular title **(owner)**.

## Goals

1. The player feels no difference in the game. This matters more than picture quality.
2. A steady 60 fps feed.
3. **Game sound is sent too** **(owner)**, as standard OMT audio next to the picture.
4. The picture keeps **the phone's own shape and resolution**. Many phones are 21:9, so the app must not force 1920×1080 **(owner)**.

## Non-goals

- iPhone. Recording on the phone. Camera features. Plain OMT (VMX) output: VMX is a CPU encode at 43–130 Mbps, which would slow the game.

## Anti-goals (it fails even if it "works")

- The game stutters or heats the phone more than phone cast did.
- A source that quietly shows a black picture or plays silence, with nothing on the phone to say so.
- Room noise in the feed: the app never switches to the microphone on its own.
- Notifications or chat pop-ups appearing on stream.

## How the feed reaches vMix

Phone (OMT-HX: HEVC, or H.264 when the phone has no HEVC encoder) → **BreadCamera or OMT-Bridge** on the PC → vMix, as plain OMT or NDI. vMix cannot decode OMT-HX itself. This is the same path Bread OMT Camera's HX mode uses today. OMT-Bridge already carries OMT-HX audio.

## What it does (decided)

**Capture (Android MediaProjection).**
- **Consent:** each Go Live asks Android's consent once. Android 14+ cannot skip or remember it, and a consent token is single-use.
- **App sharing:** on Android 14+ the player can share **just the game app**. The status bar, notifications and other apps then never reach the stream. On Android 10–13 the whole screen is shared, and the app says so on the Go Live screen.
- **Lifetime:** one `VirtualDisplay` per Go Live, kept for the whole session. The app never needs a second consent until the crew taps Stop. Android 14+ requires a `MediaProjection.Callback` registered before `createVirtualDisplay`; `onStop` (the user ending sharing from the system UI) ends the session like Stop.
- **Identity (my calls):** app name **Bread OMT Screen**, applicationId `io.github.bachtam2001.breadomtscreen`. The applicationId cannot change once published, so the owner checks it before any store release.
- **Rotation:** the feed follows the shared window's size, using `onCapturedContentResize` on Android 14+ and display changes before that. A size change restarts the encoder at the new size with a fresh keyframe. Receivers already accept a size change.

**Encode only while someone watches (like OMT-HX passive encode).**
- With no viewer, the virtual display has no surface, so nothing is composited, encoded or sent. Only the network announcement stays up.
- The first viewer starts the encoder and the sound capture; the first frame is a keyframe.
- 5 s after the last viewer leaves, both stop **(my call: the same 5 s hold as the remote-link design)**.

**Picture.**
- The screen renders straight into the hardware encoder's input surface. The app never touches the pixels, which is the cheapest path a phone has. Reuse `OMTHXEncoder`: CBR, a 1 s keyframe interval, SPS/PPS on every keyframe, realtime priority, `max-fps-to-encoder`.
- Size is the phone's own size. When the encoder cannot do that at the chosen rate, the app scales down evenly (same shape, even dimensions) until it can. The phone shows the size and rate actually being sent.
- Rate: 60 fps default, 30 available **(my call)**. The encoder repeats the last frame when the screen is still, so a paused game is still a live 60 fps source.
- Quality: Low / Medium / High at 0.06 / 0.10 / 0.15 bits per pixel per frame, the same as Bread OMT Camera. Medium at 2400×1080 60 fps is about 15.5 Mbps.

**Sound.**
- **Capture:** Android 10+ playback capture (`AudioPlaybackCaptureConfiguration`) of USAGE_GAME, USAGE_MEDIA and USAGE_UNKNOWN, at 48 kHz stereo. It is sent as OMT `FPA1` (32-bit float planar) audio, timestamped on the same monotonic clock as the video so vMix sees them in sync.
- **Games that block it:** a game may block capture. Android then gives silence and no error. The app shows a **live sound meter** on its screen and in its notification, so the crew can see whether sound is flowing **(my call)**.
- **No microphone:** there is no microphone input at all in v1.
- **New libomt work:** the Android libomt port does not send audio yet (`omt_send_getaudiostatistics` says "Audio not implemented yet"). Adding audio send to audio-subscribed channels is new work in `libomt/cpp`.

**On the phone (my calls).**
- **Screens:** one screen with the source name (editable; default `<device model> (Screen)`, following `OMTStreamName`'s rule that the name has a matched `(` `)`), Quality, Rate and a big Go Live / Stop button. While live it shows sent size and rate, viewer count, sound meter and Wi-Fi/network name.
- **Background:** a foreground service (types `mediaProjection`) keeps it running behind the game. Its notification shows Live / viewers / sound and has Stop.
- **Overlay:** none over the game.

**Network.** OMT over TCP on any interface: the venue router (5 GHz or Wi-Fi 6) or a USB-C Ethernet adapter with charging pass-through. It announces by DNS-SD (`_omt._tcp`), the same as Bread OMT Camera. Ten phones at Medium is roughly 150–300 Mbps in total. A setup note in the README says so.

## Constraints

- **Repo:** a new local git repo `F:\Coding\BreadMedia\bread-omt-screen`, created by the plan. There is no GitHub remote; the owner adds one. Never push.
- **Code it starts from:** `libomt/` (MIT) and the OMT-HX pieces of `opencamera-omthx`: `OMTHXEncoder`, `OMTHXFormat`, `OMTSender`, `OMTStreamName`, `OMTBridge.cpp` and its CMake. Those Java/JNI files live in a GPL-3.0 app, so this app is **GPL-3.0**. No Open Camera camera code.
- **Android:** minSdk 29 (playback capture needs 29), targetSdk and compileSdk 36. ABIs arm64-v8a and x86_64 (emulator), as in Bread OMT Camera. Build with this PC's SDK (`ANDROID_HOME=F:\Android\SDK`, NDK 27.3) via the Gradle wrapper.
- **Wire:** exactly what Bread OMT Camera's HX mode sends, plus FPA1 audio. Nothing new on the wire, so BreadCamera and OMT-Bridge need no change.
- **Permissions:** `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_MEDIA_PROJECTION`, `RECORD_AUDIO` (required for playback capture), `POST_NOTIFICATIONS`, `INTERNET`, `ACCESS_NETWORK_STATE`, `CHANGE_WIFI_MULTICAST_STATE` (DNS-SD).

## Testing

- **Night run, no phone attached:** JVM unit tests for the pure parts (size fit and scaling, bitrate, source name, audio interleaved→planar and timestamps), `assembleDebug` and lint, plus a host test of libomt's audio send against a receiver if the existing `qa/` harness runs on this PC.
- **Owner on a real phone** (written as a checklist in the README):
  - Go Live, with the source showing in BreadCamera / OMT-Bridge.
  - Picture at native shape, 60 fps, in a real game, with no felt lag.
  - Sound in sync.
  - A game that blocks sound shows a flat meter.
  - Rotation.
  - App-only sharing on Android 14+ hides notifications.
  - No viewer means no encode (the phone shows "idle").
  - Stop and Go Live again.

## Left to the builder

- Layout and styling of the one screen.
- How the size-fit search steps.
- The audio frame size (for example 20 ms).
- How the foreground service and the activity talk.
- Whether the libomt port needs libvmx at all for an HX-only sender (drop it if not).
