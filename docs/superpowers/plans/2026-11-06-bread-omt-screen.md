# Bread OMT Screen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An Android app that sends the phone's screen (native shape, hardware HEVC/H.264) and game sound as an OMT-HX source, encoding only while someone watches.

**Architecture:** The streaming core is copied from Bread OMT Camera (`F:\Coding\BreadMedia\opencamera-omthx`): libomt, libvmx, the JNI bridge, `OMTSender`, `OMTHXEncoder`, `OMTHXFormat` and `OMTStreamName`. Pure Java helpers (size fit, viewer gate, audio packing, meter) are unit-tested on the JVM. libomt gains audio send. A foreground service owns MediaProjection → VirtualDisplay → encoder surface and AudioPlaybackCapture → `sendAudio`. One activity starts and stops it.

**Tech Stack:** Java 17 toolchain, AGP 9.4.1, Gradle wrapper 9.8.1, NDK 27.3.13750724, CMake 3.22.1, JUnit 4; no AndroidX.

**Spec:** `F:\Coding\BreadMedia\BreadCamera\docs\superpowers\specs\2026-11-06-bread-omt-screen-design.md`

Repo: `F:\Coding\BreadMedia\bread-omt-screen` (already a git repo with one commit on `main`: README stub, LICENSE, .gitignore). Branch `bos-first-version` from `main`, merged back with `--no-ff`. Never push; there is no remote. Read `opencamera-omthx` only; never change it.

## Global Constraints

- **Identity:** applicationId and namespace `io.github.bachtam2001.breadomtscreen`, app name `Bread OMT Screen`. Copied OMT classes go in package `io.github.bachtam2001.breadomtscreen.omt`. Rename every JNI symbol prefix `Java_net_sourceforge_opencamera_OMTSender_` to `Java_io_github_bachtam2001_breadomtscreen_omt_OMTSender_`.
- **SDK:** minSdk 29, targetSdk 36, compileSdk 36. ABIs `arm64-v8a`, `x86_64`. `ndkVersion "27.3.13750724"`, CMake `3.22.1`. Java toolchain 17. The SDK comes from `ANDROID_HOME` (`F:\Android\SDK`); do not commit `local.properties`.
- **License:** GPL-3.0, credited in README: Open Camera OMT by ysdede, libomt and libvmx (MIT) from Open Media Transport.
- **Wire:** exactly Bread OMT Camera's HX video (`sendCoded`), plus OMT audio frames.
  - Audio frame: FrameType 4, 24-byte audio extended header, Codec `FPA1` (0x31415046), SampleRate 48000, Channels 2, ActiveChannels `(1 << channels) - 1`, Reserved1 0, then each channel's samples in turn as 32-bit float LE.
  - Timestamps in 100 ns ticks on the same monotonic clock as video (`ptsNs / 100`).
  - Sent only to channels with `isAudioSubscribed()`.
- **Network:** port 6400 and DNS-SD `_omt._tcp`. The name must contain a matched `(` `)` (`OMTStreamName`); the default is `<Build.MODEL> (Screen)`.
- **Video:** follow `OMTHXEncoder` as copied: CBR, 1 s keyframes, SPS/PPS on every keyframe, realtime priority, `max-fps-to-encoder`. Add `KEY_REPEAT_PREVIOUS_FRAME_AFTER` = one frame period in µs, so a still screen keeps the rate. HEVC Main when the phone has an HEVC encoder, else H.264. Bitrate is `OMTHXFormat.bitrate(w, h, fps, quality, hevc)`, where quality is 1 / 50 / 100 for Low / Medium / High; the default is Medium.
- **Rate:** 60 fps by default; 30 is the other choice.
- **Passive:** while no viewer is connected, nothing is captured, composited or encoded (the VirtualDisplay surface is `null`). The first viewer starts the encoder and audio; capture stops 5 000 ms after the last viewer leaves. The announcement stays up the whole session.
- **Session:** one MediaProjection consent and one `createVirtualDisplay` per Go Live. Register the `MediaProjection.Callback` before `createVirtualDisplay`; its `onStop` ends the session like Stop. Size changes go through `VirtualDisplay.resize` plus an encoder restart, never a second display.
- **No microphone source:** `RECORD_AUDIO` is requested only because Android requires it for playback capture.
- **Copy (exact):**
  - Idle status: `Idle: waiting for a viewer`.
  - Live status: `Live: <w>×<h> <fps> fps · <n> viewer(s)`.
  - Under the meter: `No sound? Some games block sound capture.`
  - Android 10–13 note on the Go Live screen: `This Android version shares the whole screen: notifications will show on stream.`
  - Audio permission refused: `Sound is off: Android needs the Record audio permission to capture game sound.` The session still goes live, with no audio.
  - Notification title: `Bread OMT Screen`; text is the status line; one action `Stop`.
- **Build and test** (PowerShell, repo root):
  - `./gradlew.bat --no-daemon testDebugUnitTest` for the JVM tests;
  - `./gradlew.bat --no-daemon assembleDebug lintDebug` for the APK and lint;
  - one test class: `--tests "<fqcn>"`.

  Read `app/build/test-results/testDebugUnitTest/TEST-*.xml` for results, not the console. No phone or emulator is used by this plan; device checks are the owner's (Task 6 README).

## Review Focus

1. **The phone turns** (portrait menus, landscape game): the feed changes size with a fresh keyframe; the shape is kept and never letterboxed to the old size. → Task 2 test (portrait fit) + Task 4 resize path + README checklist.
2. **vMix reconnects** (viewer 1 → 0 → 1 within a second): the encoder is not restarted. → Task 2 `ViewerGate` test.
3. **The user ends sharing from Android's own UI** (status-bar chip, or the Android 14 "Stop sharing" button): the app tears down like Stop. The notification goes, the announcement goes, and the next Go Live asks consent again. → Task 4 (`onStop`) + README checklist.
4. **A game blocks sound:** the meter stays flat with the hint shown, and video is unaffected. → Task 2 meter test (silence → 0) + README checklist.
5. **Record audio refused:** the session still goes live, with the exact "Sound is off" copy. → Task 5 + README checklist.

---

### Task 1: The app skeleton builds with the copied streaming core

**Files:**
- Copy from `opencamera-omthx`, unchanged:
  - `gradlew`, `gradlew.bat`, `gradle/wrapper/*`;
  - `libomt/` (whole folder, including `libomt.h` and `cpp/`);
  - `libvmx/` (whole folder);
  - `app/src/main/cpp/{CMakeLists.txt,OMTBridge.cpp,omtnv12.h}`.
- Create, modelled on Open Camera's but minimal: `settings.gradle` (rootProject `bread-omt-screen`, `include ':app'`), `build.gradle`, `gradle.properties` (`android.useAndroidX=true`, nothing else needed), `app/build.gradle`, `app/src/main/AndroidManifest.xml`, `app/src/main/res/values/strings.xml` (`app_name`), `app/src/main/res/mipmap-anydpi/ic_launcher.xml` (any simple adaptive icon).
- Copy and re-package to `io.github.bachtam2001.breadomtscreen.omt`: `OMTSender.java`, `OMTHXEncoder.java`, `OMTHXFormat.java`, `OMTStreamName.java`; tests `OMTHXFormatTest.java`, `OMTStreamNameTest.java` under `app/src/test/java/io/github/bachtam2001/breadomtscreen/omt/`.
- Modify: `OMTBridge.cpp` (JNI prefixes only); `CMakeLists.txt` (`project("bread_omt_screen")`; paths still resolve to `<repo>/libomt` and `<repo>/libvmx`).

**Interfaces:**
- Produces:
  - `OMTSender` with `init(String name, int width, int height, int frameRate, int quality)`, `sendCoded(...)`, `getViewerCount()`, `cleanup()` and the constants `CODEC_HEVC` / `CODEC_H264`, as in the original;
  - `OMTHXEncoder(String mime, int codecTag, int width, int height, int fps, int bitrate)`;
  - `OMTHXFormat.bitrate(...)`;
  - `OMTStreamName.normalize(String)` with `DEFAULT_SUFFIX = " (Screen)"`.
- Replace `MyDebug.LOG` guards with plain `Log.d`.
- `app/build.gradle`:
  - `testImplementation 'junit:junit:4.13.2'`, `testOptions.unitTests.returnDefaultValues = true`;
  - the `externalNativeBuild` block as in Open Camera;
  - lint `abortOnError = false`;
  - no signing config (debug only).

- [ ] **Step 1: Write the failing test.** In `OMTStreamNameTest`, change the expectations from `" (Camera)"` to `" (Screen)"`. Add `assertEquals("Phone (Screen)", OMTStreamName.normalize("Phone"))`.
- [ ] **Step 2: Run** `./gradlew.bat --no-daemon testDebugUnitTest`. Expected: the build fails, because the copied classes don't exist yet. With them copied but the suffix unchanged, the new assertion fails.
- [ ] **Step 3: Implement**: copy, re-package, rename the JNI prefixes, set `DEFAULT_SUFFIX`. Empty `MainActivity` in `io.github.bachtam2001.breadomtscreen` (launcher) so `assembleDebug` has an app.
- [ ] **Step 4: Run** `testDebugUnitTest`: `OMTHXFormatTest` and `OMTStreamNameTest` pass. Run `assembleDebug`: `app/build/outputs/apk/debug/app-debug.apk` exists. Check that `libomt.so`, `libvmx.so` and the bridge library are in the APK for both ABIs (`unzip -l`).
- [ ] **Step 5: Commit**

```bash
git add .
git commit -m "feat: app skeleton with the OMT-HX streaming core from Bread OMT Camera"
```

---

### Task 2: Size fit, viewer gate, audio packing and meter (pure Java)

**Files:**
- Create in `app/src/main/java/io/github/bachtam2001/breadomtscreen/`: `SizeFit.java`, `ViewerGate.java`, `AudioPack.java`
- Test: `app/src/test/java/io/github/bachtam2001/breadomtscreen/{SizeFitTest,ViewerGateTest,AudioPackTest}.java`

**Interfaces (all free of Android imports):**
- `static int[] SizeFit.fit(int width, int height, int fps, OMTHXFormat.Limits limits)` returns `{w, h, fps}`, or `null` when nothing fits.
  - It keeps the requested fps and shrinks the size evenly: aspect ratio kept within 1 %, both sides even, never larger than the request.
  - It stops at the first size where `limits.maxFpsAt(w, h) >= fps`.
  - It gives up (null) below 320 px on the short side.
- `ViewerGate(long holdMs)` and `boolean update(int viewers, long nowMs)`.
  - It returns whether capture should be running: true while `viewers > 0`, and still true until `holdMs` after viewers last dropped to 0.
- `static float[] AudioPack.toPlanar(float[] interleaved, int framesPerChannel, int channels)`.
- `static float AudioPack.level(float[] samples, int count)`: peak absolute value clamped to 0..1, for the meter.
- `static long AudioPack.ptsNs(long firstFrameNs, long framesSoFar, int sampleRate)`: `firstFrameNs + framesSoFar * 1e9 / sampleRate`, as a long.

- [ ] **Step 1: Write the failing tests**
  - `SizeFitTest`:
    - native is supported: `fit(2400, 1080, 60, (w,h) -> 60)` gives `{2400,1080,60}`;
    - width capped at 1920: with `(w,h) -> w <= 1920 ? 60 : 0`, the result has `w <= 1920`, `|w/h - 2400/1080| / (2400/1080) < 0.01`, `w % 2 == 0`, `h % 2 == 0` and fps 60;
    - odd input: `fit(2401, 1081, 60, (w,h) -> 60)` gives even sides;
    - portrait stays portrait: `fit(1080, 2400, 60, (w,h) -> h <= 1920 ? 60 : 0)` gives `h > w`;
    - `(w,h) -> 0` gives `null`.
  - `ViewerGateTest` (hold 5000):
    - `update(0, 0)` is false;
    - `update(1, 100)` is true;
    - `update(0, 200)` is true; `update(0, 5199)` is true; `update(0, 5200)` is false;
    - reconnect inside the hold: `update(1, 6000)` true, `update(0, 6100)` true, `update(1, 6900)` true, `update(0, 7000)` true, `update(0, 11999)` true (the hold restarts from 7000).
  - `AudioPackTest`:
    - `toPlanar({1,2,3,4,5,6}, 3, 2)` gives `{1,3,5,2,4,6}`;
    - `level` of zeros is `0f`; `level({0.25f,-0.5f}, 2)` is `0.5f`; `level({2f}, 1)` is `1f`;
    - `ptsNs(1_000, 48_000, 48_000)` is `1_000_001_000`.
- [ ] **Step 2: Run** `testDebugUnitTest`. Expected: it fails to compile because the classes don't exist.
- [ ] **Step 3: Implement** the three classes.
- [ ] **Step 4: Run** `testDebugUnitTest`. Expected: all pass.
- [ ] **Step 5: Commit**

```bash
git add app/src
git commit -m "feat: size fit keeps the phone's shape; viewer gate holds 5 s; audio packing and meter"
```

---

### Task 3: libomt sends audio

**Files:**
- Create: `libomt/cpp/OMTAudioFrame.h`, header-only, with no Android or socket includes. Test: `qa/audio-frame.cpp`, a standalone host check, and `qa/run-audio-frame.ps1`.
- Modify: `libomt/cpp/OMTSender.h/.cpp` (`sendAudio`), `libomt/cpp/libomt_android.cpp` (`omt_send` audio branch), `app/src/main/cpp/OMTBridge.cpp` (`nativeSendAudio`), `OMTSender.java` (`sendAudio`).

**Interfaces:**
- In `OMTAudioFrame.h`: `std::vector<uint8_t> omt::buildAudioFrame(const float* planar, int channels, int samplesPerChannel, int sampleRate, int64_t timestamp100ns)`.
  - It returns FrameHeader (16 bytes, from `OMTProtocol.h`: Version 1, FrameType 4, Timestamp, MetadataLength 0, DataLength = 24 + payload) + `AudioExtHeader` (24 bytes, as in the Global Constraints) + payload (`channels * samplesPerChannel * 4` bytes).
  - It returns empty for channels outside 1..32, samplesPerChannel ≤ 0 or sampleRate ≤ 0.
- `int omt::Sender::sendAudio(const float* planar, int channels, int samplesPerChannel, int sampleRate, int64_t timestamp100ns)`.
  - It builds the frame once and queues it with `sendAsync` (header + ext header + payload, as video does) to every connected channel with `isAudioSubscribed()`.
  - It returns the payload bytes, or 0.
- `omt_send` handles `OMTFrameType_Audio` by calling `sendAudio` with the frame's `Data`, `Channels`, `SamplesPerChannel`, `SampleRate` and `Timestamp`.
- JNI `nativeSendAudio(float[] planar, int channels, int samplesPerChannel, int sampleRate, long ptsNs)` returns `jboolean`. It reads the array with `GetFloatArrayRegion` and passes `ptsNs / 100`.
- Java `public boolean sendAudio(float[] planar, int channels, int samplesPerChannel, int sampleRate, long ptsNs)`. It is false when not initialised or when `planar.length < channels * samplesPerChannel`.

- [ ] **Step 1: Write the failing test** `qa/audio-frame.cpp`, a `main()` with `assert`s, built against `libomt/cpp/OMTAudioFrame.h` and `OMTProtocol.h`. Use 2 channels, 3 samples, 48000 Hz, timestamp 12345, planar `{1,2,3,4,5,6}`. Check:
  - total size 16 + 24 + 24 = 64;
  - byte 0 == 1, byte 1 == 4;
  - Timestamp (int64 LE at offset 2) == 12345;
  - DataLength (int32 LE at offset 12) == 48;
  - the ext header at 16 reads Codec 0x31415046, SampleRate 48000, SamplesPerChannel 3, Channels 2, ActiveChannels 3, Reserved1 0;
  - the floats at 40.. equal 1..6 in order.

  Also `buildAudioFrame(..., 0 channels, ...)` is empty. `qa/run-audio-frame.ps1` compiles it with `C:\msys64\mingw64\bin\g++.exe -std=c++17 -I libomt/cpp` and runs it; exit 0 means pass.

  If `OMTProtocol.h` does not compile standalone, put the two packed structs the frame needs in `OMTAudioFrame.h` with `static_assert`s on their sizes (16, 24), and have `OMTProtocol.h` include them. Do not define them twice.
- [ ] **Step 2: Run** `powershell -NoProfile -ExecutionPolicy Bypass -File qa/run-audio-frame.ps1`. Expected: it fails to compile because `buildAudioFrame` doesn't exist.
- [ ] **Step 3: Implement** the interfaces above.
- [ ] **Step 4: Run** `qa/run-audio-frame.ps1` (exit 0), then `assembleDebug` (the native code compiles for both ABIs), then `testDebugUnitTest`.
- [ ] **Step 5: Commit**

```bash
git add libomt app/src/main/cpp app/src/main/java qa
git commit -m "feat(omt): send FPA1 audio to audio-subscribed receivers"
```

---

### Task 4: The capture service

**Files:**
- Create: `app/src/main/java/io/github/bachtam2001/breadomtscreen/{CaptureService.java,Announcer.java}`
- Modify: `AndroidManifest.xml`.
  - **Permissions:** `INTERNET`, `ACCESS_NETWORK_STATE`, `ACCESS_WIFI_STATE`, `CHANGE_WIFI_MULTICAST_STATE`, `WAKE_LOCK`, `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_MEDIA_PROJECTION`, `RECORD_AUDIO`, `POST_NOTIFICATIONS`.
  - **Service:** `<service android:name=".CaptureService" android:foregroundServiceType="mediaProjection" android:exported="false"/>`.

**Interfaces:**
- `Announcer(Context)`, `boolean start(String name)` and `void stop()`.
  - It is NsdManager registration of `_omt._tcp` on port 6400, plus the multicast lock, the Wi-Fi low-latency lock and a partial wake lock. Each is taken idempotently and released in `stop()`.
  - Follow `OMTStreamingManager.registerService` / `unregisterService` in `opencamera-omthx` (its comments explain each leak it fixed).
- `CaptureService`:
  - **Start:** `ACTION_START` with extras `resultCode`, `data` (the consent `Intent`), `name`, `quality` (1/50/100) and `fps` (30/60). It calls `startForeground` (type `MEDIA_PROJECTION`) **before** `getMediaProjection`. Then `OMTSender.init(name, w, h, fps, quality)`, `Announcer.start(name)`, registers the projection callback, and calls `createVirtualDisplay` once with a `null` surface at the fitted size.
  - **Stop:** `ACTION_STOP` and `onStop` both tear down in reverse order and `stopSelf()`.
  - **Viewer poll:** every 250 ms, read `getViewerCount()` and feed `ViewerGate(5000)`. On rising to true: pick HEVC if the phone has an HEVC encoder (else H.264), `SizeFit.fit` against that encoder's `VideoCapabilities` (`areSizeAndRateSupported`), create an `OMTHXEncoder` whose sink calls `sendCoded`, `virtualDisplay.setSurface(encoder surface)`, and start audio. On falling to false: `setSurface(null)`, stop the encoder, stop audio.
  - **Size change:** `onCapturedContentResize` (API 34+), or a display-listener size change before that. It runs `SizeFit` again, calls `virtualDisplay.resize(w, h, dpi)`, and restarts the encoder if running.
  - **Audio:** an `AudioRecord` built with `AudioPlaybackCaptureConfiguration` (USAGE_GAME, USAGE_MEDIA, USAGE_UNKNOWN), `ENCODING_PCM_FLOAT`, 48000 Hz, stereo, read in 960-frame (20 ms) chunks on its own thread. Each chunk goes through `AudioPack.toPlanar` and `sendAudio` with `AudioPack.ptsNs`, the first frame time taken from `AudioRecord.getTimestamp(..., TIMEBASE_MONOTONIC)` (or `System.nanoTime()` when that fails). `AudioPack.level` feeds the meter. Without the permission, there's no AudioRecord and the status carries the "Sound is off" copy.
  - **Status:** a `static` listener (one activity) gets `(String statusLine, float level, boolean soundOff)` on each poll. The notification text is the status line, with a `Stop` action.

- [ ] **Step 1: Write the failing test** `ServiceStatusTest` for `static String CaptureService.statusLine(boolean live, int w, int h, int fps, int viewers)`. `statusLine(false, 0, 0, 0, 0)` is `"Idle: waiting for a viewer"`. `statusLine(true, 2400, 1080, 60, 2)` is `"Live: 2400×1080 60 fps · 2 viewer(s)"`.
- [ ] **Step 2: Run** `testDebugUnitTest --tests "*ServiceStatusTest"`. Expected: it fails to compile.
- [ ] **Step 3: Implement** the service and the announcer.
- [ ] **Step 4: Run** `testDebugUnitTest`, then `assembleDebug lintDebug`. Lint reports no `Error`-severity issue in the new files (`app/build/reports/lint-results-debug.txt`).
- [ ] **Step 5: Commit**

```bash
git add app/src
git commit -m "feat: capture service encodes the screen and game sound only while someone watches"
```

---

### Task 5: The Go Live screen

**Files:**
- Modify: `MainActivity.java`. Create `app/src/main/res/layout/activity_main.xml`.

**Interfaces:**
- **Controls:**
  - a name field (default `OMTStreamName.normalize(Build.MODEL)`, which gives `<model> (Screen)`; saved through `OMTStreamName.normalize` on Go Live);
  - Quality (Low / Medium / High, default Medium);
  - Rate (60 / 30, default 60);
  - one large button `Go Live` / `Stop`.

  Settings are kept in `SharedPreferences`.
- **Status:** the status line, a horizontal meter driven by `level`, the hint `No sound? Some games block sound capture.`, the Wi-Fi network name or `Ethernet`, and, on `Build.VERSION.SDK_INT < 34`, the whole-screen note.
- **Go Live flow:**
  1. Request `RECORD_AUDIO`, and `POST_NOTIFICATIONS` on API 33+.
  2. Whatever the answer, call `MediaProjectionManager.createScreenCaptureIntent()` through `startActivityForResult`.
  3. On `RESULT_OK`, start `CaptureService` (`startForegroundService`) with the extras from Task 4. A refusal leaves the screen as it was.
- **Stop:** sends `ACTION_STOP`. The button follows the service's live state through the listener.

- [ ] **Step 1: Write the failing test** `MainCopyTest`, checking `static String MainActivity.wholeScreenNote(int sdk)`: it is the exact whole-screen copy for 29..33 and `null` for 34+.
- [ ] **Step 2: Run** it. Expected: it fails to compile.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `testDebugUnitTest`, then `assembleDebug lintDebug` (no new Error-severity lint).
- [ ] **Step 5: Commit**

```bash
git add app/src
git commit -m "feat: Go Live screen with name, quality, rate, status and sound meter"
```

---

### Task 6: README, owner checklist and merge

**Files:**
- Modify: `README.md`.
  - **What it is**, with the path: phone → BreadCamera or OMT-Bridge → vMix.
  - **Setup:** a 5 GHz or Wi-Fi 6 router, or a USB-C Ethernet adapter with charging pass-through. About 15 Mbps per phone at Medium 2400×1080 60 fps, and 150–300 Mbps for ten phones.
  - **Build:** `./gradlew.bat assembleDebug`.
  - **License and credits.**
  - **Owner checklist on a real phone**, one line each:
    1. The source appears in BreadCamera / OMT-Bridge after Go Live.
    2. The phone shows `Idle: waiting for a viewer` until something connects.
    3. The picture keeps the phone's native shape at 60 fps.
    4. A real game shows no felt lag compared with not streaming.
    5. Sound is in sync.
    6. A game that blocks sound shows a flat meter with video still fine.
    7. Rotation changes the size cleanly.
    8. On Android 14+, choosing the game app only keeps notifications off stream.
    9. Ending sharing from Android's own UI stops the app.
    10. Refusing Record audio still goes live, with the "Sound is off" copy.
    11. Stop, then Go Live again, asks consent and works.

- [ ] **Step 1:** Full check on the branch: `qa/run-audio-frame.ps1`, `./gradlew.bat --no-daemon testDebugUnitTest assembleDebug lintDebug`. All pass, with no new Error-severity lint.
- [ ] **Step 2:** `git checkout main; git merge --no-ff bos-first-version -m "merge: Bread OMT Screen first version"`, then the same full check on `main`. Do not push.
