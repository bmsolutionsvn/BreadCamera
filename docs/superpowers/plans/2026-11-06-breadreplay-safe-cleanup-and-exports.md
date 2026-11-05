# BreadReplay Safe Cleanup and Exports Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A crash or power cut during disk cleanup can never corrupt recorded footage, and adding, editing or removing an input no longer wipes the export list or leaves the cleanup runner pointing at a deleted store.

**Architecture:** Three fixes found reviewing last night's merges (ledgers `2026-11-05-breadreplay-export`, `2026-11-05-breadreplay-disk-cleanup`). (1) `InputStore` recovers a half-swapped segment as a pair and `replaceSegment` rolls back when a rename fails. (2) `ExportQueue::setStores` swaps the store map in place instead of the engine destroying and recreating the queue. (3) The engine stops the cleanup runner before a store dies and rebuilds it with the current stores after an input change.

**Tech Stack:** C++20, doctest, Win32.

**Spec:** `F:\Coding\BreadMedia\BreadReplay\docs\superpowers\specs\2026-11-05-breadreplay-editing-design.md` (export and cleanup sections). This plan only closes defects in what is already merged.

Repo: `F:\Coding\BreadMedia\BreadReplay`, branch `br-safe-cleanup` from `main` (`a940c18`), merged back with `--no-ff`. Never push. Never stage `ui/.impeccable/`.

## Global Constraints

- A segment is a pair, `<base>.vmxs` + `<base>.vmxi`. Footage on disk must always be either the whole original pair or the whole rewritten pair, never one of each.
- `SegmentWriter::close()` already flushes both files (`FlushFileBuffers`); do not add another flush.
- Export copy: a running export stopped by removing or editing an input ends `"failed"` with error `an input changed while it was exporting`. The status bar already shows `Export failed: <error>`, so no UI change.
- Queued and finished exports stay in the list across any input change. Adding an input never stops a running export.
- A cleanup run that is going when an input is added, edited or removed stops after the segment it is on (it never stops mid-segment) and the runner is rebuilt with the new stores; the operator can press Clean up again. No new UI copy.
- Build and test: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1`, then `./build/release/br_tests.exe` (filter with `--test-case="Cleanup*"` etc.). UI is untouched: do not run or fix the UI suite except in Task 4's full check.

## Review Focus

1. Power lost after the third of the four swap renames (new `.vmxs` in place, `.vmxi` still the original under `.old`): on restart the segment is the original pair. → Task 1 test.
2. A rename that fails midway (file held open by antivirus or the indexer): the original stays readable now, not only after a restart, and no `.old` / `.cleanup` file is left. → Task 1 test.
3. The operator renames a camera while a group exports, with two more exports queued: the running one says why it stopped; the queued two still run. → Task 2 test.
4. A camera added during an export: the export keeps going. → Task 2 test.
5. A camera removed while Clean up runs: no crash, and a camera added later is cleaned by the next run. → Task 3 test.

---

### Task 1: The segment swap is crash-safe and rolls back on failure

**Files:**
- Modify: `src/store/input_store.cpp` (the `.old` / `.cleanup` recovery at the top of the constructor, ~lines 40-66; `replaceSegment`, ~lines 245-272)
- Test: `tests/cleanup_test.cpp` (beside `"Cleanup: a store left mid-swap recovers on load"`)

**Interfaces:**
- Recovery (constructor), per segment base that has any `.old.vmxs` / `.old.vmxi`: if **both** `<base>.vmxs` and `<base>.vmxi` exist, the swap finished, so delete the `.old` files; otherwise, for each extension with an `.old` file, move it over `<base>.<ext>` (replacing a partial new file). Then delete every `.cleanup.vmxs` / `.cleanup.vmxi` as now. Replaces today's per-file rule, which restores each `.old` file on its own.
- `bool InputStore::replaceSegment(const std::filesystem::path& base, const std::filesystem::path& newBase)` keeps its signature. It uses `std::error_code` renames. If any of the four renames fails, it undoes the ones already done in reverse order, reopens the reader on the original pair (rebuilding `times` and `bytes` as the success path does), deletes the `newBase` files, and returns `false`. Success is unchanged. The runner already counts nothing freed on `false`.

- [ ] **Step 1: Write the failing tests**
  - `"Cleanup: a store left after the third swap rename keeps the original"`: record 20 frames (`record(store, base, 20)`), take `segmentBase = segments()[0].base` and `n = recordsOf(segmentBase).size()`. Call `br::rewriteSegment(segmentBase, {{base, base + 500'000}})` while the store is open, then let the store go out of scope. By hand: rename `segmentBase.vmxs` to `.old.vmxs`, `segmentBase.vmxi` to `.old.vmxi`, and `<newBase>.vmxs` to `segmentBase.vmxs`, leaving `<newBase>.vmxi`. Reopen `br::InputStore(dir.path, proto())`. Then `recordsOf(segmentBase).size() == n`, and none of `segmentBase.old.vmxs`, `segmentBase.old.vmxi`, `<newBase>.vmxs` or `<newBase>.vmxi` exists.
  - `"Cleanup: a swap that cannot finish leaves the original readable"`: record 20 frames, `n` as above, `newBase = br::rewriteSegment(segmentBase, {{base, base + 500'000}})`. Hold `<newBase>.vmxi` open with `CreateFileW(..., GENERIC_READ, 0 /*no sharing*/, ...)` so the fourth rename fails. Then `CHECK_FALSE(store.replaceSegment(segmentBase, newBase))`. While the handle is still open:
    - `store.find(base + 1'000'000)` has a value, and `store.readVideo(*ref, false)` is not empty;
    - `recordsOf(segmentBase).size() == n`;
    - `segmentBase.old.vmxs` and `.old.vmxi` do not exist.

    Close the handle. `<newBase>.vmxs` does not exist (it was renamed back or deleted).
- [ ] **Step 2: Run and see them fail**: `./build/release/br_tests.exe --test-case="Cleanup*"`. The first test finds a mismatched pair (wrong record count or an unreadable segment). The second throws from `std::filesystem::rename` or leaves the base files missing.
- [ ] **Step 3: Implement** the two interfaces above. The existing `"a store left mid-swap recovers on load"` test (both `.old` files, no base) must still pass.
- [ ] **Step 4: Run**: `--test-case="Cleanup*"` and `--test-case="InputStore*"` pass, then the full `br_tests.exe`.
- [ ] **Step 5: Commit**

```bash
git add src/store/input_store.cpp tests/cleanup_test.cpp
git commit -m "fix(store): a segment swap recovers as a pair and rolls back on failure"
```

---

### Task 2: An input change keeps the export list

**Files:**
- Modify: `src/export/export_queue.h/.cpp`, `src/engine/engine.cpp` (`addInput` ~line 500, `removeInput` ~line 521)
- Test: `tests/export_queue_test.cpp` (beside `"Export: cancel leaves no file"`)

**Interfaces:**
- `void ExportQueue::setStores(std::map<std::string, const InputStore*> stores, bool stopRunning);`
  - It replaces `stores_` under `m_`.
  - With `stopRunning` and a job running, it makes that job end as with cancel (writer `abort()`, no `.part` left) but as `finishEntry(id, "failed", "an input changed while it was exporting", 0)`. It returns only after that job has finished (wait on `cv_`).
  - Queued and finished entries are untouched.
  - `runJob` copies `stores_` under `m_` when the job starts and builds its `FrameSource` from that copy.
- Engine:
  - `addInput` calls `exportQueue_->setStores(<all stores>, false)` instead of `rebuildExportQueue()`.
  - `removeInput` calls `exportQueue_->setStores(<stores without id>, true)` first, before any store is moved out, in place of `stopExportQueue()`, and no longer rebuilds the queue at the end.
  - `start()` and `stop()` are unchanged.
  - Both calls are guarded by `if (exportQueue_)`.

- [ ] **Step 1: Write the failing tests** (both `BC_REQUIRES(bc::test::Req::H264Encoder)`, with the `Fixture` / `jobFor` / `waitFor` helpers already in the file)
  - `"Export: removing an input stops only the running export, with a reason"`:
    1. `a = f.makeLong(30)`; queue `jobA` and then `jobB` (a second `makeLong(30)`) to different paths.
    2. Wait until A is `running` with `progress > 0`, as the cancel test does.
    3. Call `q.setStores(f.stores, true)`. When it returns, A's state is `"failed"` with error `"an input changed while it was exporting"`, and A's final file and `.part` do not exist.
    4. `q.states().size() == 2`. `waitFor(q, jobB, 60 s).state == "done"`.
  - `"Export: adding an input does not stop the running export"`: queue one `makeLong(10)` job, wait until it is running, call `q.setStores(f.stores, false)`, then `waitFor(...).state == "done"`.
- [ ] **Step 2: Run and see them fail**: `./build/release/br_tests.exe --test-case="Export*"`. The build fails because `setStores` does not exist.
- [ ] **Step 3: Implement** the interfaces above.
- [ ] **Step 4: Run**: `--test-case="Export*"` and `--test-case="Engine*"` pass (the existing `"removing and re-adding an input while Output plays does not crash"` test must stay green), then the full `br_tests.exe`.
- [ ] **Step 5: Commit**

```bash
git add src/export src/engine/engine.cpp tests/export_queue_test.cpp
git commit -m "fix(export): an input change keeps the export list and says why a running export stopped"
```

---

### Task 3: The cleanup runner follows input changes

`Engine::rebuildCleanupRunner()` runs only in `start()`, so the runner keeps raw `InputStore*` for the stores present then. `removeInput` destroys a store the runner may be rewriting (use-after-free), and an input added later is never cleaned.

**Files:**
- Modify: `src/engine/engine.cpp` (`addInput`, `removeInput`)
- Test: `tests/engine_test.cpp` (beside `"removing and re-adding an input while Output plays does not crash"`)

**Interfaces:**
- `removeInput`: `if (cleanup_) cleanup_->stop();` at the top, beside Task 2's `setStores`, before any store is moved out. `rebuildCleanupRunner()` at the end inside `if (started_)`.
- `addInput`: `rebuildCleanupRunner()` inside `if (started_)` after `buildInputs()`.
- `CleanupRunner::stop()` already finishes the current segment before returning; do not change the runner.

- [ ] **Step 1: Write the failing test** `"Engine: cleanup covers an input added later and survives input churn"`, using the `Fixture`. Expose `size_t Engine::cleanupStoreCount() const` (number of stores the current runner holds, 0 without one; add `size_t storeCount() const` to `CleanupRunner`) for this test only, with a one-line comment that it is for tests.
  1. Use `makeShow(1)` and `start()`, then check `cleanupStoreCount() == 1`.
  2. `input.add` a second file input built like `makeShow(2)`'s second input, then check `cleanupStoreCount() == 2`.
  3. Then 20 × (`cleanup.run`, `input.remove` second, `input.add` second). Every `input.*` returns `ok`, and the process survives.
  4. Finish with `cleanupStoreCount() == 2`, then `engine.stop()`.
- [ ] **Step 2: Run and see it fail**: `./build/release/br_tests.exe --test-case="Engine: cleanup*"`. `cleanupStoreCount()` stays 1 after the add.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run**: `--test-case="Engine*"` and `--test-case="Cleanup*"` pass, then the full `br_tests.exe`.
- [ ] **Step 5: Commit**

```bash
git add src/engine src/store/cleanup_runner.h src/store/cleanup_runner.cpp tests/engine_test.cpp
git commit -m "fix(engine): the cleanup runner follows input changes"
```

---

### Task 4: Merge

- [ ] **Step 1:** Full check on the branch: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe; npm --prefix ui test; npm --prefix ui run build`. All pass.
- [ ] **Step 2:** `git checkout main; git merge --no-ff br-safe-cleanup -m "merge: crash-safe cleanup; input changes keep exports"`, then run the full check again on `main`. Do not push.
