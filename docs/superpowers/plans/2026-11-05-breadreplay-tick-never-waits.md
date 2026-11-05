# BreadReplay: Ticks Never Wait, Frame-Rate Snapping Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Preview and Output ticks never block on the engine's command lock, timeline edits snap to the show's real frame rate, undo never reuses a replay id, exports work with Vietnamese paths, and three engine hazards found in review (show-open hang, input-removal crash, retention under an export) are closed.

**Why (review of merge 9b12447):** the multicam-timeline run fixed a store data race by making every `Player` method, including `tick()`, take `Engine::cmdM_`. That lock is held for whole commands, `saveShow()` (a disk write) and the 10 Hz `statusJson()`; plan 4 adds `cleanup.plan`, which reads every segment index under it. So a slow command can delay an on-air frame, which the editing spec's anti-goals forbid ("any stutter or delay on the on-air output caused by editing"). Separately, the timeline snaps on a hard-coded 100 ms (`PERIOD_US = 100000`) because status carries no frame rate, and `EventStore::restoreFrom` copies `nextId_`, so an undo followed by a mark reuses an id.

**Architecture:** Each `Player` gets its own short mutex for its state and keeps a private copy of the event it plays (Output: the frozen copy as today; Preview: a copy refreshed when the store's version changes). `tick()` reaches the store only through `try_lock`: when the command lock is busy it plays from its copy and leaves pass writes pending for the next tick that gets the lock. Command-thread methods already run under `cmdM_` and keep taking it as they do now.

**Tech Stack:** C++20, doctest; React 18 + TypeScript, Vitest.

**Spec:** `F:\Coding\BreadMedia\BreadReplay\docs\superpowers\specs\2026-11-05-breadreplay-editing-design.md` §2 anti-goals, §4.2 (snap to frames), §4.5–4.6. Follow-up to `2026-11-05-breadreplay-multicam-timeline.md`.

Repo: `F:\Coding\BreadMedia\BreadReplay`, branch `br-tick-never-waits` from `main`, merged back with `--no-ff`. Never push. Change no other repo.

## Global Constraints

- `Player::tick()` and `OutputPlayer::tickOnce()` never call a blocking lock on the store/command lock; only `try_lock`. Lock order everywhere: command lock → player mutex; `tick()` takes the player mutex, then only `try_lock`s the command lock.
- No pass write is lost: a deferred write lands on a later tick or at the next command-thread call.
- Every existing engine and UI test keeps passing unchanged, except where this plan names a change.
- Tests: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1`, then `./build/release/br_tests.exe`; UI: `npm --prefix ui test`, `npm --prefix ui run build`.

## Review Focus

1. A ride key arrives (command thread, holding the command lock) while `tick()` runs on the output thread: no deadlock, no torn state. → Task 1 stress test.
2. The command lock is held for 500 ms during a pass: the picture keeps moving and the pass reaches the store once the lock is free. → Task 1 test.
3. Preview shows a replay that the timeline just edited: the next tick that gets the lock uses the new In/Out and cuts. → Task 1 test.
4. `statusJson()` reads a player's state while it ticks: it reads a copy taken under the player mutex. → Task 1 (no torn reads; covered by the stress test under the debug runtime).
5. A 50 fps show: timeline drags land on 20 ms frames, `S` splits on the same grid. → Task 2 tests.

---

### Task 1: Players read a private copy and only try the store lock

**Files:**
- Modify: `src/play/player.h`, `src/play/player.cpp`, `src/play/output_player.cpp` (if it touches the store in `tickOnce`), `src/engine/engine.cpp` (status reads `Player::snapshot()`)
- Test: `tests/player_test.cpp`, `tests/output_player_test.cpp`

**Interfaces:**
- Produces:
  - `PlayerState Player::snapshot() const;` — a copy under the player mutex; `Engine::statusJson()` and the router use it instead of `state()` wherever another thread may tick the player. `state()` stays for single-threaded tests.
  - Private: `mutable std::mutex m_;` guarding `state_`, the copy, pass state; `std::optional<Event> copy_` (replaces `frozen_` for both modes: Output fills it at cue/restore/cueCopy and never refreshes it from the store; Preview refreshes it whenever it holds the store lock and `events_.version() != copyVersion_`); `bool storeDirty_` (a pass write is pending).
  - `tick()`: lock `m_`; `std::unique_lock store(*lock_, std::try_to_lock)` (or no lock when `lock_` is null, as in tests that pass none); if owned: refresh Preview's copy, then flush a pending pass write; play from `copy_` either way; after moving, apply the pass splice to `copy_` at once (so old cuts it plays over never fire) and to the store when owned, else set `storeDirty_`.
  - Command-thread methods (`cue`, `play`, `pause`, `setAngle`, …) are called with the command lock held by the router; they lock `m_`, refresh the copy, and flush `storeDirty_` first.

- [ ] **Step 1: Write the failing tests** (`tests/player_test.cpp`; a `std::recursive_mutex storeLock` passed to the `Player`; event `in 0`, `out 10 s`, `cam1@0`):

```cpp
TEST_CASE("Player: a tick never waits for the store lock") {
  // hold storeLock on another thread for 500 ms; meanwhile 10 ticks each return in < 5 ms
  // and posUs advances 10 periods.
}

TEST_CASE("Player: a pass made while the store is busy lands when it frees") {
  // event also has cam3@4s. Cue at 2 s, play, tick to 3 s, setAngle("cam2") (lock free).
  // Another thread now holds storeLock; tick to 5 s: every tick returns inputId "cam2"
  // (the copy has the splice), and the store still has cam3@4s. Release; one more tick:
  // the store has cam2@3s and no cut in (3 s, 5 s].
}

TEST_CASE("Player: Preview follows an edit on the next tick") {
  // cue (preview, not frozen); events.trim(id, 0, -5 * kS) under storeLock on the test
  // thread; next tick(): remainingSeconds() reflects out = 5 s.
}

TEST_CASE("Player: ride keys and ticks from two threads do not deadlock") {
  // output-style player (freeze) playing; thread A ticks 2000 times; thread B, holding
  // storeLock per call like the router, calls setAngle / setSpeedPreset 500 times;
  // both finish within 5 s; the store event's angles are sorted and unique by tUs.
}
```
Write the bodies with exact asserts as described; time with `std::chrono::steady_clock`.

`tests/output_player_test.cpp`: `"OutputPlayer: keeps pace while the command lock is held"` — `start()` at 60 fps with a counting output and a `Player` built with a lock; hold the lock for 1 s on the test thread → at least 57 ticks in that second.

- [ ] **Step 2: Run and see them fail** — `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe -tc="Player*,OutputPlayer*"`; the waiting tests time out or fail their time bounds.
- [ ] **Step 3: Implement** the interfaces above. Remove the `ponytail:` comment only if the ceiling it names is gone.
- [ ] **Step 4: Run all engine tests** — `./build/release/br_tests.exe`, all pass (run `Player*` 20 times: `for ($i=0;$i -lt 20;$i++){ ./build/release/br_tests.exe -tc="Player*" }`, 0 failures).
- [ ] **Step 5: Commit**

```bash
git add src/play src/engine/engine.cpp tests/player_test.cpp tests/output_player_test.cpp
git commit -m "fix(play): ticks never wait for the command lock"
```

---

### Task 2: Status carries the frame rate; the timeline snaps on it

**Files:**
- Modify: `src/engine/engine.cpp` (`statusJson`), `ui/src/api/types.ts`, `ui/src/App.tsx`, `ui/src/screen/Timeline.tsx`, `ui/src/screen/timelineEdits.ts`
- Test: `tests/engine_test.cpp` (or wherever status JSON is asserted), `ui/src/screen/Timeline.test.tsx`, `ui/src/App.test.tsx`

**Interfaces:**
- Produces: status `"rate": {"num": N, "den": D}`; `Status.rate?: { num: number; den: number }`. App: `fps = status.rate ? num / den : 60000 / 1001` replaces the `FPS` constant everywhere it is used. `Timeline` prop `periodUs: number` (App passes `Math.round(1e6 / fps)`) replaces `PERIOD_US`; `split` takes `periodUs` as a required argument (no default).

- [ ] **Step 1: Write the failing tests**
  - Engine: `"Engine: status carries the show rate"` → `statusJson()["rate"]["num"] == 30`, `["den"] == 1` for the test show.
  - `Timeline.test.tsx`: `'drags snap to the show frame at 50 fps'` — `periodUs={20_000}`, snapping off (Shift), Out dragged to x 903 of 1000 over a 10 s view → `outUs === 9_020_000`.
  - `App.test.tsx`: `'S splits on the show frame grid'` — status `rate {num:50, den:1}`, Preview at 4_031_000 → the new cut at 4_040_000.
- [ ] **Step 2: Run and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — engine and UI suites, UI build: all pass.
- [ ] **Step 5: Commit**

```bash
git add src/engine/engine.cpp ui/src tests
git commit -m "fix(timeline): snap to the show's frame rate"
```

---

### Task 3: Undo never reuses an id; no-op keys send nothing

**Files:**
- Modify: `src/events/event_store.cpp` (`restoreFrom`), `ui/src/App.tsx`
- Test: `tests/edit_history_test.cpp`, `ui/src/App.test.tsx`

**Interfaces:**
- Produces: `restoreFrom` keeps `nextId_ = max(current nextId_, snapshot nextId_)` (and the same for `nextListId_`). App's `ui.split` / `ui.delete` send nothing when the edit returns an event equal to the current one (compare `inUs`, `outUs`, `angles`, `speeds`, `zooms`).

- [ ] **Step 1: Write the failing tests**
  - `"History: a mark after an undo gets a new id"` — mark (id 1), undo, mark → id 2.
  - `App.test.tsx` `'S on an existing cut sends nothing'`.
- [ ] **Step 2: Run and see them fail.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — engine and UI suites, UI build: all pass.
- [ ] **Step 5: Commit**

```bash
git add src/events/event_store.cpp tests/edit_history_test.cpp ui/src/App.tsx ui/src/App.test.tsx
git commit -m "fix(edit): undo never reuses an id; no-op keys send nothing"
```

---

### Task 4: Export paths in any language

The export run's review (ledger `2026-11-05-breadreplay-export`) found that paths reach libavformat through `path::string()` (the ANSI code page), so a group renamed `Trận 1` or an export folder with Vietnamese letters fails or writes a mangled name. The owner works in Vietnamese.

**Files:** `src/export/mp4_writer.cpp` (and any other place that hands a path to FFmpeg), `tests/mp4_writer_test.cpp`

- [ ] **Step 1: Write the failing test** `"Mp4Writer: a Vietnamese folder and name work"` — folder `TempDir / u8"Trận đấu"`, final path `exportPath(folder, u8"Trận 1", t)`; write 10 frames, `finish()` → `std::filesystem::exists(final)` is true and no `.part` remains. Skip without an H.264 encoder, as the other writer tests do.
- [ ] **Step 2: Run and see it fail.**
- [ ] **Step 3: Implement:** hand FFmpeg the UTF-8 form (`reinterpret_cast<const char*>(path.u8string().c_str())`; FFmpeg's Windows file protocol converts UTF-8 to wide); rename `.part` → final with `std::filesystem::rename` on the `path` objects, never on narrow strings.
- [ ] **Step 4: Run** — writer tests pass.
- [ ] **Step 5: Commit** — `git add src/export tests/mp4_writer_test.cpp; git commit -m "fix(export): paths in any language"`

---

### Task 5: Opening a show cannot hang; removing an input cannot crash; exports keep their media

Also from the export run's review (pre-existing, confirmed by reading `engine.cpp`):
- `show.open` / `show.new` run under `cmdM_` and call `stop()`, which joins the status thread, while that thread may be blocked in `statusJson()` on `cmdM_`: a deadlock.
- `removeInput` (and `addInput` / `updateInput`) call `rebuildFrameSource()`, which replaces `source_` while the preview and output threads are reading it: a use-after-free.
- Retention can delete segments a queued or running export still needs.

**Files:** `src/engine/engine.cpp`, `src/play/frame_source.h/.cpp`, `src/export/export_queue.h/.cpp`, `tests/engine_test.cpp`, `tests/frame_source_test.cpp`, `tests/export_queue_test.cpp`

**Interfaces:**
- `statusLoop` builds its status with `cmdM_` taken by `try_to_lock`; when busy it skips that push (status is 10 Hz). `statusJson()` called from command handlers keeps working as now.
- `void FrameSource::setStores(std::map<std::string, const InputStore*> stores);` — swaps the map under the source's own lock and drops decoders and cached frames of inputs no longer present. `rebuildFrameSource()` calls it when `source_` exists instead of replacing the object.
- `std::vector<ProtectedRange> ExportQueue::protectedRanges() const;` — In − 3 s … Out + 3 s of every event in queued and running jobs. `Engine::retentionTick()` adds them to the ranges it keeps.

- [ ] **Step 1: Write the failing tests**
  - `engine_test.cpp` `"Engine: opening a show while status runs does not hang"` — with the status thread running and a sender set, `handle({cmd:'show.new'})` 10 times; each returns within 5 s (run the call on a `std::async` and `wait_for`).
  - `"Engine: removing and re-adding an input while Output plays does not crash"` — Output playing a replay; 20 × (`input.remove`, `input.add`) of a second input; the output player's `ticks()` keeps rising and the process survives.
  - `frame_source_test.cpp` `"FrameSource: setStores drops a removed input"` — `get(cam2, t)` returns null after `setStores` without `cam2`; `cam1` still serves.
  - `export_queue_test.cpp` `"Export: a queued export's media is protected"` — a job queued for an event at 20–25 s → `protectedRanges()` contains `[17 s, 28 s]`; after it finishes, empty.
- [ ] **Step 2: Run and see them fail** (the hang test times out; the crash test may crash: run it alone).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run all engine tests** — all pass.
- [ ] **Step 5: Commit** — `git add src tests; git commit -m "fix(engine): show open cannot hang, input removal cannot crash, exports keep their media"`

---

### Task 6: Merge

- [ ] **Step 1:** Full check on the branch: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe; npm --prefix ui test; npm --prefix ui run build`. All pass.
- [ ] **Step 2:** `git checkout main; git merge --no-ff br-tick-never-waits -m "merge: ticks never wait; frame-rate snapping"`; full check again on `main`. Do not push.
