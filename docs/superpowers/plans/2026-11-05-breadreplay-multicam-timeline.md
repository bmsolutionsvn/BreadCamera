# BreadReplay Multicam Timeline, Passes and Undo Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replays are shaped on a Premiere/CapCut-style multicam timeline (one track per camera; trim, move, split, slip, change camera), every edit can be undone, live riding replaces only the stretch it played over, and the picture on air never changes under the operator.

**Architecture:** The engine gains an `EditHistory` of event-store snapshots, driven generically by the router (snapshot before each edit command, keep it if the store changed), a pure `splicePass` rule that live-riding passes use instead of today's `overwriteFrom`, and a freezing `Player` for Output that plays copies taken at Take / highlight start. The UI computes every gesture with pure functions in `timelineEdits.ts` and sends one `event.update` on release.

**Tech Stack:** C++20, doctest (`br_tests`); React 18 + TypeScript, Vitest + Testing Library.

**Spec:** `F:\Coding\BreadMedia\BreadReplay\docs\superpowers\specs\2026-11-05-breadreplay-editing-design.md` §4 (mockups `timeline-directions.html` A and `multicam-and-groups.html` option 1 in `F:\Coding\BreadMedia\.superpowers\brainstorm\16760-1793820474\content\`). Plan 2 of 4; it needs plan 1 (`2026-11-05-breadreplay-groups-and-tally.md`) merged: `selectedIds`, `cameraColor`, `event.move`, the tally tokens and `lastMark`.

Repo: `F:\Coding\BreadMedia\BreadReplay`, branch `br-editing-timeline` from `main`, merged back with `--no-ff`. Never push. Change no other repo.

## Global Constraints

- The UI drives the engine only through control commands. A drag previews locally and sends **one** command on release.
- Existing hotkeys keep their meaning. New keys: `S` split, `F` fit, `Delete`, `Ctrl+Z` undo, `Ctrl+Y` and `Ctrl+Shift+Z` redo — remappable rows in Settings; they act everywhere except in a text field.
- Undo history: engine-side, at most **200** snapshots, memory only, cleared when a show opens. Live playout is not an edit.
- Output plays a frozen copy taken at Take / Play; timeline edits, group moves and undo change only the next play. Live riding on Output (lever, camera keys, zoom) still acts on the picture at once.
- A pass replaces only the stretch it played over; one pass is one undo step. While cued or paused, camera keys only switch Preview's camera.
- Everything snaps to frames; with snapping on (default) edges also snap to the playheads, In/Out and other switch points within 6 px; Shift while dragging disables it. Out of range clamps, never errors.
- UI text ≥ 11 px; text on colour AA. No explanatory sentences on screen.
- Tests: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1`, then `./build/release/br_tests.exe`; UI: `npm --prefix ui test`, `npm --prefix ui run build`.

## Review Focus

1. A pass that ends on its own at Out (no pause command), then Ctrl+Z: the whole pass is undone in one step. → Task 2 router test.
2. Riding while playing in reverse (`dir = -1`): the picture follows the keys, but nothing is recorded, because a reverse pass has no clean forward-time meaning **(call)**. → Task 2 player test.
3. Undo pressed while Output plays the same replay: the store reverts, the on-air picture does not jump. → Task 2 test.
4. An edge dragged past a neighbour cut, past In/Out, or outside recorded media: it clamps; cuts stay sorted and at least one frame apart. → Task 3 tests.
5. `Delete` pressed while the rename field of a group has focus: nothing is deleted. → Task 5 test.

---

### Task 1: Undo / redo history in the engine

**Files:**
- Create: `src/events/edit_history.h`, `src/events/edit_history.cpp`, `tests/edit_history_test.cpp`
- Modify: `CMakeLists.txt` (add the source to `br_core` and the test to `br_tests`), `src/events/event_store.h/.cpp`, `src/engine/command_router.h/.cpp`, `src/engine/engine.h/.cpp`
- Test: `tests/router_test.cpp`

**Interfaces:**
- Produces:
  - `class EditHistory { public: explicit EditHistory(size_t limit = 200); void record(const EventStore& before); bool undo(EventStore& store); bool redo(EventStore& store); bool canUndo() const; bool canRedo() const; void clear(); };` — `record` clears the redo stack and drops the oldest past `limit`; `undo` pushes the current store onto redo and restores the newest snapshot; `redo` the reverse.
  - `void EventStore::restoreFrom(const EventStore& snap);` — copies everything, then sets `version_ = max(old version, snap.version) + 1`, so clients always see a newer version.
  - `Engine::history()` returns the engine's `EditHistory&`; `openShow` clears it; `statusJson()` adds `"edit": {"canUndo": bool, "canRedo": bool}`.
  - Router: the **edit commands** are `mark.in`, `mark.out`, `mark.lastN`, `event.update`, `event.trim`, `event.delete`, `event.clearCuts`, `event.move`, `list.new`, `list.rename`, `list.delete`, `list.reorder`. For each, the router copies the store before handling and calls `history.record(copy)` only when `version()` changed. `list.select` is not an edit.
  - New commands `edit.undo` / `edit.redo` → `{ok:true, done:bool}`.
  - `event.update` also accepts `speeds` (`[{tUs, speed}]`) and `zooms` (`[{tUs, rect:{cx,cy,scale}}]`), each sorted by `tUs`; it keeps `outUs >= inUs`.
  - `event.delete` also accepts `ids: [int]` (one edit, so one undo step).

- [ ] **Step 1: Write the failing tests**

`tests/edit_history_test.cpp`:
```cpp
TEST_CASE("History: undo and redo restore the store") {
  br::EventStore s;
  br::EditHistory h;
  h.record(s);
  s.lastN(100 * kS, 1 * kS, "cam1");
  const auto v = s.version();
  CHECK(h.undo(s));
  CHECK(s.events().empty());
  CHECK(s.version() > v);
  CHECK(h.redo(s));
  CHECK(s.events().size() == 1);
  CHECK_FALSE(h.redo(s));
}

TEST_CASE("History: keeps at most 200 steps and a new edit clears redo") {
  br::EventStore s;
  br::EditHistory h;
  for (int i = 0; i < 205; ++i) { h.record(s); s.lastN((100 + i) * kS, 1 * kS, "c"); }
  int n = 0;
  while (h.undo(s)) ++n;
  CHECK(n == 200);
  CHECK(s.events().size() == 5);
  h.redo(s);
  h.record(s);
  CHECK_FALSE(h.canRedo());
}
```
`tests/router_test.cpp`:
```cpp
TEST_CASE("Router: undo reverts a mark and redo brings it back") {
  Fixture f;
  br::CommandRouter r(*f.engine);
  r.handle({{"cmd", "mark.lastN"}, {"n", 1}});
  CHECK(r.handle({{"cmd", "edit.undo"}})["done"] == true);
  CHECK(f.engine->events().events().empty());
  CHECK(r.handle({{"cmd", "edit.redo"}})["done"] == true);
  CHECK(f.engine->events().events().size() == 1);
}

TEST_CASE("Router: a command that changes nothing adds no undo step") {
  Fixture f;
  br::CommandRouter r(*f.engine);
  r.handle({{"cmd", "event.trim"}, {"id", 42}, {"inFrames", 1}});
  CHECK(r.handle({{"cmd", "edit.undo"}})["done"] == false);
}

TEST_CASE("Router: event.update takes speeds and zooms") {
  Fixture f;
  br::CommandRouter r(*f.engine);
  const int id = r.handle({{"cmd", "mark.lastN"}, {"n", 2}})["id"];
  const auto& e = *f.engine->events().find(id);
  r.handle({{"cmd", "event.update"}, {"id", id},
            {"speeds", {{{"tUs", e.inUs + kS}, {"speed", 0.25}}, {{"tUs", e.inUs}, {"speed", 1.0}}}},
            {"zooms", {{{"tUs", e.inUs}, {"rect", {{"cx", 0.4}, {"cy", 0.5}, {"scale", 2.0}}}}}}});
  REQUIRE(e.speeds.size() == 2);
  CHECK(e.speeds[0].speed == doctest::Approx(1.0));
  CHECK(e.zooms[0].rect.scale == doctest::Approx(2.0));
}
```
(`kS` = 1'000'000; add it to `router_test.cpp` if missing. A show reopen test: `show.new` after a mark → `edit.undo` returns `done == false`.)

- [ ] **Step 2: Run and see them fail**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe -tc="History*,Router*"`
Expected: build errors for `EditHistory`.

- [ ] **Step 3: Implement** the interfaces above.

- [ ] **Step 4: Run all engine tests** — `./build/release/br_tests.exe`, all pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/events/edit_history.h src/events/edit_history.cpp src/events/event_store.h src/events/event_store.cpp src/engine tests/edit_history_test.cpp tests/router_test.cpp
git commit -m "feat(engine): undo and redo for every edit"
```

---

### Task 2: Passes replace only what they play over; Output plays a frozen copy

**Files:**
- Create: `src/events/pass.h` (header-only templates), `tests/pass_test.cpp`
- Modify: `src/events/event_store.h/.cpp` (delete `overwriteFrom`; add setters), `src/play/player.h/.cpp`, `src/play/output_player.h/.cpp`, `src/engine/engine.cpp` (Output's `Player` with `freeze = true`), `src/engine/command_router.cpp` (pass undo), `CMakeLists.txt`
- Test: `tests/player_test.cpp`, `tests/output_player_test.cpp`, `tests/router_test.cpp`

**Interfaces:**
- Consumes: Task 1's `EditHistory`, the router's edit-command rule.
- Produces:
  - `pass.h`:
    - `template <typename P> std::vector<P> splicePass(const std::vector<P>& base, const std::vector<P>& pass, int64_t fromUs, int64_t toUs);` → base points with `tUs < fromUs`, then `pass`, then base points with `tUs > toUs`; sorted by `tUs`.
    - `template <typename P> std::vector<P> finishPass(const std::vector<P>& base, const std::vector<P>& pass, int64_t fromUs, int64_t toUs, int64_t outUs);` → `splicePass`, then, when `pass` is non-empty and `toUs < outUs`, adds a point at `toUs` carrying **base's value at `toUs`** (the last base point with `tUs <= toUs`) if it differs from the last pass point's value, so everything after the pass plays as before.
    - `sameValue` overloads: `AngleCut` by `inputId`, `SpeedPoint` by `speed`, `ZoomPoint` by `cx`, `cy`, `scale`.
  - `EventStore`: `void setAngles(int id, std::vector<AngleCut>)`, `setSpeeds`, `setZooms` (bump version); `recordAngle/Speed/Zoom(id, t, v)` now insert or replace the point at exactly `t` and **erase nothing else**.
  - `Player(EventStore&, bc::Rate, bool recordCuts, bool freeze = false);` and `const Event* event() const;` — with `freeze`, `cue(id)` and `restore(state)` copy the store's event into a private `frozen_`, and `tick`, `step`, `remainingSeconds` read `event()` (the copy). `void cueCopy(const Event& e);` cues a given copy (used by highlights).
  - Pass rules in `Player` (only when `recordCuts`, an event is cued and `dir == +1`; playing in reverse, the keys change only the picture: `state_.angle`, the lever, the zoom):
    - `play()` starts a pass: it remembers the store event (and, with `freeze`, the frozen copy) as the pass bases.
    - While playing, `setAngle` / `setLever` / `setSpeedPreset` / `setZoom` append a point at `posUs` to that kind's pass points; the kind's `fromUs` is its first pass point. After each record and after each `tick()` that moves, every active kind is rewritten with `splicePass(base, passPoints, min(fromUs, posUs), max(fromUs, posUs))` into the store (`setAngles`…) and, with `freeze`, into `frozen_`. So old points the pass has played over stop firing.
    - The pass ends on `pause()`, on reaching In/Out in `tick()`, on `cue`/`cueCopy`/`cueAt`/`restore`, on `setDirection` and on `step`: each active kind is written once more with `finishPass(..., e.outUs)`.
    - While not playing: `setAngle` changes only `state_.angle` (Preview's camera = the mark target) and records nothing. `setLever` / `setSpeedPreset` / `setZoom` record one point at `posUs` with `record*` (an ordinary edit).
  - `OutputPlayer::playEvents` copies each event when it starts the highlight (`std::vector<Event> highlight_`) and cues with `cueCopy`; its fade player cues copies too.
  - Router pass undo: on `player.play` or `take`, if no pass is pending, copy the store (`passBefore_`). The pass is **settled** — `history.record(*passBefore_)` if the store's version changed, then cleared — at the start of every edit command, `edit.undo`, `edit.redo`, `player.cue`, `player.cueAt`, `player.live`, `player.pause`, `player.play`, `take`, `show.new`, `show.open`. `player.angle` / `lever` / `preset` / `zoom` / `step` / `direction` / `lock` do not settle. `player.lever` / `preset` / `zoom` on a player that is **not** playing are edit commands (snapshot rule of Task 1).

- [ ] **Step 1: Write the failing tests**

`tests/pass_test.cpp`:
```cpp
TEST_CASE("Pass: splice keeps base outside the played stretch") {
  std::vector<br::AngleCut> base{{0, "cam1"}, {4 * kS, "cam3"}, {8 * kS, "cam4"}};
  const auto out = br::splicePass(base, {{3 * kS, "cam2"}}, 3 * kS, 6 * kS);
  REQUIRE(out.size() == 3);
  CHECK(out[1].inputId == "cam2");
  CHECK(out[2].tUs == 8 * kS);
}

TEST_CASE("Pass: finish restores the base value where the pass stopped") {
  std::vector<br::AngleCut> base{{0, "cam1"}, {4 * kS, "cam3"}, {8 * kS, "cam4"}};
  const auto out = br::finishPass(base, {{3 * kS, "cam2"}}, 3 * kS, 6 * kS, 10 * kS);
  REQUIRE(out.size() == 4);
  CHECK(out[2].tUs == 6 * kS);
  CHECK(out[2].inputId == "cam3");
  CHECK(out[3].inputId == "cam4");
}
```
`tests/player_test.cpp` (event `in = 0`, `out = 10 s`, angles `cam1@0`, `cam3@4s`, `cam4@8s`; Player at 10 fps so one tick = 100 ms; helper `ticksTo(player, tUs)` ticks until `posUs >= tUs`):
- `"Player: a pass replaces only the stretch it played over"` — cue at 2 s (`step`), `play()`, ticks to 3 s, `setAngle("cam2")`, ticks to 6 s, `pause()` → store angles `cam1@0, cam2@3s, cam3@6s, cam4@8s`.
- `"Player: an old cut the pass plays over does not fire"` — same as above, but at 4.5 s `tick().inputId == "cam2"`.
- `"Player: riding speed leaves camera cuts alone"` — a pass with only `setSpeedPreset(0.25)` → angles unchanged.
- `"Player: riding in reverse changes the picture but not the replay"` — cue at 7 s, `setDirection(-1)`, `play()`, at 6 s `setAngle("cam2")` → `tick().inputId == "cam2"`; play to 5 s, pause → store angles unchanged.
- `"Player: camera keys while paused only switch Preview"` — cued, not playing, `setAngle("cam2")` → angles unchanged, `state().angle == "cam2"`.
- `"Player: Output plays a frozen copy"` — `freeze = true`, cue, then `store.trim(id, 0, -5 * kS)`: `remainingSeconds()` is still based on `out = 10 s`; `tick()` keeps playing past 5 s.
- `"Player: riding on Output records into the store and the picture"` — `freeze = true`, playing, `setAngle("cam2")` → `tick().inputId == "cam2"` and the store event has `cam2`.

`tests/output_player_test.cpp`: `"OutputPlayer: a highlight plays the replays as they were at Play"` — `playEvents({a, b})`, then `events.remove(b)` → after `a` ends, `currentEvent() == b` and frames keep coming.

`tests/router_test.cpp`: `"Router: one pass is one undo step, even when it ends by itself"` — `player.cue` (preview, the new event), `player.play`, two `player.preset` and one `player.angle` commands, tick the preview player to the end without `player.pause` (call `engine.playerFor("preview").tick()` in a loop until not playing), then `edit.undo` → the event's angles and speeds equal those before `player.play`; a second `edit.undo` reverts the mark.

`"Router: undo while Output plays does not change the picture"` — `take` a replay, `event.trim` it shorter, `edit.undo`, `edit.redo`: `engine.playerFor("output").remainingSeconds()` follows the copy taken at `take`, not the store.

- [ ] **Step 2: Run and see them fail**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe -tc="Pass*,Player*,OutputPlayer*,Router*"`
Expected: build errors for `splicePass`, `cueCopy`, the `freeze` parameter.

- [ ] **Step 3: Implement** the interfaces above. Mark the known ceiling in `player.cpp`: `// ponytail: a timeline edit to the same kind during a pass is overwritten by the pass's base; split the base per point if that matters.`

- [ ] **Step 4: Run all engine tests** — `./build/release/br_tests.exe`, all pass.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/events src/play src/engine tests/pass_test.cpp tests/player_test.cpp tests/output_player_test.cpp tests/router_test.cpp
git commit -m "feat(play): passes replace only what they play over; Output plays a frozen copy"
```

---

### Task 3: Timeline edit maths (pure)

**Files:**
- Create: `ui/src/screen/timelineEdits.ts`, `ui/src/screen/timelineEdits.test.ts`

**Interfaces:**
- Produces (every function returns a new `EventEdit` and never mutates):
  - `type EventEdit = Pick<Event, 'inUs' | 'outUs' | 'angles' | 'speeds' | 'zooms'>;`
  - `type Limits = { minUs: number; maxUs: number; periodUs: number };`
  - `mediaLimits(inputs: InputStatus[], ev: EventEdit, periodUs: number): Limits` — `minUs` = start of the coverage range of the first block's camera that contains `ev.inUs`; `maxUs` = end of the range of the last block's camera that contains `ev.outUs`; a camera with no containing range gives no limit beyond the current In/Out.
  - `snapTime(tUs: number, o: { periodUs: number; targets: number[]; usPerPx: number; snap: boolean }): number` — nearest target within 6 px when `snap`, else nearest frame (`Math.round(t / periodUs) * periodUs`).
  - `trimIn(ev, tUs, lim)`, `trimOut(ev, tUs, lim)` — clamp to media and to at least one period from the other end. Moving In later drops cuts/points at or before the new In except the one in effect, which moves to the new In; moving In earlier moves the first cut/points to the new In. `trimOut` drops cuts/points at or after the new Out.
  - `moveSwitch(ev, i, tUs, lim)` (`i >= 1`) — clamps to `[angles[i-1].tUs + period, (angles[i+1]?.tUs ?? outUs) - period]`.
  - `changeCamera(ev, i, inputId)` — sets block `i`'s camera; then drops any cut whose camera equals the previous block's.
  - `moveBlock(ev, i, deltaUs, lim)` — moves cut `i` and cut `i+1` together, clamped by their neighbours; block 0's own start stays at In, the last block's end stays at Out (so those cases move one cut only).
  - `split(ev, tUs)` — a new cut at the frame-rounded `tUs`, same camera, when strictly inside `(inUs, outUs)` and not on an existing cut; else unchanged.
  - `deleteBlock(ev, i)` — `i >= 1`: removes cut `i` (the previous block extends). `i == 0` with more blocks: block 0 takes block 1's camera and cut 1 goes. One block: unchanged.
  - `slip(ev, deltaUs, lim)` — In, Out and every cut, speed and zoom point move by the delta, clamped so In ≥ `minUs` and Out ≤ `maxUs`.
  - `moveSpeedEdge(ev, i, tUs, lim)`, `setSpeed(ev, i, speed)`, `moveZoomEdge(ev, i, tUs, lim)`, `resetZoom(ev, i)` (rect `{cx:.5, cy:.5, scale:1}`) — same clamping as switches.

- [ ] **Step 1: Write the failing tests** — one `test()` per function with exact numbers. Base fixture: `period = 100_000`, `ev = { inUs: 0, outUs: 10_000_000, angles: [{tUs:0,inputId:'c1'},{tUs:4_000_000,inputId:'c2'},{tUs:7_000_000,inputId:'c3'}], speeds: [{tUs:0,speed:1}], zooms: [] }`, `lim = { minUs: -2_000_000, maxUs: 12_000_000, periodUs: 100_000 }`. Required cases:
  - `trimIn(ev, 5_000_000, lim)` → `inUs 5_000_000`, angles `[{5_000_000,'c2'},{7_000_000,'c3'}]`, speeds `[{5_000_000,1}]`.
  - `trimIn(ev, -5_000_000, lim).inUs === -2_000_000` (clamped to media); `trimOut(ev, -1, lim).outUs === 100_000` (one period after In).
  - `moveSwitch(ev, 1, 8_000_000, lim)` → cut 1 at `6_900_000` (one period before cut 2).
  - `changeCamera(ev, 2, 'c2')` → angles `[{0,'c1'},{4_000_000,'c2'}]`.
  - `moveBlock(ev, 1, 500_000, lim)` → cuts at `4_500_000` and `7_500_000`; `moveBlock(ev, 1, 9_000_000, lim)` clamps cut 2 to `9_900_000` and keeps the block's length.
  - `split(ev, 5_049_999)` adds `{5_000_000,'c2'}`; `split(ev, 4_000_000)` is unchanged.
  - `deleteBlock(ev, 1)` → `[{0,'c1'},{7_000_000,'c3'}]`; `deleteBlock(ev, 0)` → `[{0,'c2'},{7_000_000,'c3'}]`.
  - `slip(ev, 3_000_000, lim)` → In `2_000_000`, Out `12_000_000`, cuts `+2_000_000` (clamped by `maxUs`).
  - `snapTime(4_030_000, { periodUs: 100_000, targets: [4_000_000], usPerPx: 10_000, snap: true }) === 4_000_000`; with `snap: false` → `4_000_000` by frame rounding; `snapTime(4_080_000, …snap:true)` → `4_100_000` (target is 8 px away).
  - `mediaLimits` with `c1` coverage `[[-1_000_000, 20_000_000]]`, `c3` coverage `[[0, 11_000_000]]` → `{ minUs: -1_000_000, maxUs: 11_000_000 }`.

- [ ] **Step 2: Run and see them fail** — `npm --prefix ui test -- timelineEdits`; module not found.
- [ ] **Step 3: Implement** `timelineEdits.ts`.
- [ ] **Step 4: Run** — `npm --prefix ui test -- timelineEdits`, all pass.
- [ ] **Step 5: Commit**

```bash
git add ui/src/screen/timelineEdits.ts ui/src/screen/timelineEdits.test.ts
git commit -m "feat(ui): timeline edit maths"
```

---

### Task 4: The multicam timeline component

**Files:**
- Modify: `ui/src/screen/Timeline.tsx` (rewrite), `ui/src/screen/Timeline.test.tsx` (rewrite), `ui/src/App.tsx` (the view moves into `Timeline`), `ui/src/app.css`

**Interfaces:**
- Consumes: Task 3's functions; plan 1's `cameraColor`, `cameraIndex`, `selectedIds`, `lastMark`, `event.move`.
- Produces: `Timeline` props `{ client, events, status, selectedId, selectedIds, onSelect, selectedBlock: number | null, onSelectBlock: (i: number | null) => void, fitKey: number, pin, width? }`. `fitKey` changing fits the view to the replay. Test ids: `track-<inputId>`, `block-<i>`, `switch-<i>` (edge before block `i`), `handle-in`, `handle-out`, `slip-bar`, `ph-pvw`, `ph-pgm`, `order-<eventId>`; the `section` carries `data-from` / `data-to` (view range, µs).

Layout (top to bottom):
- Whole-recording bar with replay ticks and the view box; dragging the box moves the view.
- Ruler (11 px labels).
- One track per camera (label: number chip + name): recording faint (`.rec` at 35 % of today's colour), gaps red as today. The selected replay is a window across all tracks (In/Out handles 6 px wide, a 6 px slip bar along the top). Each block sits on its camera's track in that camera's colour with its name (11 px, dark text). The target camera's track keeps plan 1's outline and pin.
- Speed lane (today's) and a zoom lane (segments labelled `×2.0`; unzoomed segments empty).
- Playheads: Preview `--pvw`, Output `--pgm` while on air and in view, across all rows.
- Play-order strip above the tracks when `selectedIds.length > 1`: the selected replays as blocks in display order, labelled with their names; dragging one before another sends `event.move {ids:[id], listId: <the target's group>, beforeId}`; clicking one calls `onSelect(id, selectedIds)`.

Gestures (mousedown → local preview with Task 3 functions → mouseup sends **one** `event.update {id, inUs, outUs, angles, speeds, zooms}`; nothing is sent while moving; a drag that changed nothing sends nothing):
- In / Out handle → `trimIn` / `trimOut`. Switch edge → `moveSwitch`. Slip bar → `slip`.
- Block: after 4 px, the larger axis wins: vertical → `changeCamera` to the track under the pointer; horizontal → `moveBlock`. A click without moving selects the block (`onSelectBlock(i)`).
- Speed / zoom segment edges → `moveSpeedEdge` / `moveZoomEdge`. Right-click a speed segment → 100 / 75 / 50 / 25 % (`setSpeed`); a zoom segment → Reset zoom (`resetZoom`); a block → Split at playhead, Delete block; empty track → today's "Show here in Preview" / "Back to live".
- Snapping per Global Constraints; targets = both playheads, In, Out, every cut. Shift held at mouseup or during move disables.
- Wheel pans the view by 10 % of its span per notch; Ctrl+wheel zooms ×1.25 per notch around the pointer (span at least 1 s, at most the whole recording); `fitKey` change fits In − 5 % … Out + 5 %. The default view is today's (`App`'s rule moves here) until the operator pans or zooms; selecting another replay resets it.

- [ ] **Step 1: Write the failing tests** in `Timeline.test.tsx` (render with `width={1000}`, a 10 s view from 0, inputs `c1`, `c2`, `c3`, the Task 3 fixture event as id 1; `client.send = vi.fn()`):
  - `'one track per camera, each block on its camera'` — `block-1` is inside `track-c2`.
  - `'dragging Out sends one event.update on release'` — mousedown on `handle-out` at x 1000, move to x 900 (no send yet), mouseup → exactly one call with `outUs: 9_000_000`.
  - `'dragging a block to another track changes its camera'` — `block-2` dragged from `track-c3` to `track-c1` → `angles[2].inputId === 'c1'`.
  - `'dragging a switch edge moves that cut'` — `switch-1` from x 400 to x 450 → `angles[1].tUs === 4_500_000`.
  - `'Shift turns snapping off'` — with the Preview playhead at 4_520_000 and snapping, the edge lands on 4_520_000; with `shiftKey: true` on the move and up events it lands on 4_500_000 (frame-rounded).
  - `'wheel pans and Ctrl+wheel zooms at the pointer'` — wheel `deltaY: 100` → `data-from` `1_000_000`; Ctrl+wheel `deltaY: -100` at x 500 → span 8_000_000 centred on 5_000_000.
  - `'playheads take the tally colours'` — `ph-pvw` and `ph-pgm` present while Output is on air.
  - `'the play-order strip reorders with event.move'` — `selectedIds [1, 2]`; dropping `order-2` before `order-1` → `{cmd:'event.move', ids:[2], listId:'g1', beforeId:1}`.

- [ ] **Step 2: Run and see them fail** — `npm --prefix ui test -- Timeline`.
- [ ] **Step 3: Implement** the component and styles; keep `timeToX` / `xToTime` exports.
- [ ] **Step 4: Run** — `npm --prefix ui test; npm --prefix ui run build`, all pass.
- [ ] **Step 5: Commit**

```bash
git add ui/src/screen/Timeline.tsx ui/src/screen/Timeline.test.tsx ui/src/App.tsx ui/src/app.css
git commit -m "feat(ui): multicam replay timeline"
```

---

### Task 5: Edit keys, undo buttons

**Files:**
- Modify: `ui/src/hotkeys.ts`, `ui/src/hotkeys.test.ts`, `ui/src/App.tsx`, `ui/src/App.test.tsx`, `ui/src/api/types.ts` (`Status.edit?: { canUndo: boolean; canRedo: boolean }`), `ui/src/screen/Timeline.tsx` (two buttons)

**Interfaces:**
- Consumes: Task 1's `edit.undo` / `edit.redo` and `status.edit`; Task 3's `split`, `deleteBlock`; Task 4's `selectedBlock`, `fitKey`.
- Produces — hotkey rows (labels as shown) and defaults:

| id | label | default | command |
|---|---|---|---|
| `edit.split` | Split at playhead | `S` | `{cmd:'ui.split'}` |
| `view.fit` | Fit replay | `F` | `{cmd:'ui.fit'}` |
| `edit.delete` | Delete | `Delete` | `{cmd:'ui.delete'}` |
| `edit.undo` | Undo | `Ctrl+Z` | `{cmd:'edit.undo'}` |
| `edit.redo` | Redo | `Ctrl+Y` | `{cmd:'edit.redo'}` |

  plus a fixed (not remappable) `ctrl+shift+z` → `{cmd:'edit.redo'}`. App: `ui.split` → `event.update` with `split(ev, previewPosUs)` for the primary replay; `ui.fit` → `fitKey + 1`; `ui.delete` → `deleteBlock` via `event.update` when a block is selected, else `event.delete {ids: selectedIds}` (nothing selected → nothing). `edit.*` go straight to the engine. Undo ↶ / Redo ↷ icon buttons (aria-labels `Undo`, `Redo`, 28 px) at the timeline's top right, disabled from `status.edit`.

- [ ] **Step 1: Write the failing tests**
  - `hotkeys.test.ts`: `'edit keys map to their commands'` (S, F, Delete, Ctrl+Z, Ctrl+Y, Ctrl+Shift+Z); `'Delete does nothing while a text field has focus'` (target an `<input>` → `null`); `'S still splits while a list row has focus'` (target a `<button>` → `{cmd:'ui.split'}`).
  - `App.test.tsx`: `'Delete with two replays selected deletes both in one command'` → `{cmd:'event.delete', ids:[1, 3]}`; `'S splits the primary replay at the Preview playhead'` → one `event.update` whose angles contain the new cut; `'Undo is disabled until the engine says it can undo'`.
- [ ] **Step 2: Run and see them fail** — `npm --prefix ui test`.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — `npm --prefix ui test; npm --prefix ui run build`, all pass.
- [ ] **Step 5: Commit**

```bash
git add ui/src/hotkeys.ts ui/src/hotkeys.test.ts ui/src/App.tsx ui/src/App.test.tsx ui/src/api/types.ts ui/src/screen/Timeline.tsx
git commit -m "feat(ui): split, fit, delete, undo and redo keys"
```

---

### Task 6: Merge

- [ ] **Step 1:** Full check on the branch: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe; npm --prefix ui test; npm --prefix ui run build`. All pass.
- [ ] **Step 2:** `git checkout main; git merge --no-ff br-editing-timeline -m "merge: multicam timeline, passes, frozen on-air copy, undo"`; full check again on `main`. Do not push.
