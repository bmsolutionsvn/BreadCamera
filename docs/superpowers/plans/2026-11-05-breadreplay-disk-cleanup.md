# BreadReplay Disk Cleanup Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Clean up button frees the disk of every recorded picture and sound no replay uses, on every camera, keeping each replay's In–Out ± 3 s, never touching the segments being written or the last 5 minutes.

**Architecture:** A pure planner compares each closed segment with the kept ranges (all replays ± 3 s, merged) and decides keep / delete / rewrite. A rewrite copies the frames inside the kept ranges (each range starting at the keyframe at or before it) into a temporary segment with `SegmentWriter`, then `InputStore::replaceSegment` swaps it in under the store lock and bumps a generation counter that tells `FrameSource` to drop that input's decode state. A `CleanupRunner` does this one segment at a time on a below-normal thread.

**Tech Stack:** C++20, doctest; React 18 + TypeScript, Vitest.

**Spec:** `F:\Coding\BreadMedia\BreadReplay\docs\superpowers\specs\2026-11-05-breadreplay-editing-design.md` §9. Plan 4 of 4; needs plans 1–3 merged (it adds to the same status bar and engine status).

Repo: `F:\Coding\BreadMedia\BreadReplay`, branch `br-editing-cleanup` from `main`, merged back with `--no-ff`. Never push. Change no other repo.

## Global Constraints

- Kept: for every replay, on **every** camera, In to Out plus **3 s** each side. Compressed (H.264/HEVC) kept ranges start at the keyframe at or before the range (up to one GOP early); VMX frames are all keyframes.
- Never touched: segments being written, and any segment with a frame in the last **5 minutes** of the recording (`newestTs − 300 s`).
- One confirmation before it runs, showing how much disk it frees and that it cannot be undone. It is the only modal in the editing design and lives outside the live controls (the status bar's disk area).
- Runs in the background, below the players' priority; never stalls Preview or Output; a replay that is playing keeps playing.
- Afterwards the timeline shows the removed stretches as gaps (coverage already does this once the index changes).
- Tests: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1`, then `./build/release/br_tests.exe`; UI: `npm --prefix ui test`, `npm --prefix ui run build`.

## Review Focus

1. A replay marked while cleanup runs (reviewing an older moment): the runner re-reads the kept ranges before each segment, so it is not cut away. → Task 3 test.
2. The segment Preview or Output is reading is swapped: playback continues with correct pictures (the decoder resets), no crash. → Task 2 test.
3. A crash mid-rewrite leaves a temporary segment: the store ignores and deletes it on the next load, and the original stays whole. → Task 2 test.
4. Retention deletes a segment the runner has planned: the runner skips it without failing. → Task 3 test.
5. An open mark (In only, `outUs == inUs`) or an event with Out before In: its ranges are still kept, using min/max. → Task 1 test.

---

### Task 1: Kept ranges and the cleanup plan (pure)

**Files:**
- Create: `src/store/cleanup.h`, `src/store/cleanup.cpp`, `tests/cleanup_test.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces:
  - `std::vector<ProtectedRange> keptRanges(const std::vector<Event>& events, int64_t marginUs);` — per event `[min(in,out) − margin, max(in,out) + margin]`, sorted, overlaps and touching ranges merged.
  - `enum class CleanupAction { Keep, Delete, Rewrite };`
  - `struct SegmentPlan { std::string inputId; std::filesystem::path base; CleanupAction action; std::vector<ProtectedRange> keep; int64_t bytesFreed; };` — `keep` (Rewrite only) is the kept ranges clipped to the segment, each start moved back to the keyframe at or before it.
  - `std::vector<SegmentPlan> planCleanup(const std::map<std::string, const InputStore*>& stores, std::span<const ProtectedRange> kept, int64_t protectFromUs);` — per closed segment (not `isWriting`): any frame at or after `protectFromUs` → Keep; no frame inside `kept` → Delete (`bytesFreed` = the segment's `bytes`); every frame inside → Keep; else Rewrite. `bytesFreed` for Rewrite = Σ dropped frames' `videoLen + audioLen + 24 + 40`. Keep entries are not returned.
  - `int64_t totalFreed(const std::vector<SegmentPlan>&);`
  - Constants `kCleanupMarginUs = 3'000'000`, `kCleanupProtectUs = 300'000'000`.

- [ ] **Step 1: Write the failing tests** (`tests/cleanup_test.cpp`; build stores in a `TempDir` with `InputStore::append` at 10 fps, closing segments as `segment_test.cpp` / `input_store_test.cpp` do):
  - `"Cleanup: kept ranges add the margin and merge"` — events `[10 s, 12 s]`, `[14 s, 15 s]`, and an open mark `in == out == 40 s`, and one with `out < in` (`[52 s, 50 s]`) → `[7, 18]`, `[37, 43]`, `[47, 55]` (seconds).
  - `"Cleanup: a VMX segment around one replay is rewritten to the kept range"` — one closed VMX segment 0–60 s, kept `[17 s, 33 s]` → one Rewrite, `keep == {[17 s, 33 s]}`, `bytesFreed` equals the summed sizes of the frames outside.
  - `"Cleanup: an H.264 segment keeps from the keyframe before"` — keyframes every 2 s (flag on every 20th frame), kept starts at 17 s → `keep[0].inUs == 16 s`.
  - `"Cleanup: untouched, open and recent segments are not planned; unused ones are deleted"` — segment A 0–60 s with no replay → Delete; the open segment → absent; segment B whose last frame is after `protectFromUs` → absent.

- [ ] **Step 2: Run and see them fail** — `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe -tc="Cleanup*"`; build errors.
- [ ] **Step 3: Implement** (read records with `SegmentReader::open(base)`).
- [ ] **Step 4: Run** — the four cases pass.
- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/store/cleanup.h src/store/cleanup.cpp tests/cleanup_test.cpp
git commit -m "feat(store): plan what disk cleanup keeps"
```

---

### Task 2: Rewrite a segment and swap it in

**Files:**
- Modify: `src/store/cleanup.h/.cpp`, `src/store/input_store.h/.cpp`, `src/play/frame_source.h/.cpp`
- Test: `tests/cleanup_test.cpp`, `tests/frame_source_test.cpp`

**Interfaces:**
- Consumes: Task 1's `SegmentPlan`.
- Produces:
  - `std::filesystem::path rewriteSegment(const std::filesystem::path& base, std::span<const ProtectedRange> keep);` — writes `<base>.cleanup` (`.vmxs` / `.vmxi`) with `SegmentWriter`, same header, copying each kept frame's full essence (`readVideo(i, false)`, its `previewLen`, `readAudio(i)`, keyframe flag, `srcPtsUs`) in order; returns the new base. Throws on I/O error, deleting what it wrote.
  - `bool InputStore::replaceSegment(const std::filesystem::path& base, const std::filesystem::path& newBase);` — refuses the writing segment and an unknown base (false). Under the store lock: closes the old reader, renames the old files to `<base>.old`, the new ones to `base`, reopens the reader, deletes the `.old` files, recomputes the index and coverage, increments `generation()`.
  - `uint64_t InputStore::generation() const;` — also incremented by `removeSegment`.
  - `InputStore` load: files named `*.cleanup.vmxs/.vmxi` or `*.old.vmxs/.vmxi` are deleted; if a base has only `.old` files (crash between renames), they are renamed back.
  - `FrameSource`: per input, remembers the store's `generation()`; on `get` / `prefetch` / `preview`, a changed generation drops that input's decoders (`Dec` entries, all lanes), cached frames and GOP tables before serving.

- [ ] **Step 1: Write the failing tests**
  - `cleanup_test.cpp` `"Cleanup: a rewritten segment keeps exactly the kept frames, byte for byte"` — VMX segment 0–60 s, keep `[17 s, 33 s]`, then `replaceSegment` → `find(20 s)` returns a frame whose `readVideo` equals the one read before the swap; `isGapAt(40 s)` is true; `coverage()` has a split at 33 s.
  - `"Cleanup: an H.264 rewrite stays decodable"` — an H.264 store (use the coded test media helpers `frame_source_test.cpp` uses), keep from mid-GOP, swap, then `FrameSource::get` at the kept start returns a picture (`!inGap`).
  - `"Cleanup: a store left mid-swap recovers on load"` — create `<base>.old.*` with no `<base>.*`, plus a stray `<other>.cleanup.*`; open the store → the `.old` segment is back under `base`, the stray files are gone.
  - `frame_source_test.cpp` `"FrameSource: a swapped segment resets the decoder"` — `get(cam, 20 s)` (primes a decoder), swap via `replaceSegment`, `get(cam, 21 s)` returns the right frame (compare to a fresh `FrameSource`).
- [ ] **Step 2: Run and see them fail** — build errors.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — `./build/release/br_tests.exe`, all pass.
- [ ] **Step 5: Commit**

```bash
git add src/store src/play/frame_source.h src/play/frame_source.cpp tests/cleanup_test.cpp tests/frame_source_test.cpp
git commit -m "feat(store): rewrite a segment to what replays use and swap it in"
```

---

### Task 3: The cleanup runner and its commands

**Files:**
- Create: `src/store/cleanup_runner.h`, `src/store/cleanup_runner.cpp`
- Modify: `src/engine/engine.h/.cpp`, `src/engine/command_router.cpp`, `CMakeLists.txt`
- Test: `tests/cleanup_test.cpp`, `tests/router_test.cpp`

**Interfaces:**
- Consumes: Tasks 1–2.
- Produces:
  - `struct CleanupStatus { std::string state; double progress; int64_t bytesFreed; std::string error; };` — `state` ∈ `idle`, `running`, `done`, `failed`.
  - `class CleanupRunner { public: using KeptFn = std::function<std::vector<ProtectedRange>()>; CleanupRunner(std::map<std::string, InputStore*> stores, KeptFn kept, std::function<int64_t()> newestTs, int64_t protectUs = kCleanupProtectUs); ~CleanupRunner(); bool start(); CleanupStatus status() const; void stop(); };` — `start()` returns false while running. The thread (`THREAD_PRIORITY_BELOW_NORMAL`) plans once, then for each planned segment: calls `kept()` again and re-plans that one segment (a replay made meanwhile changes it; Keep → skip), skips a base that no longer exists, then deletes (`removeSegment`) or rewrites + `replaceSegment`. `bytesFreed` counts what was actually freed. One failure stops the run with `failed` and its reason; finished segments stay done.
  - Engine: owns one `CleanupRunner` (kept = `keptRanges(events, kCleanupMarginUs)` read under the engine's mutex); stops it in `stop()` and `openShow()`. Commands: `cleanup.plan` → `{ok, bytesFreed, segments}` (no change on disk); `cleanup.run` → `{ok}` or `{ok:false, error:"cleanup is already running"}`. `statusJson()` adds `"cleanup": CleanupStatus`.

- [ ] **Step 1: Write the failing tests**
  - `cleanup_test.cpp` `"Cleanup: the runner frees what the plan said"` — two closed segments (one unused, one around a replay), `protectUs = 0` → state `done`, `bytesFreed == totalFreed(plan)`, the unused segment is gone.
  - `"Cleanup: a replay made during the run is kept"` — `kept()` returns nothing on its first call and `[20 s, 25 s]` afterwards → the segment containing 20 s is rewritten (not deleted) and `find(22 s)` still returns a frame.
  - `"Cleanup: a segment removed meanwhile is skipped"` — delete one planned segment's files before `start()` returns its first progress (use a `kept` callback that removes it on the second call) → state `done`, no error.
  - `router_test.cpp` `"Router: cleanup.plan reports nothing to free while everything is recent"` → `bytesFreed == 0`, `segments == 0`; `"Router: cleanup.run twice reports it is running"` (second call while the first runs, or `ok` twice if the first already finished — assert one of the two exact replies).
- [ ] **Step 2: Run and see them fail** — build errors.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — `./build/release/br_tests.exe`, all pass.
- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/store/cleanup_runner.h src/store/cleanup_runner.cpp src/engine tests/cleanup_test.cpp tests/router_test.cpp
git commit -m "feat(engine): background disk cleanup with plan and run commands"
```

---

### Task 4: Clean up button and confirmation (UI)

**Files:**
- Create: `ui/src/dialogs/CleanupConfirm.tsx`, `ui/src/dialogs/CleanupConfirm.test.tsx`
- Modify: `ui/src/api/types.ts` (`Status.cleanup?: { state: 'idle'|'running'|'done'|'failed'; progress: number; bytesFreed: number; error: string }`), `ui/src/screen/StatusBar.tsx` (+ test), `ui/src/format.ts` (+ test), `ui/src/App.tsx`, `ui/src/app.css`

**Interfaces:**
- Consumes: Task 3's commands and status.
- Produces:
  - `formatBytes(n: number): string` — `0 B`, `512 KB`, `12.3 GB` (1024-based, one decimal from MB up).
  - Status bar disk area: a `Clean up` button. Click → `cleanup.plan`; `bytesFreed == 0` → the button shows `Nothing to free` for 3 s and no dialog; otherwise `CleanupConfirm` opens.
  - `CleanupConfirm` props `{ bytes: number; onConfirm(): void; onCancel(): void }` — a modal (`role="dialog"`, `aria-modal="true"`, focus on Cancel, Escape cancels): title `Free ${formatBytes(bytes)}?`, one line `This cannot be undone.`, buttons `Clean up` (sends `cleanup.run`) and `Cancel`. While it is open, hotkeys are paused as for Settings.
  - While running: `Cleaning up · 40 %`, the button disabled; `done`: `Freed 12.3 GB` for 10 s; `failed`: `Cleanup failed: <error>` until the next run.

- [ ] **Step 1: Write the failing tests**
  - `format.test.ts`: `formatBytes(0) === '0 B'`, `formatBytes(524288) === '512 KB'`, `formatBytes(13207024435) === '12.3 GB'`.
  - `CleanupConfirm.test.tsx`: `'shows what it frees and confirms'` → text `Free 12.3 GB?`, `This cannot be undone.`; `Clean up` calls `onConfirm`; Escape calls `onCancel`.
  - `StatusBar.test.tsx`: `'Clean up asks the engine first and confirms before running'` (send `cleanup.plan` resolves `{ok:true, bytesFreed: 13207024435, segments: 4}` → dialog; confirm → `{cmd:'cleanup.run'}`); `'nothing to free opens no dialog'`; `'a running cleanup shows progress'`.
- [ ] **Step 2: Run and see them fail** — `npm --prefix ui test`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — `npm --prefix ui test; npm --prefix ui run build`, all pass.
- [ ] **Step 5: Commit**

```bash
git add ui/src/dialogs/CleanupConfirm.tsx ui/src/dialogs/CleanupConfirm.test.tsx ui/src/api/types.ts ui/src/screen/StatusBar.tsx ui/src/screen/StatusBar.test.tsx ui/src/format.ts ui/src/format.test.ts ui/src/App.tsx ui/src/app.css
git commit -m "feat(ui): Clean up button with one confirmation"
```

---

### Task 5: Merge

- [ ] **Step 1:** Full check on the branch: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe; npm --prefix ui test; npm --prefix ui run build`. All pass.
- [ ] **Step 2:** `git checkout main; git merge --no-ff br-editing-cleanup -m "merge: disk cleanup keeps only what replays use"`; full check again on `main`. Do not push.
