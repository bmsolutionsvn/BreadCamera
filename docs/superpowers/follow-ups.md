# BreadCamera follow-ups

Deliberate shortcuts and open items. One line each, with where it lives.

## Untested on hardware
- `h264_nvenc` preview path — never opened on the AMD build PC (`src/engine/preview_encoder.cpp`).
- NDI output — only tested if the NDI SDK was installed on the build PC; see the M1 report (`src/io/ndi_output.cpp`).
- Task 6: minor (deferred): `tests/ffmpeg_input_test.cpp:206` — new decode-failure test forces `allowHwDecode=false`, so the d3d11va-stall variant is proven by construction only.
- Task 11A fix: minor (deferred): the new code-point test drives `validateOutput` only — no runtime test drives `adapter_factory::makeOutput` with a multi-byte NDI name (the `#if BC_WITH_NDI` line compiles but is unexercised); disclosed in the fix report.

## Known ceilings (ponytail)
- Readback maps synchronously under the context lock (`src/gpu/converter.cpp`, `Converter::readback`); double-buffer staging if the tick budget gets tight.
- Task 14 fix: the 4x 1080p60 M1 acceptance case fails reproducibly on the readback hotspot — `Converter::readback`/`toBgra8` (synchronous `CopyResource` + blocking `Map(READ)` + 8.3 MB memcpy under the context lock, `src/engine/camera.cpp:140`, per distinct size per camera per tick) costs ~98% of the tick (~24 fps instead of 60); `NdiOutput::needsCpu()` is true (`src/io/ndi_output.h:15`) and spec §9 names NDI as M1's output; A/B with `cpu:false` outputs passes (1201 frames/20 s, 0 overruns) (`tests/e2e_m1_test.cpp`). Reported, not fixed — planner decides.
- Bilinear-only downscale for previews (`src/gpu/shaders/linear_to_bgra8.hlsl`).
- No audio clock-drift resampling; overflow trims (`src/core/audio_delay.h`).
- The audio resampler is built from the first frame only (`src/io/ffmpeg_input.cpp`, `emitAudio`).
- Fullscreen output stretches, no letterbox (`src/io/fullscreen_output.h`).
- `Converter::fit` is plain bilinear; add a box pre-filter if 1080p → 540p outputs shimmer (`src/gpu/shaders/fit_linear.hlsl`).
- After a tick overrun a half-rate output can wait one extra tick (`src/engine/camera.cpp`).
- Preview encoding runs on the tick thread (`src/engine/preview_service.h`).
- WebSocket sends happen under the client-map lock (`src/engine/control_server.cpp`).
- Task 3: minor (deferred): `src/core/show_store.cpp:71` — `CopyFileW` return ignored; a failed LKG copy still reports success.
- Task 3: minor (deferred): `src/core/show_store.cpp:67-70` — main replaced before parse-back validation (invalid in-memory Show reaches disk first).
- Task 3: minor (deferred): `src/core/show_store.cpp:113-117` — a failed debounced save consumes `pending_` with no retry (recorded in `lastSaveError()`).
- Task 3: minor (deferred): `src/core/show.cpp:20` — a non-string value under a secret key bypasses Encrypt/Redact (unreachable via `load()`, reachable in memory).
- Task 3: minor (deferred): `src/core/show.cpp:71-72` — `outputToJson` writes `params` raw in every mode; fine for M1, a hazard when an output kind gains a secret param (plan-shaped: `outputToJson` has no `SecretMode`).
- Task 3: minor (deferred): `src/core/show.cpp:153` — a non-object `"settings"` silently coerces to defaults instead of `ShowError`.
- Task 3: minor (deferred): `src/core/rate.cpp:19` — leading-zero spellings (`"060/1"`) accepted (plan-verbatim).
- Task 2: minor (deferred): `src/core/bounded_queue.h:21` — `push()` into a closed queue discards without incrementing `dropped()`.
- Task 2: minor (deferred): `src/core/bounded_queue.h:23` — `BoundedQueue<T>(0)` calls `pop_front()` on an empty deque → UB (plan code; queue sizes come from spec §4.1 as 2).
- Task 2: minor (deferred): `src/core/frame_sync.h:58-59` — `repeats_` increments for Slate/idle picks that emit nothing, over-counting vs. its name.
- Task 6: minor (deferred): `src/io/srt_params.cpp:33` — a >2048-char URL throws "url must start with srt://"; length violation reported as a scheme error.
- Task 6: minor (deferred): `src/io/input.h:24-25` — contract comment does not say the caller must `stop()` before destroying the `InputSink` (raw `sink_` → UB otherwise).
- Task 6: minor (deferred): `src/io/ffmpeg_input.cpp:228-246` — the archetypal EAGAIN-only failure mode throws with no FFmpeg error code in the message.
- Task 6: minor (deferred): `src/io/ffmpeg_input.cpp:80` — `av_log_set_level(AV_LOG_ERROR)` leaves FFmpeg/libsrt stderr unredacted (could echo a full URL with query once a log sink lands); `bc`'s own messages are query-stripped.
- Task 5: minor (deferred): `CMakeLists.txt:45` — `find_program(FXC fxc REQUIRED)` hard-fails configure outside a VS dev shell (the one place a missing tool breaks the "never fail the build" posture).
- Task 5: minor (deferred): `src/gpu/converter.cpp:55-57` — `Map`'s HRESULT unchecked before `memcpy` into `m.pData`.
- Task 11: minor (deferred): `src/engine/engine_main.cpp:55-59` — `std::stoi` accepts trailing garbage (`--port 47810xyz` → 47810 instead of exit 2); check `pos == v.size()`.
- Task 11: minor (deferred): `engine_main.cpp:37,43,82` — `writeDiscovery`'s `create_directories` and `GpuDevice::create` are outside any try, so those failures exit with a non-{0,2,3,4} code (and discovery skips removal); `MoveFileExW` result ignored; `engine_main.cpp:65` echoes an unknown first argv (a positional-token mis-invocation prints the token to stderr).
- Task 11: minor (deferred): `control_server.cpp:75` — client→engine **binary** frames are silently dropped (spec defines text control messages only; "malformed → error response" does not extend to binary).
- Task 11: minor (deferred): `command_router.cpp:35-43` — `mergeSecrets`: a `params` object that merely *omits* `passphrase` clears the stored secret (matches the defined contract, but a forgetful client silently wipes it).
- Task 11: minor (deferred): `engine_main.cpp:164` — a stale discovery file survives an unclean exit (dead pid/port/token until the next launch); the UI should validate the pid or the reader should check staleness.
- Task 9 (resolved in `b337f51`, whole-branch fix wave): `/cameras/<id>/tickError` used to be write-once-forever — it now clears on the next clean tick (`src/engine/engine.cpp`, success path of `cam->tick`), pinned by an engine test that forces a throw, heals the camera and asserts the key is gone.
- Task 9: minor (deferred): `src/engine/engine.h:26` comment claims adapters are torn down outside the tick lock, but `reconfigure()`'s `out->stop()` runs inside `apply()`'s `mutex_` — the comment overclaims; a vsync-blocked `Present` can stall the tick ~1 period during a show edit (plan-verbatim).
- Task 9: minor (deferred): two residual never-black edges unreachable today — a camera's first-ever tick throwing yields a null `TextureRef` to `tap_` (`engine.cpp:148,157`) and `FullscreenOutput::deliver` returns without presenting when `!f.linear` yet counts it delivered. **Flagged to the Task 10/11 implementers: null-check `tap_` frames.**
- Task 9: minor (deferred): pre-loop unlocked `settings_` read (`engine.cpp:114`) — formal C++ data race, harmless on the normal path; cheap hardening is to construct `sched` inside the first locked block.
- Task 7: minor (deferred): base `QueuedOutput` destructor `abort()`s when a subclass forgets `stop()` — loud but fatal; consider a softer diagnostic.
- Carried flag for the whole-branch triage: no show-name field exists in `ShowSettings` (the plan's protocol table row lives only in `docs/`), and the NDI SDK's own 200-name cap is what the limit protects.
- Task 12 fix: residual variant of finding 2 — a stale-but-parseable engine.json from a crash plus an engine started moments earlier (mutex held, file not yet overwritten) skips waitForDiscovery and can re-enter the relaunch loop (`src/shell/shell_main.cpp`).
- Task 12 (observation, not graded): showWindow() issues SW_SHOW then an unguarded SW_RESTORE, so tray "Open BreadCamera" de-maximises a maximised window (`src/shell/shell_main.cpp`).
- Task 12 (observation, not graded): quitAll()'s direct launcher->isRunning()/kill() calls sit outside the supervisor mutex — sound only because stopSupervising() precedes them; an unwritten invariant (`src/shell/shell_main.cpp`).
- Task 12 (observation, inherent): TOCTOU between isEngineProcess and adopt's OpenProcess; the opened handle pins the process object, so TerminateProcess still cannot hit anything else.
- Task 4: the background-image decode + upload runs inside `loadStageParams`, i.e. under the engine mutex, so every camera stalls while a big image decodes (`src/engine/camera.cpp:21-45`).
- Task 4: `Compositor::refine` runs whenever an alpha exists even when the background mode is `none` — wasted GPU work (`src/engine/camera.cpp`, `endTick`).
- Task 4: per-tick JSON lookups (`backgroundFrom`/`refineFrom`/matte quality) and per-tick batch allocations should be precomputed in `loadStageParams` (`src/engine/camera.cpp`, `endTick` / `engine.cpp` matte loop).
- Task 4: the slate loader has the same narrow-string path bug the background loader was fixed for — `loadImageBgra(settings_.slatePath)` builds the path from a UTF-8 `std::string` (ANSI code page on Windows) (`src/engine/engine.cpp:42`); fix like `camera.cpp` with `bc::toWide`.
- Task 4 housekeeping: five empty root-level `*.hlsl` duplicates from `d69c4fc` (`alpha_blur.hlsl`, `alpha_morph.hlsl`, `bg_blur.hlsl`, `bg_composite.hlsl`, `bg_downsample.hlsl`) should be deleted in a separate housekeeping commit.

## Spec deviations (authorised in M1)
- NDI alpha uses BGRA instead of UYVA (`src/io/ndi_output.cpp`).
- Preview sizes are 512×288 / 1280×720 instead of ~480×270 (`src/engine/preview_hub.h`).
- The UI ships beside the shell exe (`ui/` folder) instead of embedded in it (`CMakeLists.txt`, `cmake/CopyUi.cmake`).

## Test gaps (deferred)
- Socket tests use fixed ports inside 44620–48715, which WSL mirrored networking (`%USERPROFILE%\.wslconfig`, `networkingMode=mirrored`) reserves on the build PC: `ControlServer: refuses a wrong token…`, `ControlServer: hostile messages…`, `FfmpegInput: an SRT listener…`, `FfmpegInput: stop() returns promptly…` and the M1 acceptance cases fail to bind there. The aider night checks exclude them; fix by binding port 0 (OS-chosen) in the tests, or move WSL's range.
- Task 2: minor (deferred): `tests/bounded_queue_test.cpp:35` — asserts on the wall clock (plan-verbatim; wide 1000 ms bound).
- Task 2: observation for the whole-branch review: Task 2 left `setPolicy`, `reopen`, `toString(FeedPhase)`, `TickSchedule::rate()`, `AudioDelay::sampleRate()/channels()` unasserted (they compile).
- Task 6: minor (deferred): `tests/ffmpeg_input_test.cpp` — `RecordingSink` declared after `FfmpegInput`, so a mid-test REQUIRE failure can destroy the sink while the input thread calls it.
- Task 5: minor (deferred): `tests/gpu_test.cpp:52-68` — nothing pins the `height < 720` (BT.601) side of the GPU matrix selection (chroma-neutral content cannot distinguish matrices).
- Task 11: minor (deferred): `tests/control_server_test.cpp:139-142` — the wire test covers 3 of 4 hostile categories; wrong-typed fields are pinned at router level only (one `ask(R'({"id":11,"cmd":"addCamera","args":"x"})','bad-request')` closes it).
- Task 10: minor (deferred): `tests/preview_test.cpp:55` / `preview_encoder.cpp:90` — `forceKeyframe` is not isolated: `gop_size = fps` puts a natural IDR at frame 30 exactly where the test forces, so `keys >= 2` passes even if forcing did nothing (brief-verbatim). Force at a non-GOP frame instead.
- Task 11A: minor (deferred): no engine-level test for a mid-stream **show-resolution** change (`setSettings resolution` → `apply` → `setFormat`); the size-switch test switches the *input* size and uses a single output, and a tail-stall with no exception slips past `maxGap`. One extra case worth adding in the whole-branch wave.
- Task 11A: minor (deferred): `tests/gpu_test.cpp:132-156` — only a 1:1 source is letterbox-tested; the 4:3-input case from the rules has no direct test (shader formula is aspect-generic — coverage breadth only).
- Task 9: minor (deferred): coverage gaps the brief gave no tests for — `tickError` catch, `input-config` path, engine stop→start, mid-show rate change, `setFrameTap`, stall with multiple outputs, a *throwing* (vs slow/config-failed) output beside a healthy peer.
- Task 9: minor (deferred): `FakeInput` now pushes its first frame before `onState(Live, …)` (baseline signalled Live first); no test asserts `/input/state` for fake inputs.
- Task 12 fix: no positive unit case for isEngineProcess (a real engine pid → true); only the smoke run covers it (`tests/supervisor_test.cpp`).
- Task 12 fix: if REQUIRE(l.isRunning()) fails in the isEngineProcess test, l.kill() is skipped and a 30 s PING lingers (`tests/supervisor_test.cpp`).
- Task 14 fix: `tests/engine_test.cpp:82` `maxGap < 50ms` measured exactly 50 ms right after the 4x case in-suite (3/3 pass in isolation; did not recur in the next two full runs) — boundary flake, deferred.
- Task 4: minor (deferred): unlocked `r.matte->batchSizes` reads in one matte test (`tests/engine_matte_test.cpp`, the blur/replace case) — snapshot under `r.matte->m` like the overrun case does.
- Task 4: pre-existing flake — `tests/engine_test.cpp:251` half-rate audio carry (`1600 == 3200`, 1 of 3 runs), unrelated to Task 4.
- Task 14 review: `tests/e2e_m1_test.cpp` — `exeDir()` neither checks `GetModuleFileNameW`'s return length nor `MAX_PATH` truncation (test-only; the folder is controlled).
- Task 14 review: `tests/e2e_m1_test.cpp` — the `askShow()` busy-retry loops have empty bodies; a short sleep between attempts would be clearer (style).
- Task 14 fix review: `tests/e2e_m1_test.cpp` — the CaseGuard is declared after `engine.start()`/the pusher loop, leaving a narrow non-REQUIRE exception window before the guard exists; `~CaseGuard` is implicitly noexcept (its steps are verified-idempotent and non-throwing); and the seam test cannot pin the guard's declaration-order property (verified by review instead).
- Whole-branch review: no engine-level test for a mid-stream **show**-resolution change (`setSettings` → `apply` → `setFormat`) — the path is simple but runtime-untested; M2 hardening (RF#2 as phrased covers the input-side switch).
- Whole-branch review pre-merge checklist (planner-owned, operator-facing): `scripts/build.ps1`/`test.ps1` have only been invoked via byte-identical `.bat` stand-ins (unattended-session approval gate — the `.ps1` files themselves are untouched since `b5144bd`); `.github/workflows/ci.yml` has never executed (confirm `windows-2025` resolves); a `BC_WITH_NDI=OFF` configure has never run on this machine (the NDI 6 SDK is installed, so builds are `BC_WITH_NDI=ON`).

## Elsewhere
- The UI uses `@bmsolutionsvn/design`'s tokens (`bm.css`) but not its MUI theme or `BmAppShell`: the native pro-tool window is BreadCamera's own (spec §6.4, `ui/src/app.css`).
- The title bar loses the top-edge resize handle (the side and bottom edges and Snap still resize) (`src/shell/shell_main.cpp`, `WM_NCCALCSIZE`).
- Task 5: minor (deferred): `tests/gpu_test.cpp:73` — doctest `MESSAGE` prints `AMD Radeon RX 66001` (needs `DOCTEST_CONFIG_TREAT_CHAR_STAR_AS_STRING` in `tests/main.cpp`, outside Task 5's file set).
- Task 13 (unverified claim, verified 2026-10-10): the npm install reported 6 vulnerabilities (2 critical) in the ui/ dev chain — `npm audit --prefix ui` returns exactly **6 vulnerabilities (3 moderate, 1 high, 2 critical)**, all in the vitest/vite/esbuild/tinypool/vite-node dev chain; `npm audit fix --force` would install vitest@5.0.3 (a breaking change), so it stays deferred (`ui/package-lock.json`).
