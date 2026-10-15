# BreadCamera follow-ups

Deliberate shortcuts and open items. One line each, with where it lives.

## M4a
- M4a Task 2 review: flipped NDI video (negative `line_stride_in_bytes`) is skipped once with `{code:"ndi-format"}` — we request `NDIlib_recv_color_format_BGRX_BGRA`, not the `_flipped` variant; if a flipped source appears, request `_flipped` or flip on the GPU (`src/io/ndi_input.cpp`).
- M4a Task 2 review: `tests/ndi_input_test.cpp` uses fixed sleeps (4 s / 3 s / 5 s); poll a condition instead if the build box is loaded.
- M4a Task 3 (OMT output, libomt) is superseded by `docs/superpowers/plans/2026-10-13-m4c-omt-hx-output.md` (omthx, plain OMT + OMT-HX) — not implemented here.
- M4a Task 4: capture is video only (no audio) — USB/UVC audio stays a follow-up (spec §4.2).
- M4a Task 4: the build PC enumerates 5 virtual capture devices (NDI Webcam Video 1–4, a phone virtual camera). The first never delivers a frame, so the input uses an async MF reader and the test scans devices for one that delivers; a PC with none prints `[skip] no capture device delivered frames` (`src/io/mf_capture_input.cpp`, `tests/mf_capture_test.cpp`).
- M4a Task 6: NDI input drops audio that is not 48 kHz, with a visible message (`src/io/ndi_input.cpp`).
- M4a Task 6: NDI input/discovery are tested only when the NDI SDK is installed on the build PC (`tests/ndi_input_test.cpp`).

## M4b
- DeckLink, AJA and the virtual camera are **untested on hardware**; the owner's checklist is `docs/superpowers/manual-checks/m4b-hardware.md`.
- AJA output is fill only (spec §4.3 lists key+fill) — add a second channel for the key (`src/io/aja_output.cpp`).
- DeckLink output uses `DisplayVideoFrameSync`; scheduled playback with preroll would remove jitter (`src/io/decklink_output.cpp`).
- UYVY conversion creates a swscale context per frame (`src/io/uyvy.cpp`).
- The virtual camera is 1920×1080 at 30 fps, at most 4 cameras (`src/core/vcam_shared.h`).
- The vcam DLL needs HKLM registration (admin) — done by the installer (M5).

## M4c
- Draw the OMT tally on the multiview tile (reads `outputs/<id>/tally`) (`src/engine/camera.cpp`).
- M4c acceptance: the operator two-tool check (engine `omt` output + OMT-HX `omthx-recv.exe`) was not run on this PC — it needs a live input delivering frames to the camera; the automated `tests/omt_output_test.cpp` covers the same sender↔receiver path (plain OMT, OMT-HX with alpha, and the tally round-trip).

## Untested on hardware
- `h264_nvenc` preview path — never opened on the AMD build PC (`src/engine/preview_encoder.cpp`).
- NDI output — only tested if the NDI SDK was installed on the build PC; see the M1 report (`src/io/ndi_output.cpp`).
- Task 6: minor (deferred): `tests/ffmpeg_input_test.cpp:206` — new decode-failure test forces `allowHwDecode=false`, so the d3d11va-stall variant is proven by construction only.
- Task 11A fix: minor (deferred): the new code-point test drives `validateOutput` only — no runtime test drives `adapter_factory::makeOutput` with a multi-byte NDI name (the `#if BC_WITH_NDI` line compiles but is unexercised); disclosed in the fix report.
- Maxine (M2 Task 5): the backend and the SDK proxy sources compile only with the SDK present — expect `/permissive- /W4` conformance hard errors from the proxies on the first RTX build (`CMakeLists.txt`, the `BC_WITH_MAXINE` block); silence per-source if needed.
- Maxine (M2 Task 5): runtime needs the SDK `bin` on `PATH` — the SDK proxies `LoadLibrary` the vendor DLLs from the normal search path; `cmake/FindMaxine.cmake` only adds includes.
- Maxine (M2 Task 5): `NvVFX_SetImage` copy-vs-retain semantics and `NvCVImage_Transfer` completion semantics were pinned against the online SDK docs only — re-confirm on the installed headers at the first RTX compile (the fix takes the conservative reading: persistent view images + an explicit stream sync after every transfer).

## Known ceilings (ponytail)
- Readback maps synchronously under the context lock (`src/gpu/converter.cpp`, `Converter::readback`) — **fixed** in `b1e9bc2`: the camera now `begin`s a staging copy and `tryFinish`es it non-blocking (`D3D11_MAP_FLAG_DO_NOT_WAIT`), so the tick never maps (`src/gpu/readback.h`, `src/engine/camera.cpp` `endTick`); the blocking `readback` stays for callers that need this tick's pixels (the engine capture path; the ONNX matte used it on the tick until `d4faaed`).
- Task 14 fix: the 4x 1080p60 M1 acceptance case fails reproducibly on the readback hotspot — `Converter::readback`/`toBgra8` (synchronous `CopyResource` + blocking `Map(READ)` + 8.3 MB memcpy under the context lock, `src/engine/camera.cpp`, per distinct size per camera per tick) costs ~98% of the tick (~24 fps instead of 60); `NdiOutput::needsCpu()` is true (`src/io/ndi_output.h:15`) and spec §9 names NDI as M1's output; A/B with `cpu:false` outputs passes (1201 frames/20 s, 0 overruns). **Fixed** in `b1e9bc2` (async `ReadbackSlot`): GTX 1650 M1 now **17** overruns/20 s (was 991), **1182–1183** frames/20 s ≈59 fps (was 209 ≈10 fps), latency **8.8–11.5** ms (was 30/52/73/93); M2 **9** overruns/20 s (was 1030), latency **8.4/10.1/12.1/14.4** ms (was 55/101/144/186) — only the strict `overruns == 0` bound still fails (`tests/e2e_m1_test.cpp`, `tests/e2e_m2_test.cpp`).
- Residual after `b1e9bc2`: the readback still copies the full frame (~8.3 MB) per distinct output size per camera per tick — `toBgra8` + the staging `CopyResource` stay on the tick (`src/engine/camera.cpp`, `endTick`); a fully GPU-side encode path would remove it (`src/gpu/readback.h`).
- Residual after `b1e9bc2`: `overruns == 0` is still not met on this PC — the §8.3 soak (GTX 1650, `bc_soak --minutes 2`, exit 1, `%TEMP%\bcam-soak.csv`) still shows **+304…+397** overruns/10 s (was ~430–465) with median latency **17.3/27.1/31.8/38.9** ms and working set ~1.51 GB; the tick's remaining `toBgra8`/`CopyResource` calls plus the 4 SRT output paths (CPU `sws_scale` + `h264_nvenc` sharing the GPU with the tick, 4 SRT decode threads) are the likely co-limiters — the same 4×1080p60 acceptances with a cheap `fake` sink now settle at ~9–17 overruns/20 s, so the tick itself is nearly on-budget (`tools/soak/soak_main.cpp`).
- Residual after `b1e9bc2`: the ONNX matte's own per-camera readback was still on the tick (`OnnxMatte::process`, `src/engine/onnx_matte.cpp`) — M6 recorded **+388** overruns/20 s with the matte sampled `active` at **28.1** ms/camera (> the 16.7 ms tick and the 10 ms 60 %-tick matte budget; under load the guard bypassed it, so an earlier sample read `bypassed`). **Fixed** in `d4faaed`: the matte now `begin`s/`tryFinish`es a per-camera `ReadbackSlot` so only the cheap `toBgra8` downsize dispatches on the tick — GTX 1650 M6 now **0** overruns/20 s (was +388), matte **`active`** on all 4 cameras, latency **7.2–8.6** ms; inference is unchanged at **27.9–34.9** ms/camera (it was already on the worker) — the acceptance passes 10/10 over two runs (`tests/e2e_m6_test.cpp`).
- Residual after `d4faaed`: the ONNX matte's inference is still serialised per camera on one worker (`OnnxMatte::worker` round-robins — `src/engine/onnx_matte.h`), so the mask trails by camera-count × inference; the per-camera `toBgra8` downsize still dispatches on the tick (`OnnxMatte::process`) — only the blocking `Map`/`memcpy` moved off; and `overruns == 0` still fails in the §8.3 soak (GTX 1650, `bc_soak --minutes 2`, exit 1, `%TEMP%\bcam-soak.csv`: **+353…+415** overruns/10 s, matte `active` on all 4 cameras, median latency **24.3/32.8/40.5/52.0** ms — up from the recorded post-`b1e9bc2` +304…+397 and 17.3/27.1/31.8/38.9 ms — consistent with the matte now doing its per-camera inference in the soak instead of being bypassed — working set ~1.52 GB) — the M6 acceptance (fake outputs) now passes, so the soak's residual overruns come from the SRT output paths, not the matte (`tools/soak/soak_main.cpp`).
- Bilinear-only downscale for previews (`src/gpu/shaders/linear_to_bgra8.hlsl`).
- M3: `sampleChartRaw` is exported without a `validQuad` guard (`src/core/chart.h:30`) — only `sampleChart` validates; document the contract or validate at the raw entry too.
- M3: the tone-curve duplicate-x merge is a pairwise mean, not an n-point mean (`src/core/calibration.cpp`, `neutralCurve`) — absorbed by the monotone pass and the ±0.25 clamp, so only visible with clustered near-identical patches.
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
- Maxine (M2 Task 5): the green-screen model batch is pinned to 8 (`NVVFX_MODEL_BATCH`, `src/engine/maxine_matte.cpp`) — more than 8 simultaneous matte cameras exceeds it; the grow-only capacity rebuild would need a batching split.
- Maxine (M2 Task 5): quality-flip hysteresis in `MaxineMatte::process` (`src/engine/maxine_matte.cpp`) — a leading-camera change flips `q` and triggers a full rebuild (model reload under the engine mutex); debounce or pin quality per show.
- M2 Task 6 review: a failed `getStageSchemas` load is swallowed in `useStageSchemas` (`ui/src/state/schemas.ts`) — an empty schema list renders "No effects yet" in the Effects section even when the camera has stages; surface a schemas-unavailable state.
- M2 Task 6 review: `as IconName` casts (`ui/src/inspector/Effects.tsx`, `ui/src/dialogs/AddEffectDialog.tsx`, `ui/src/multiview/Strip.tsx`) hide unknown wire icon names as a blank svg; add a membership fallback in `Icon`.
- M2 Task 6 review: `labelOf` is duplicated in `Effects.tsx` and `AddEffectDialog.tsx` — move it to `ui/src/state/labels.ts`.
- M2 Task 6 review: `useStageSchemas` opens a second `useEngine` subscription (`ui/src/state/schemas.ts`) — pass the engine status down from Workspace instead.
- M2 Task 6 review: strip icon vs the effect switch disagree when `stages[kind].enabled` is absent (hand-edited show only) — `Strip.tsx:44` treats absent as on (`!== false`), the switch falls back to `fields[0].default` (`Effects.tsx`).

## Spec deviations (authorised in M1)
- NDI alpha uses BGRA instead of UYVA (`src/io/ndi_output.cpp`).
- Preview sizes are 512×288 / 1280×720 instead of ~480×270 (`src/engine/preview_hub.h`).
- The UI ships beside the shell exe (`ui/` folder) instead of embedded in it (`CMakeLists.txt`, `cmake/CopyUi.cmake`).

## Test gaps (deferred)
- Socket tests use fixed ports inside 44620–48715, which WSL mirrored networking (`%USERPROFILE%\.wslconfig`, `networkingMode=mirrored`) reserves on the build PC: `ControlServer: refuses a wrong token…`, `ControlServer: hostile messages…`, `FfmpegInput: an SRT listener…`, `FfmpegInput: stop() returns promptly…` and the M1 acceptance cases fail to bind there. The aider night checks exclude them; fix by binding port 0 (OS-chosen) in the tests, or move WSL's range. The M2 acceptance (`tests/e2e_m2_test.cpp`) sidesteps the range: listeners on 49100–49103 (the plan's 47931–47934 return WSAEADDRINUSE on this PC).
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
- Maxine (M2 Task 5): `tests/maxine_matte_test.cpp` only asserts matte dimensions — add a content check (non-trivial alpha) and a >4-camera growth case when an RTX machine is available.
- Task 4: pre-existing flake — `tests/engine_test.cpp:251` half-rate audio carry (`1600 == 3200`, 1 of 3 runs), unrelated to Task 4.
- M3: `tests/chart_test.cpp` has no empty-frames case and no boundary-quad (corners on the patch edge) case.
- M3 Task 6 review: a capture timeout and a genuine "no picture" both reach the operator as code `invalid` (`src/engine/command_router.cpp`, `src/engine/color_calibration_service.cpp`); give them distinct codes if it matters.
- M3 Task 6 review: `ColorCalibrationService::calibrate` holds its mutex across the ~1–3 s capture, so a concurrent `forget` (holding the router lock) waits (`src/engine/color_calibration_service.cpp`).
- M3 Task 6 review: `FakeFx::lastChart` is set but never asserted — the router test does not pin that the chart argument/corners reach the effect (`tests/command_router_test.cpp`).
- M3 Task 7 review: `useCalibration`'s `findChart` poll swallows transport errors — a persistent error leaves "Looking for the chart…" forever with nothing surfaced (`ui/src/calibration/useCalibration.ts`).
- M3 Task 8: under `BC_FORCE_WARP=1` two unrelated timing tests fail (`tests/engine_color_test.cpp` capture timeout, `tests/engine_test.cpp` tickError) — fixed 300 ms sleeps the software rasterizer cannot meet; not a product defect.
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

## M2
- Maxine AI Green Screen backend is **untested on hardware**; its only test (`tests/maxine_matte_test.cpp`) skips without an RTX GPU + `VFXSDK_ROOT` (`src/engine/maxine_matte.cpp`).
- Maxine frames are staged through CPU memory (~1–2 ms per camera); use NvCVImage D3D11 interop if the budget needs it (`src/engine/maxine_matte.cpp`).
- One matte quality per batch: the first camera's setting wins (`src/engine/maxine_matte.cpp`, `process`).
- Background stage budget is a fixed quarter tick per camera (`src/engine/camera.cpp`, `endTick`).
- The M2 acceptance measures pipeline cost with a test matte; the real inference cost needs an RTX run (`tests/e2e_m2_test.cpp`).
- M2 acceptance on the build PC (RX 6600): the zero-overrun bound **fails** — +742…+757 skips in 20 s across four runs (tick body 32.7 ms EWMA vs the 16.7 ms budget); every other check passes (latency 19.5/26.7/33.4/44.1 ms ≤ 50 in the final run, all runs ≤ 48, matte + background `active` on all four cameras, keyed corner/centre alpha 0/255 — 15 of 16 assertions). Temporary instrumentation split the tick: matte batch ~0.001 ms (TestMatte), refine ≤ 0.24 ms, background ≤ 0.03 ms per camera, blocking CPU readback 5.5–12.7 ms per camera (≈ the whole tick) — the same `Converter::readback` hotspot as the M1 entry under Known ceilings. Bounds left strict; the acceptance verdict is the owner's (`tests/e2e_m2_test.cpp`).
- M2 run observation: `CameraStats::fps` reads 164 during sustained overruns — `freshThisSecond_` accumulates until the `index % ticksPerSecond == 0` publish fires, and tick-index skipping delays that publish past one second (`src/engine/engine.cpp`, `run`).

## M3
- Real-camera calibration is a **human check** (`docs/superpowers/manual-checks/m3-calibration.md`). Automated evidence uses rendered charts with known casts (`tests/e2e_m3_test.cpp`).
- Chart detection depends on OpenCV's `mcc` module (`BC_WITH_CHART_DETECT`); without it the operator places the corners (`src/engine/chart_detector.cpp`).
- The tablet chart's colours are only as good as the tablet; tablet mode matches cameras to each other, not to absolute colour (`src/engine/color_calibration_service.cpp`).
- Trims stay in the colour schema as hidden fields at neutral values; manual trims are out of scope (spec §10) (`src/core/stage_params.cpp`).
- OpenCV (Apache-2.0) must be listed in the M5 licence notices (`installer/THIRD_PARTY_NOTICES.md`).

## M5
- The 8-hour soak (`scripts/soak.ps1 -Hours 8 -Faults`) is queued as its own night run; M5 carries the 30-minute evidence.
- Fresh-install check is a human gate: `docs/superpowers/manual-checks/m5-fresh-install.md`.
- Engine error messages stay as detail lines under the plain headlines (`ui/src/errors.ts`).
- The spec §8.3 soak does not pass on this PC: tick overruns and per-camera median latency > 50 ms (≈3 fps to the SRT outputs) on the then-default Intel UHD 630 adapter — the same Converter::readback hotspot as M1/M2 (§9); criteria unchanged, the 8-hour run stays queued (scripts/soak.ps1, .superpowers/sdd/2026-10-08-m5-hardening/task-3-report.md).

## M6
- ONNX backend uses DirectML on every vendor; spec §5.1 named TensorRT EP for NVIDIA (Maxine is NVIDIA's primary backend) (`src/engine/onnx_matte.cpp`).
- MODNet is a portrait model run frame by frame with simple EWMA smoothing: expect more edge flicker than Maxine; a person must judge quality on real cameras (human check).
- The ONNX mask trails the picture: a single worker round-robins every camera, so the lag scales with the camera count × the per-camera inference time (e.g. with 4 cameras each waits for the three ahead of it), not a flat "1–2 frames" (`src/engine/onnx_matte.h`).
- Worker inference errors (M6 review fix wave): resolved — `process()` now surfaces the first worker error (token-scoped, cleared by a later success), so a dead session bypasses and retries per the M2 rules instead of freezing the mask (`src/engine/onnx_matte.cpp`, `worker`/`process`).
- The M6 acceptance (`tests/e2e_m6_test.cpp`) is deliberately strict and **failed on an integrated GPU before `d4faaed`**: `OnnxMatte::process` did a synchronous per-camera readback on the tick thread, which tripped the engine's 60 %-tick overrun guard, so the matte stage reported `bypassed` (not `active`) and tick overruns rose (build PC, Intel UHD 630: +40 overruns in 20 s; `onnx-cpu` on WARP: +490); candidate fix: move the readback to the worker (`src/engine/onnx_matte.cpp`, `process`). **Resolved** in `d4faaed` (async `ReadbackSlot`; see the Known-ceilings entry): on the GTX 1650 the acceptance now passes with the matte `active` and 0 overruns/20 s — the UHD 630 and WARP (`onnx-cpu`) paths were not re-measured.
- The M6 acceptance's SRT ports (M6 review fix wave): resolved — moved from 47991–47994 (inside the WSL-mirrored reserved range on this PC) to 49161–49164 (`tests/e2e_m6_test.cpp`).

## GPU adapter (2026-10-16)
- The engine creates its D3D11 device on the **highest-performance** DXGI adapter by default (`b91f8ce`, `src/gpu/device.cpp`): on this PC the **NVIDIA GeForce GTX 1650**, not the Intel UHD 630. Override with `BC_GPU_ADAPTER` — a 0-based DXGI index or a case-insensitive description substring (e.g. `UHD` for the iGPU); an unmatched value is logged and ignored; `BC_FORCE_WARP=1` still wins.
- Measured this PC 2026-10-16, default (GTX 1650) vs `BC_GPU_ADAPTER=UHD` (UHD 630), `build\release\bc_soak.exe --minutes 2`: both exit 1 — NVIDIA `%TEMP%\bcam-soak-default.csv` ~430–465 overruns/10 s, per-camera median latency 24.8 / 37.4 / 48.6 / 58.3 ms, working set ~1.46 GB; Intel `%TEMP%\bcam-soak-intel.csv` ~490–590 overruns/10 s, medians 78.5 / 128.1 / 160.6 / 187.2 ms, working set ~2.01 GB (the §8.3 memory bound is not evaluated in a 2-minute run — the 10-minute baseline window never opens).
- Acceptances both ways (all still **FAIL**, bounds unchanged): M1 4×1080p60 — NVIDIA 991 overruns/20 s, 209 fake-output frames (≈10 fps), latency 30 / 52 / 73 / 93 ms; Intel 929 overruns, 272 frames, latency 30 / 44 / 61 / 72 ms — both miss ≥1180 frames and ≤50 ms (`tests/e2e_m1_test.cpp`). M2 — NVIDIA 1030 overruns, latency 55 / 101 / 144 / 186 ms; Intel 1110 overruns, 59 / 108 / 154 / 201 ms (`tests/e2e_m2_test.cpp`).
- M6 is the one case the adapter visibly moves: ONNX DirectML inference is **37.5 ms/camera on the GTX 1650 vs 425.8 ms on the UHD 630** (~11×), overruns +6 vs +171 — but the matte was `bypassed` (not `active`) both ways (pre-`d4faaed`; the async readback now keeps it `active` on the GTX 1650) and latency passes (6–7 / 4–26 ms) (`tests/e2e_m6_test.cpp`).
- **The adapter choice does not fix the throughput ceiling.** On the 4×1080p60 acceptances the two adapters are within run-to-run noise, so the synchronous `Converter::readback` (`src/gpu/converter.cpp`) under the tick lock — not the GPU — remains the bottleneck; those cases deliver through the CPU-bound `fake` output (`tests/support/fake_output.h`, `needsCpu() == true`), so this is the same hotspot the M1/M2/M5 entries already record.
- Open: the UI/Settings has **no adapter picker** — `BC_GPU_ADAPTER` is env-only; and the ONNX matte's inference is still serialised per camera on one worker, so the mask trails by camera-count × inference (`src/engine/onnx_matte.cpp`). **Partly fixed** in `d4faaed` (async `ReadbackSlot`): the per-camera readback is now off the tick — only the `toBgra8` downsize dispatch remains — so the pre-`d4faaed` "37.5 ms/camera > the 16.7 ms budget" figure (on-tick readback + inference measured together) is superseded.
- Surprise: this configure found no NDI SDK — `BC_WITH_NDI=OFF` ("Could NOT find NDI") — contrary to the whole-branch note that the NDI 6 SDK is installed; the soak's `ndi` outputs were refused and the acceptances use `fake` outputs, so the numbers above are unaffected.

## C1
- **Two simultaneous guests (acceptance).** Fixed in `46216fc` (whole-branch fix wave): `lkc` retains every subscribed track keyed by its mid and resolves each track's publisher identity from the offer's `mid_to_track_id` plus the participants' track lists, so two guests publishing in one room each keep their own video and audio track instead of the last one winning; `subscribe(identity)` filters by participant identity with a fail-closed rule for an unattributable mid. The C1 acceptance case "two simultaneous guests are two BreadCamera cameras" passes on this — slot 1 and slot 2 both decode and go live (`src/io/lkc/lkc.cpp`, `tests/e2e_c1_test.cpp`).
- **One hidden receiver per slot.** Each `BreadCallInput` creates its own `lkc::Client`, so N enabled slots join as N hidden participants and need N receiver tokens (one token reused for two inputs would kick the first). Sharing one client across slots is the remaining work (`src/io/breadcall_input.cpp`, design §5 D4).
- **Unsubscribed media still crosses the wire.** `lkc` sets `auto_subscribe=1` on the signalling URL and filters locally by participant identity; it never sends `UpdateSubscription`, so every slot's media is delivered to the receiver and the other slots' packets are dropped in-process rather than unsubscribed at the server. Send `UpdateSubscription` (subscribe to `guest_<slot>` only) if bandwidth matters at scale (`src/io/lkc/lkc.cpp`).
- **Slot-assignment race.** BreadCall picks a free slot at join from live presence, which lags a disconnecting guest; two quick joins can race for the same slot. Reserving slots is the fuller vMix-parity feature (`Breadcall`, design §5 D3).
- **`lkc` auth classification is string-matched.** `wsErrorIsAuthRejection` keys off libdatachannel's `"WebSocket connection failed"`/`"WebSocket handshake failed"`; it matches the two observed cases but is over-broad (a server that is up but not a WebSocket also reads as an auth rejection) (`src/io/lkc/lkc.cpp`).
- **Live audio is not end-to-end covered.** The acceptance publishers are video-only, so no Opus frame reaches the sink end to end; the decode path is implemented and lazily built (`src/io/breadcall_input.cpp`, `tests/e2e_c1_test.cpp`).
- **Factory `slot` guard gap.** Resolved (whole-branch fix wave): `DefaultAdapterFactory::makeInput` guards a missing/non-integer `slot` like `url`/`token`, throwing "BreadCall slot is required (a whole number)" instead of `nlohmann::out_of_range`; a slot outside 1..8 still reaches `BreadCallInput` and reads as `breadcall-no-guest` (`src/engine/adapter_factory.cpp`).
- **No duplicate-slot check.** Nothing refuses two cameras on the same slot; both subscribe to the same `guest_<n>` as separate receivers. Add a router/show validation (`src/engine/command_router.cpp`).
- **Token TTL and refresh.** The receiver token is short-lived; a stale token is terminal `input-config` and the operator re-mints — BreadCamera does not refresh it (`src/io/breadcall_input.cpp`, design §8).
- **SRTP profile.** libdatachannel's libsrtp build has no AES-GCM, so LiveKit media negotiates `SRTP_AES128_CM_SHA1_80`; confirm LiveKit Cloud's policy before relying on it in production (`src/io/lkc/lkc.cpp`).
- **BreadStudio's S2 must reuse `lkc`** — one WebRTC client for both products, no second stack (roadmap §3.2).
