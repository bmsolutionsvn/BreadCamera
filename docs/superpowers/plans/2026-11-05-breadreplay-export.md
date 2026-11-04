# BreadReplay Export Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Export a group, the current selection or one replay to MP4 (H.264 or H.265, High / Medium / Small or an exact bitrate), rendered exactly as it plays, in the background, without touching what is on air.

**Architecture:** An `ExportQueue` runs one job at a time on a below-normal thread. Each job builds its own GPU device, `FrameSource`, `Renderer` and a freezing `Player`, and drives a second `OutputPlayer` (the same composition: cuts, speed, zoom, Fade/Cut, audio) as fast as it can, with one output: an `Mp4Writer` that encodes with the GPU encoders and muxes to `<final>.part`, renamed on success.

**Tech Stack:** C++20, FFmpeg (libavcodec / libavformat / libswscale as linked by BreadCamera's `bc_io`), doctest; React 18 + TypeScript, Vitest.

**Spec:** `F:\Coding\BreadMedia\BreadReplay\docs\superpowers\specs\2026-11-05-breadreplay-editing-design.md` §8 (and §3 constraints). Plan 3 of 4; needs plans 1 and 2 merged (`playEvents`, `displayOrder`, `orderOf`, the freezing `Player` with `cueCopy`, `selectedIds`).

Repo: `F:\Coding\BreadMedia\BreadReplay`, branch `br-editing-export` from `main`, merged back with `--no-ff`. Never push. Change no other repo (BreadCamera's `openH264Encoder` is used as is).

## Global Constraints

- Export never shares the on-air player and never preempts it: its own device, decode lane, renderer and encoder; its thread runs at `THREAD_PRIORITY_BELOW_NORMAL`.
- The product FFmpeg has **no libx264**. Encoders: H.264 via `bc::openH264Encoder` (NVENC → AMF → QSV → MF); H.265 tries `hevc_nvenc`, `hevc_amf`, `hevc_qsv`, `hevc_mf` in that order. H.265 is offered only when one of them opens.
- MP4. Presets at 1080p30: H.264 High 20 / Medium 10 / Small 5 Mbps; H.265 12 / 6 / 3 Mbps; scaled by `(w × h × fps) / (1920 × 1080 × 30)`. Or an exact Mbps. Audio AAC 48 kHz stereo 192 kb/s. The show's raster and rate.
- File: `<Group or Selection or replay name> - YYYY-MM-DD HHmm.mp4` in the export folder (Settings), never overwriting (adds ` (2)`, ` (3)`…). A failed or cancelled export leaves no file under the final name and no `.part`.
- Several exports queue and run one at a time; progress and Cancel in the status bar. The export panel remembers the last codec and quality.
- Rendered exactly as it plays: cuts, speeds, zooms, Fade/Cut, audio as played (muted below 100 %).
- UI text ≥ 11 px; no modal (the export panel is a non-modal popover).
- Tests: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1`, then `./build/release/br_tests.exe`; UI: `npm --prefix ui test`, `npm --prefix ui run build`.

## Review Focus

1. A replay crossing a recording gap: the export keeps its frame count (held picture, as on air) and does not fail. → Task 3 test.
2. The export folder is missing or not writable: the job fails with a reason, and no `.part` is left. → Task 3 test.
3. The app closes or a show opens while an export runs: the job stops and its `.part` is deleted. → Task 3 test.
4. A replay is edited or deleted after its export was queued: the export uses the copy taken at `export.start`. → Task 3 test.
5. 59.94 fps and an odd raster (1366 × 768): timestamps stay exact over a minute and the encoder gets even dimensions. → Tasks 1 and 2 tests.

---

### Task 1: Bitrate and file-name rules

**Files:**
- Create: `src/export/export_rules.h`, `src/export/export_rules.cpp`, `tests/export_rules_test.cpp`
- Modify: `CMakeLists.txt` (sources to `br_core`, test to `br_tests`)

**Interfaces:**
- Produces:
  - `enum class ExportCodec { H264, Hevc };` `enum class ExportQuality { High, Medium, Small, Custom };`
  - `int64_t exportBitrate(ExportCodec, ExportQuality, std::optional<double> mbps, uint32_t w, uint32_t h, bc::Rate rate);` — bits/s; `Custom` uses `mbps` (must be > 0, else throws `std::invalid_argument`).
  - `std::string sanitizeFileName(std::string);` — replaces `<>:"/\|?*` and control characters with `_`, trims trailing dots and spaces, empty → `Export`.
  - `std::filesystem::path exportPath(const std::filesystem::path& folder, const std::string& name, const std::tm& local);` — `<sanitized name> - YYYY-MM-DD HHmm.mp4`, then ` (2)`, ` (3)` … before `.mp4` while a file with that name or its `.part` exists.
  - `ExportCodec parseCodec(const std::string&)` (`"h264"` / `"hevc"`), `ExportQuality parseQuality(const std::string&)` (`"high"`, `"medium"`, `"small"`, `"custom"`), both throwing `std::invalid_argument` on anything else.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("Export rules: presets scale with pixels and frame rate") {
  using br::ExportCodec, br::ExportQuality;
  CHECK(br::exportBitrate(ExportCodec::H264, ExportQuality::High, {}, 1920, 1080, {30, 1}) == 20'000'000);
  CHECK(br::exportBitrate(ExportCodec::H264, ExportQuality::High, {}, 1920, 1080, {60, 1}) == 40'000'000);
  CHECK(br::exportBitrate(ExportCodec::Hevc, ExportQuality::Small, {}, 1280, 720, {30, 1}) == 1'333'333);
  CHECK(br::exportBitrate(ExportCodec::Hevc, ExportQuality::Medium, {}, 1920, 1080, {60000, 1001}) == 11'988'012);
  CHECK(br::exportBitrate(ExportCodec::H264, ExportQuality::Custom, 8.5, 1920, 1080, {30, 1}) == 8'500'000);
  CHECK_THROWS_AS(br::exportBitrate(ExportCodec::H264, ExportQuality::Custom, 0.0, 1920, 1080, {30, 1}), std::invalid_argument);
}

TEST_CASE("Export rules: names are safe and never overwrite") {
  bc::test::TempDir dir;
  std::tm t{}; t.tm_year = 126; t.tm_mon = 10; t.tm_mday = 5; t.tm_hour = 21; t.tm_min = 7;
  CHECK(br::exportPath(dir.path, "Game 1: Final?", t).filename() == "Game 1_ Final_ - 2026-11-05 2107.mp4");
  std::ofstream(dir.path / "Game 1 - 2026-11-05 2107.mp4") << "x";
  std::ofstream(dir.path / "Game 1 - 2026-11-05 2107 (2).mp4.part") << "x";
  CHECK(br::exportPath(dir.path, "Game 1", t).filename() == "Game 1 - 2026-11-05 2107 (3).mp4");
  CHECK(br::sanitizeFileName(" . ") == "Export");
}
```
(Integer results truncate toward zero.)

- [ ] **Step 2: Run and see them fail** — `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe -tc="Export rules*"`; build errors.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — the two cases pass.
- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/export/export_rules.h src/export/export_rules.cpp tests/export_rules_test.cpp
git commit -m "feat(export): bitrate presets and safe file names"
```

---

### Task 2: Encoders and the MP4 writer

**Files:**
- Create: `src/export/export_encoder.h/.cpp`, `src/export/mp4_writer.h/.cpp`, `tests/mp4_writer_test.cpp`
- Modify: `CMakeLists.txt` (link libswscale / libavformat through the same FFmpeg targets `bc_io` links; look them up in `../BreadCamera/CMakeLists.txt` and its `cmake/` folder)

**Interfaces:**
- Consumes: Task 1's `ExportCodec`; `bc::openH264Encoder(w, h, fpsNum, fpsDen, bitRate, gop, globalHeader)` from `../BreadCamera/src/io/h264_encoder.h`.
- Produces:
  - `bc::OpenedEncoder openExportEncoder(ExportCodec, uint32_t w, uint32_t h, bc::Rate, int64_t bitRate);` — GOP = 2 s of frames, `globalHeader = true`. HEVC: the order in Global Constraints with the same per-encoder options BreadCamera uses for H.264 (NV12 input, no B-frames). Throws `std::runtime_error` listing every attempt.
  - `const std::vector<ExportCodec>& availableExportCodecs();` — probed once (open and free a 640 × 360 encoder of each codec), cached.
  - `struct Mp4WriterOptions { std::filesystem::path finalPath; ExportCodec codec; int64_t bitRate; uint32_t w, h; bc::Rate rate; };`
  - `class Mp4Writer final : public bc::Output` — `needsCpu() == true`, `wantsGpuRgba8() == false`; `submit(OutFrame)` converts `bgra` to NV12 (swscale; width and height rounded **down** to even for the encoder, the source cropped by at most one pixel), encodes video with pts = frame index in time base `1/rate`, and feeds `audio` (48 kHz interleaved float stereo) to FFmpeg's native AAC encoder (`avcodec_find_encoder(AV_CODEC_ID_AAC)`, `fltp`, 192 kb/s, 1024-sample frames, pts in samples). Writes `<finalPath>.part` with the `mp4` muxer and `movflags=+faststart`.
  - `void finish();` — flushes both encoders, writes the trailer, renames `.part` → final. `void abort();` — closes and deletes `.part`. `const std::optional<std::string>& error() const;` — the first failure; after one, `submit` does nothing and `finish` behaves as `abort`. The destructor calls `abort()` unless `finish()` succeeded.

- [ ] **Step 1: Write the failing tests** (`tests/mp4_writer_test.cpp`; skip with BreadCamera's `support/requires.h` helpers when no H.264 encoder opens, as other GPU tests do). Helper `probe(path)` opens the file with libavformat and returns `{videoFrames, videoCodec, width, height, durationS, audioRate, audioChannels}`; `feed(writer, n, rate)` submits `n` solid-colour 1366 × 768 BGRA frames with the samples `bc::TickSchedule::audioFramesForTick` gives:
  - `"Mp4Writer: writes H.264 + AAC and renames on finish"` — 60 frames at 30 fps → 60 video frames, `h264`, 1366 × 768 (even, as given), duration 2.0 ± 0.05 s, AAC 48000 Hz, 2 channels; no `.part` remains.
  - `"Mp4Writer: abort leaves nothing"` — 10 frames then `abort()` → neither the final file nor `.part` exists.
  - `"Mp4Writer: 59.94 fps keeps exact timestamps"` — 3600 frames at `{60000, 1001}` → duration `3600 × 1001 / 60000` ± 1 frame.
  - `"Mp4Writer: H.265 when available"` — run only if `availableExportCodecs()` contains `Hevc` → codec `hevc`.
- [ ] **Step 2: Run and see them fail** — build errors.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — `./build/release/br_tests.exe -tc="Mp4Writer*"`, all pass (or skip with a logged reason on a PC without the encoder).
- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/export/export_encoder.h src/export/export_encoder.cpp src/export/mp4_writer.h src/export/mp4_writer.cpp tests/mp4_writer_test.cpp
git commit -m "feat(export): GPU encoders and MP4 writer"
```

---

### Task 3: The export queue and its commands

**Files:**
- Create: `src/export/export_queue.h/.cpp`, `tests/export_queue_test.cpp`
- Modify: `src/events/show_file.h/.cpp` (settings), `src/engine/engine.h/.cpp`, `src/engine/command_router.cpp`, `CMakeLists.txt`
- Test: `tests/router_test.cpp`, `tests/show_file_test.cpp`

**Interfaces:**
- Consumes: Tasks 1–2; `OutputPlayer::playEvents`, `idle()`, `tickOnce()`; `Player(…, recordCuts = false, freeze = true)`; `FrameSource(stores, gpu, pool, threads)`; `Renderer` as `Engine` builds it.
- Produces:
  - `struct ExportJob { std::string name; std::vector<Event> events; ExportCodec codec; int64_t bitRate; std::filesystem::path path; };` — `events` are **copies** in play order, taken at `export.start`.
  - `struct ExportState { int id; std::string name, state, error, path; double progress; };` — `state` ∈ `queued`, `running`, `done`, `failed`, `cancelled`; `progress` 0..1.
  - `class ExportQueue { public: ExportQueue(std::map<std::string, const InputStore*> stores, ShowSettings settings); ~ExportQueue(); int add(ExportJob); void cancel(int id); std::vector<ExportState> states() const; void stop(); };` — one worker thread (`THREAD_PRIORITY_BELOW_NORMAL`), FIFO; keeps the last 5 finished jobs in `states()`. `stop()` / the destructor cancel the running job (its `.part` deleted) and join.
  - Per job: a new `bc::GpuDevice` (`Pref::HardwareOrWarp`), `TexturePool`, `Converter`, `FrameSource(stores, gpu, pool, 2)`, `Renderer`; a private `EventStore` holding only the copies (build it through `EventStore::fromJson` with those events in `rootOrder`); `Player(store, rate, false, true)`; `OutputPlayer` with one `Mp4Writer`; `playEvents(ids)`; loop `tickOnce()` until `idle()` or cancelled. Progress = ticks done / total ticks, total = Σ each event's `remainingSeconds()` from its In (a scratch freezing `Player`) × fps, minus fade overlaps.
  - `ShowSettings` gains `exportFolder` (default empty = `<show folder>/Exports`, created on first export), `exportCodec` (`"h264"`), `exportQuality` (`"high"`), `exportMbps` (`10`); saved in the show file and set via `settings.set`.
  - Commands: `export.start {listId? | ids? | id?, codec?, quality?, mbps?, name?}` → `{ok, id}`. Which: `ids` → in `displayOrder()`, default name `Selection`; `listId` (`""` = root) → `orderOf`, default name the group's name (`Replays` for the root); `id` → that replay, default name its name. Codec/quality default to the saved settings. Unknown or empty → `{ok:false, error}`. `export.cancel {id}`.
  - `statusJson()` adds `"exports": [ExportState…]` and `"exportCodecs": ["h264", "hevc"?]`.

- [ ] **Step 1: Write the failing tests** (`tests/export_queue_test.cpp`; build stores as `output_player_test.cpp`'s `Fixture` does, with two inputs: `cam1` red frames, `cam2` blue frames, 6 s at 30 fps; skip without an H.264 encoder):
  - `"Export: a replay with cuts, speed and a fade exports frame-accurate"` — two events: A (1 s–3 s, `cam1`, cut to `cam2` at 2 s, speed 0.5 from 2.5 s) and B (4 s–5 s, `cam2`); settings `transition = "fade"`, `fadeS = 0.2`. Reference: an `OutputPlayer` on the same copies with `RecordingOutput` (from `output_player_test.cpp`; move it to `tests/support/recording_output.h` if needed) counts N ticks to idle. Export → the MP4 has N video frames; the mean colour of decoded frame 5 is red and of frame N − 5 is blue (channel ±16 of the reference frame's BGRA).
  - `"Export: the output player keeps its pace while exporting"` — the engine-style `OutputPlayer::start()` at 60 fps with a counting fake output, plus a 10 s export → over 2 s the on-air player ticks ≥ 114 times.
  - `"Export: a recording gap keeps the frame count"` — an event across a 1 s gap in `cam1` → frame count equals the reference's.
  - `"Export: cancel leaves no file"` — cancel after the first progress update → state `cancelled`, no final file, no `.part`.
  - `"Export: an unwritable folder fails with a reason"` — `path` inside a folder that is a regular file → state `failed`, `error` non-empty, no `.part`.
  - `"Export: stop during a job deletes its part"` — destroy the queue mid-job → no `.part`.
  - `"Export: later edits do not change a queued export"` — `export.start` through the router, then `event.delete` the replay → the job still finishes `done`.
  - `router_test.cpp` `"Router: export.start names a group export after the group"` — `state.name == "Game 1"`, `path` ends with `.mp4` (skip without an encoder); `{cmd:'export.start'}` with nothing to export → `ok == false`.
  - `show_file_test.cpp`: the four export settings round-trip.
- [ ] **Step 2: Run and see them fail** — build errors.
- [ ] **Step 3: Implement.** Mark in `export_queue.cpp`: `// ponytail: frames go GPU -> CPU BGRA -> NV12 (swscale); a GPU NV12 path if export speed matters.`
- [ ] **Step 4: Run** — `./build/release/br_tests.exe`, all pass.
- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/export src/events/show_file.h src/events/show_file.cpp src/engine tests
git commit -m "feat(export): background export queue and commands"
```

---

### Task 4: Export panel and status (UI)

**Files:**
- Create: `ui/src/dialogs/ExportPanel.tsx`, `ui/src/dialogs/ExportPanel.test.tsx`
- Modify: `ui/src/api/types.ts`, `ui/src/screen/EventList.tsx` (+ test), `ui/src/screen/StatusBar.tsx` (+ test), `ui/src/dialogs/RecordingTab.tsx` (+ test), `ui/src/App.tsx`, `ui/src/app.css`

**Interfaces:**
- Consumes: Task 3's commands and status fields.
- Produces:
  - Types: `ExportState { id: number; name: string; state: 'queued'|'running'|'done'|'failed'|'cancelled'; progress: number; error: string; path: string }`; `Status.exports?: ExportState[]`, `Status.exportCodecs?: Array<'h264'|'hevc'>`; `ShowSettings.exportFolder/exportCodec/exportQuality/exportMbps`.
  - `ExportPanel` props `{ client, what: { listId?: string; ids?: number[]; id?: number; name: string }, codecs: Array<'h264'|'hevc'>, last: { codec, quality, mbps }, onClose }` — a non-modal popover (no backdrop; Escape or outside click closes; hotkeys keep working outside its fields). Fields: name (prefilled), codec `H.264` / `H.265` (H.265 hidden when not in `codecs`), quality `High` / `Medium` / `Small` / `Custom` with an Mbps number field (Custom only; Start disabled unless > 0). Start → `export.start {…what, name, codec, quality, mbps?}` and `settings.set {exportCodec, exportQuality, exportMbps}`, then closes.
  - Entry points: each group header gets an Export button beside Play; with 2+ selected, `Export selection` beside `Play selection`; the replay context menu gets `Export…`.
  - Status bar: while a job runs, `Exporting <name> · 42 %` and a Cancel button (aria-label `Cancel export`, sends `export.cancel {id}`); `+2 queued` when more wait; a failed job shows `Export failed: <error>` until the next job starts; a finished job shows `Exported <name>` for 10 s.
  - Recording tab: an `Export folder` text field (empty = the show's `Exports` folder) → `settings.set {exportFolder}`.

- [ ] **Step 1: Write the failing tests**
  - `ExportPanel.test.tsx`: `'H.265 is hidden when no encoder offers it'`; `'Custom needs a bitrate above zero'`; `'Start sends the export and remembers the choice'` → `{cmd:'export.start', listId:'g1', name:'Game 1', codec:'hevc', quality:'small'}` and `{cmd:'settings.set', exportCodec:'hevc', exportQuality:'small', exportMbps:10}`; `'opens on the last codec and quality'`.
  - `EventList.test.tsx`: `'a group's Export opens the panel for that group'`.
  - `StatusBar.test.tsx`: `'a running export shows progress and cancels'` → text `Exporting Game 1 · 42 %`, Cancel sends `{cmd:'export.cancel', id:7}`; `'a failed export says why'`.
  - `RecordingTab.test.tsx`: `'the export folder is a setting'`.
- [ ] **Step 2: Run and see them fail** — `npm --prefix ui test`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — `npm --prefix ui test; npm --prefix ui run build`, all pass.
- [ ] **Step 5: Commit**

```bash
git add ui/src/dialogs/ExportPanel.tsx ui/src/dialogs/ExportPanel.test.tsx ui/src/api/types.ts ui/src/screen ui/src/dialogs/RecordingTab.tsx ui/src/dialogs/RecordingTab.test.tsx ui/src/App.tsx ui/src/app.css
git commit -m "feat(ui): export panel and export status"
```

---

### Task 5: Merge

- [ ] **Step 1:** Full check on the branch: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe; npm --prefix ui test; npm --prefix ui run build`. All pass.
- [ ] **Step 2:** Make the frame-accurate test keep its MP4 when the environment variable `BR_KEEP_EXPORT` names a folder; run it once with `BR_KEEP_EXPORT` set to `F:\Coding\BreadMedia\BreadReplay\build\export-check` and write the file's path in the ledger, so the reviewer can open it.
- [ ] **Step 3:** `git checkout main; git merge --no-ff br-editing-export -m "merge: export to MP4"`; full check again on `main`. Do not push.
