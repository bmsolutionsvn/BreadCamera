# BreadReplay Groups, Multi-Select and Tally Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replays sit at a root level or in renamable groups (Game 1, Game 2…), can be multi-selected, moved and played in list order, and the screen shows PGM (red), PVW (green) and the mark target (camera colour + ◎) without sentences.

**Architecture:** The event store gains a root order (list id `""`) and group operations; the router exposes them as `list.*` / `event.move` commands and `highlight.play` takes explicit ids. The UI's event list becomes a root-plus-groups panel with selection held in `App`; the monitors, angle tiles, mark buttons and timeline lanes take one colour language from a single `cameraColors.ts`.

**Tech Stack:** C++20, doctest (`br_tests`), nlohmann::json; React 18 + TypeScript, Vitest + Testing Library.

**Spec:** `F:\Coding\BreadMedia\BreadReplay\docs\superpowers\specs\2026-11-05-breadreplay-editing-design.md` §5, §6, §7 (approved mockup `F:\Coding\BreadMedia\.superpowers\brainstorm\16760-1793820474\content\mark-target-v2.html`). This is plan 1 of 4; plans 2–4 (timeline + undo, export, disk cleanup) build on it.

Repo: `F:\Coding\BreadMedia\BreadReplay`, branch `br-editing-groups` from `main`, merged back into `main` with `--no-ff`. Never push. Change no other repo.

## Global Constraints

- The UI drives the engine only through control commands; every new action is an engine command.
- Existing hotkeys keep their meaning (F1–F8 camera to Preview, `[`/`]` trim, I/O/Space mark keys, Enter take). F2 never renames.
- New or touched UI text is at least 11 px; text on colour meets WCAG AA (4.5:1).
- Tally: PGM `#e5484d` (text on `#b3261e`), PVW `#30a46c` (text on `#17694a`). Camera colours: Cam 1 `#5aa0ff`, Cam 2 `#e4a94b`, Cam 3 `#c084fc`, Cam 4 `#f472b6`; Cams 5–8 here: `#22d3ee`, `#facc15`, `#fb923c`, `#cbd5e1` (no hue within ±25° of the tally red or green; at least 3:1 against the track `#151a20`).
- No explanatory sentences on screen in the touched components (tooltips via `title` may stay).
- Root list id is the empty string `""`. A new show starts with no group.
- Tests: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1`, then `./build/release/br_tests.exe`; UI: `npm --prefix ui test` and `npm --prefix ui run build`.

## Review Focus

1. A list row (a `<button>`) has focus after a click: Ctrl+A must still act, but Space/Enter on that focused button must not both click it and mark/take. → Task 3 hotkey test.
2. The target group is deleted, then the operator marks: the new replay lands at the root, not in a dead list id. → Task 1 test.
3. An old show file (lists with "Game 1", `currentList` "list-1", no `rootOrder`) loads with its group intact and no duplicated ids. → Task 1 test.
4. A selection dropped onto one of its own rows (`beforeId` is a moved id): no replay is lost or duplicated. → Task 1 test.
5. Play selection with an id that was deleted meanwhile, or with only unknown ids: unknown ids are skipped; nothing plays and the output stays idle when none remain. → Task 2 test.

---

### Task 1: Root and groups in the event store

**Files:**
- Modify: `src/events/event_store.h`, `src/events/event_store.cpp` (remove `ensureList()` and its calls)
- Test: `tests/event_store_test.cpp` (add cases; fix any existing case that assumed an automatic "Game 1")

**Interfaces:**
- Produces (all on `br::EventStore`):
  - `const std::vector<int>& orderOf(const std::string& listId) const;` — `""` → root order; unknown id → a static empty vector.
  - `std::vector<int> displayOrder() const;` — root first, then each group in `lists()` order. **(call: root on top)**
  - `std::string newList(const std::string& name);` — empty name → `"Game N"`, N = `lists().size() + 1`; the new group becomes the target (as today).
  - `bool renameList(const std::string& id, const std::string& name);` — trims spaces; false for an empty name or unknown id.
  - `void deleteList(const std::string& id);` — its replays are appended to the root order in their group order with `listId = ""`; if it was the target, the target becomes `""`.
  - `void selectList(const std::string& id);` — accepts `""` (root) or a known id.
  - `void moveEvents(const std::vector<int>& ids, const std::string& toList, std::optional<int> beforeId);` — removes each id from its current order, inserts them in the given order before `beforeId` in `toList` (at the end when absent or not in that order); sets `listId`. If `beforeId` is itself in `ids`, insert before the first id after it in the target order that is not moved (end if none). Unknown `toList` → no change. Bumps version once.
  - `makeEvent` appends to the target list's order, or the root order when the target is `""`; `remove` also erases from the root order.
  - JSON: new field `"rootOrder"`. `fromJson`: an event whose `listId` matches no list becomes root; an event missing from its list's order is appended to it; ids are never duplicated.

- [ ] **Step 1: Write the failing tests** in `tests/event_store_test.cpp`:

```cpp
TEST_CASE("Groups: a new show has no group and marks go to the root") {
  br::EventStore s;
  CHECK(s.lists().empty());
  CHECK(s.currentList().empty());
  const int id = s.lastN(100 * kS, 12 * kS, "cam1").id;
  CHECK(s.find(id)->listId.empty());
  CHECK(s.orderOf("") == std::vector<int>{id});
}

TEST_CASE("Groups: a new group is named Game N and becomes the target") {
  br::EventStore s;
  const auto g1 = s.newList("");
  const auto g2 = s.newList("");
  CHECK(s.lists()[0].name == "Game 1");
  CHECK(s.lists()[1].name == "Game 2");
  CHECK(s.currentList() == g2);
  const int id = s.lastN(100 * kS, 12 * kS, "cam1").id;
  CHECK(s.orderOf(g2) == std::vector<int>{id});
  s.selectList("");
  const int r = s.lastN(110 * kS, 12 * kS, "cam1").id;
  CHECK(s.orderOf("") == std::vector<int>{r});
  (void)g1;
}

TEST_CASE("Groups: deleting the target group moves its replays to the root and targets the root") {
  br::EventStore s;
  const int a = s.lastN(100 * kS, 1 * kS, "cam1").id;  // root
  const auto g = s.newList("Game 1");
  const int b = s.lastN(110 * kS, 1 * kS, "cam1").id;
  const int c = s.lastN(120 * kS, 1 * kS, "cam1").id;
  s.deleteList(g);
  CHECK(s.lists().empty());
  CHECK(s.currentList().empty());
  CHECK(s.orderOf("") == std::vector<int>{a, b, c});
  CHECK(s.find(b)->listId.empty());
  const int d = s.lastN(130 * kS, 1 * kS, "cam1").id;
  CHECK(s.find(d)->listId.empty());
}

TEST_CASE("Groups: moving a selection into a group before a replay") {
  br::EventStore s;
  const int a = s.lastN(100 * kS, 1 * kS, "c").id, b = s.lastN(101 * kS, 1 * kS, "c").id,
            c = s.lastN(102 * kS, 1 * kS, "c").id;
  const auto g = s.newList("G");
  const int d = s.lastN(103 * kS, 1 * kS, "c").id;
  s.moveEvents({c, a}, g, d);
  CHECK(s.orderOf(g) == std::vector<int>{c, a, d});
  CHECK(s.orderOf("") == std::vector<int>{b});
  CHECK(s.find(a)->listId == g);
}

TEST_CASE("Groups: dropping a selection onto one of its own rows loses nothing") {
  br::EventStore s;
  const int a = s.lastN(100 * kS, 1 * kS, "c").id, b = s.lastN(101 * kS, 1 * kS, "c").id,
            c = s.lastN(102 * kS, 1 * kS, "c").id;
  s.moveEvents({a, b}, "", b);
  CHECK(s.orderOf("") == std::vector<int>{a, b, c});
  s.moveEvents({a}, "", std::nullopt);
  CHECK(s.orderOf("") == std::vector<int>{b, c, a});
}

TEST_CASE("Groups: rename trims and refuses an empty name") {
  br::EventStore s;
  const auto g = s.newList("");
  CHECK(s.renameList(g, "  Final  "));
  CHECK(s.lists()[0].name == "Final");
  CHECK_FALSE(s.renameList(g, "   "));
  CHECK_FALSE(s.renameList("nope", "X"));
}

TEST_CASE("Groups: display order is the root, then each group") {
  br::EventStore s;
  const int a = s.lastN(100 * kS, 1 * kS, "c").id;
  const auto g = s.newList("");
  const int b = s.lastN(101 * kS, 1 * kS, "c").id;
  CHECK(s.displayOrder() == std::vector<int>{a, b});
  (void)g;
}

TEST_CASE("Groups: an old show keeps its Game 1 and round-trips with a root") {
  const auto old = nlohmann::json::parse(R"({"nextId":2,"nextListId":2,"currentList":"list-1",
    "lists":[{"id":"list-1","name":"Game 1","order":[1]}],
    "events":[{"id":1,"listId":"list-1","inUs":0,"outUs":5}]})");
  auto s = br::EventStore::fromJson(old);
  REQUIRE(s.lists().size() == 1);
  CHECK(s.currentList() == "list-1");
  CHECK(s.orderOf("list-1") == std::vector<int>{1});
  CHECK(s.orderOf("").empty());
  s.selectList("");
  const int r = s.lastN(100 * kS, 1 * kS, "c").id;
  const auto back = br::EventStore::fromJson(s.toJson());
  CHECK(back.orderOf("") == std::vector<int>{r});
  CHECK(back.currentList().empty());
}
```

- [ ] **Step 2: Run and see them fail**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe -tc="Groups*"`
Expected: build errors for `orderOf`, `renameList`, `deleteList`, `moveEvents`, `displayOrder`.

- [ ] **Step 3: Implement the interfaces above** in `event_store.h/.cpp`. Root order is a private `std::vector<int> rootOrder_`. Remove `ensureList()` (constructor and `fromJson`). Update `OutputPlayer::playHighlight` to read `events_.orderOf(id)` instead of its own list scan (keep the empty-order fallback out: `orderOf` is the order).

- [ ] **Step 4: Run all engine tests**

Run: `./build/release/br_tests.exe`
Expected: all pass (fix any older case that relied on an automatic "Game 1").

- [ ] **Step 5: Commit**

```bash
git add src/events/event_store.h src/events/event_store.cpp src/play/output_player.cpp tests/event_store_test.cpp
git commit -m "feat(events): root level and group operations"
```

---

### Task 2: Group, move and play-selection commands

**Files:**
- Modify: `src/engine/command_router.cpp:135-196`, `src/play/output_player.h`, `src/play/output_player.cpp`
- Test: `tests/router_test.cpp`, `tests/output_player_test.cpp`

**Interfaces:**
- Consumes: Task 1's `EventStore` methods.
- Produces (commands; replies are `{"ok":true,...}` or `{"ok":false,"error":"..."}`):
  - `list.new {name?}` → `{ok, id}`; missing or empty name → "Game N".
  - `list.rename {id, name}` → error `"name is empty"` or `"no such group"` when `renameList` is false.
  - `list.delete {id}`; `list.select {id}` (`""` = root).
  - `event.move {ids:[int], listId:string, beforeId?:int}`.
  - `highlight.play {ids:[int]}` plays those ids; `{listId}` plays that group (`""` = root); neither → the target list (today's behaviour).
  - `mark.in` / `mark.out` / `mark.lastN` now reply `{ok, id, edge:"in"|"out", tUs, inputId}`: `tUs` is the event's `inUs` for `in`, `outUs` for `out` and `lastN`; `inputId` is the camera used. (Plan 1 Task 4 stamps from this.)
  - `void OutputPlayer::playEvents(std::vector<int> ids);` — keeps known ids only, sorts them by their position in `events_.displayOrder()`, then plays them as `playHighlight` does (Fade or Cut). `playHighlight(listId)` becomes `playEvents(events_.orderOf(listId))`. Empty after filtering → returns without playing.

- [ ] **Step 1: Write the failing tests**

`tests/router_test.cpp`:
```cpp
TEST_CASE("Router: group commands create, rename, move and delete") {
  Fixture f;
  br::CommandRouter r(*f.engine);
  const int a = r.handle({{"cmd", "mark.lastN"}, {"n", 1}})["id"];
  const std::string g = r.handle({{"cmd", "list.new"}})["id"];
  CHECK(f.engine->events().lists()[0].name == "Game 1");
  CHECK(r.handle({{"cmd", "list.rename"}, {"id", g}, {"name", ""}})["error"] == "name is empty");
  CHECK(r.handle({{"cmd", "list.rename"}, {"id", g}, {"name", "Final"}})["ok"] == true);
  r.handle({{"cmd", "event.move"}, {"ids", {a}}, {"listId", g}});
  CHECK(f.engine->events().orderOf(g) == std::vector<int>{a});
  r.handle({{"cmd", "list.delete"}, {"id", g}});
  CHECK(f.engine->events().orderOf("") == std::vector<int>{a});
}

TEST_CASE("Router: a mark replies with the camera and time it used") {
  Fixture f;
  br::CommandRouter r(*f.engine);
  const auto res = r.handle({{"cmd", "mark.lastN"}, {"n", 2}});
  CHECK(res["ok"] == true);
  CHECK(res["edge"] == "out");
  CHECK(res["inputId"] == "cam1");
  CHECK(res["tUs"].get<int64_t>() == f.engine->events().find(res["id"])->outUs);
}
```

`tests/output_player_test.cpp` (use the file's `Fixture`, its events and `RecordingOutput`):
```cpp
TEST_CASE("OutputPlayer: a selection plays in displayed order, whatever order it was sent") {
  Fixture f;
  const int a = f.events.lastN(kBase + 1 * kS, 1 * kS, "cam1").id;  // root
  f.events.newList("");
  const int c = f.events.lastN(kBase + 3 * kS, 1 * kS, "cam1").id;  // group
  br::OutputPlayer op(*f.player, *f.src, *f.renderer, f.events, f.settings);
  op.playEvents({c, a, 999});
  CHECK(op.currentEvent() == a);
}

TEST_CASE("OutputPlayer: a selection of unknown ids plays nothing") {
  Fixture f;
  br::OutputPlayer op(*f.player, *f.src, *f.renderer, f.events, f.settings);
  op.playEvents({999});
  CHECK(op.idle());
}
```

- [ ] **Step 2: Run and see them fail**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe -tc="Router*,OutputPlayer*"`
Expected: build error for `playEvents`; then the new router cases fail.

- [ ] **Step 3: Implement** the commands in `CommandRouter::handle` and `OutputPlayer::playEvents`. In `highlight.play`, use `cmd.contains("ids")` / `cmd.contains("listId")` to tell "root" (`""`) from "not given".

- [ ] **Step 4: Run all engine tests**

Run: `./build/release/br_tests.exe`
Expected: all pass.

- [ ] **Step 5: Commit**

```bash
git add src/engine/command_router.cpp src/play/output_player.h src/play/output_player.cpp tests/router_test.cpp tests/output_player_test.cpp
git commit -m "feat(engine): group, move and play-selection commands; marks report their camera"
```

---

### Task 3: Groups panel, multi-select and play selection (UI)

**Files:**
- Create: `ui/src/screen/selection.ts`, `ui/src/screen/selection.test.ts`
- Modify: `ui/src/api/types.ts` (`EventStore.rootOrder: number[]`), `ui/src/screen/EventList.tsx` (rewrite), `ui/src/screen/EventList.test.tsx` (rewrite), `ui/src/App.tsx`, `ui/src/hotkeys.ts`, `ui/src/hotkeys.test.ts`, `ui/src/app.css`

**Interfaces:**
- Consumes: Task 2's commands.
- Produces:
  - `selection.ts`: `displayOrder(store: EventStore): number[]` (root, then groups, same rule as the engine); `groupOf(store, id): string`; `toggleId(ids: number[], id: number): number[]`; `rangeIds(order: number[], anchor: number, id: number): number[]` (inclusive, in `order`).
  - `EventList` props: `{ client, events, selectedId: number|null, selectedIds: number[], onSelect: (primary: number|null, ids: number[]) => void, hotkeys? }`. `App` owns `selectedIds` (and keeps `selected` as the primary). Plans 2 and 3 use `selectedIds`.
  - Hotkey row `{ id: 'select.all', label: 'Select all in group' }`, default `'Ctrl+A'`, command `{ cmd: 'ui.selectAll' }` (App selects every replay in the primary's group, or the target group when nothing is selected).
  - `keyToCommand` change: Ctrl combos are matched (`combo()` prefixes `ctrl+` before `shift+`); Alt/Meta still return null. `isTyping`: INPUT, TEXTAREA, SELECT and contentEditable always block; a focused BUTTON blocks only `space` and `enter`.

Panel behaviour (one row = one replay, as today's `button.ev`):
- No group exists → one plain list, header `Replays`, no group headers.
- Groups exist → the root rows first (header `No group` **(call)**), then a header per group: chevron (collapse, local state), name, count, ◎ when it is the target, Play ▸, and a `+ Group` button at the panel top (sends `list.new {}`).
- Clicking a group header (not its buttons) sends `list.select {id}`; clicking the target group's header again, or the root header, sends `list.select {id: ''}`.
- Double-click the name, or right-click → Rename: an inline `<input>`; Enter sends `list.rename`, Escape or blur cancels. Right-click → Delete group: the header turns into `Delete <name>?` with `Delete` / `Keep` buttons (no modal; reverts after 4 s) **(call)**; Delete sends `list.delete`.
- Row click: `onSelect(id, [id])` and `player.cue` as today. Ctrl+click: toggles in `selectedIds`, no cue. Shift+click: `rangeIds(displayOrder, primary, id)`, no cue.
- Drag a row (HTML5 drag-and-drop) onto another row → `event.move {ids, listId: that row's group, beforeId: that row's id}`; onto a group header → `event.move {ids, listId}` (end). `ids` = the selection in display order when the dragged row is selected, else just that row.
- Bottom button: with 2+ selected `Play selection (n)` → `highlight.play {ids}` (display order); otherwise `Play group (<name>)` / `Play all` (root) → `highlight.play {listId}`.
- Existing empty-state note stays only when there are no replays at all.

- [ ] **Step 1: Write the failing tests**

`ui/src/screen/selection.test.ts`:
```ts
test('displayOrder lists the root, then each group', () => {
  expect(displayOrder(store({ rootOrder: [1], lists: [{ id: 'g1', name: 'Game 1', order: [3, 2] }] }))).toEqual([1, 3, 2]);
});
test('rangeIds selects inclusively in display order either way', () => {
  expect(rangeIds([1, 3, 2, 5], 2, 3)).toEqual([3, 2]);
});
```
(`store()` is a small local builder filling the other `EventStore` fields.)

`ui/src/screen/EventList.test.tsx` cases, each asserting `client.send` calls:
- `'no group shows one plain list'` — `lists: []`, `rootOrder: [1, 3]`: header `Replays`, no `+ Group` header rows other than the button, rows in order.
- `'clicking a group targets it; clicking the target again targets the root'` → `{cmd:'list.select', id:'g1'}` then (rerender with `currentList:'g1'`) `{cmd:'list.select', id:''}`; the target header contains `◎`.
- `'+ Group creates a group'` → `{cmd:'list.new'}`.
- `'double-click renames inline; Escape cancels'` → typing `Final` + Enter sends `{cmd:'list.rename', id:'g1', name:'Final'}`; a second edit with Escape sends nothing.
- `'Delete group asks inline, then deletes'` → no send on the menu item, `{cmd:'list.delete', id:'g1'}` after clicking `Delete`.
- `'Ctrl+click and Shift+click build a selection; Play selection plays it in display order'` → `onSelect` receives `[1, 3]` after Ctrl+click on 3 (primary 1); the button reads `Play selection (2)` and sends `{cmd:'highlight.play', ids:[1, 3]}`.
- `'dropping a selected row on another group's row moves the selection'` → `{cmd:'event.move', ids:[1, 3], listId:'g1', beforeId:2}` (use `fireEvent.dragStart` / `drop` with a stub `dataTransfer`).

`ui/src/hotkeys.test.ts`:
```ts
test('Ctrl+A selects all even when a list row has focus; Space on a focused button does not mark', () => {
  const button = document.createElement('button');
  expect(keyToCommand(new KeyboardEvent('keydown', { key: 'a', ctrlKey: true }), hotkeyMap())).toEqual({ cmd: 'ui.selectAll' });
  const onButton = (init: KeyboardEventInit) => { const e = new KeyboardEvent('keydown', init); Object.defineProperty(e, 'target', { value: button }); return e; };
  expect(keyToCommand(onButton({ key: 'a', ctrlKey: true }), hotkeyMap())).toEqual({ cmd: 'ui.selectAll' });
  expect(keyToCommand(onButton({ key: ' ' }), hotkeyMap())).toBeNull();
});
```

- [ ] **Step 2: Run and see them fail**

Run: `npm --prefix ui test`
Expected: the new cases fail (missing module `selection`, old EventList markup).

- [ ] **Step 3: Implement** `selection.ts`, the panel, App selection state and the hotkey changes. Styles in `app.css`: group header row 28 px, name 12 px, count and ◎ in `var(--bm-text-muted)`; ◎ drawn as in the mockup (`.aim`: 2 px ring + centre dot) in `var(--bm-text)`; the primary row keeps `.ev.sel`; other selected rows get `.ev.multi` (the `.sel` background at 60 % opacity).

- [ ] **Step 4: Run UI tests and build**

Run: `npm --prefix ui test; npm --prefix ui run build`
Expected: all pass; build succeeds.

- [ ] **Step 5: Commit**

```bash
git add ui/src/screen/selection.ts ui/src/screen/selection.test.ts ui/src/screen/EventList.tsx ui/src/screen/EventList.test.tsx ui/src/App.tsx ui/src/hotkeys.ts ui/src/hotkeys.test.ts ui/src/api/types.ts ui/src/app.css
git commit -m "feat(ui): groups panel, multi-select and play selection"
```

---

### Task 4: PGM / PVW / mark-target visuals

**Files:**
- Create: `ui/src/screen/cameraColors.ts`, `ui/src/screen/cameraColors.test.ts`
- Modify: `ui/src/app.css`, `ui/src/format.ts` (+ `format.test.ts`), `ui/src/screen/Monitors.tsx` (+ test), `ui/src/screen/AngleRow.tsx` (+ test), `ui/src/screen/ControlsBar.tsx` (+ test), `ui/src/screen/Timeline.tsx` (+ test), `ui/src/App.tsx`

**Interfaces:**
- Consumes: Task 2's mark reply `{id, edge, tUs, inputId}`.
- Produces:
  - `cameraColors.ts`: `CAMERA_COLORS: readonly string[]` (the 8 colours in Global Constraints, in order); `cameraColor(index: number): string` (0-based, wraps at 8); `cameraIndex(status: Status | null, inputId: string | undefined): number` (−1 when unknown); `TALLY = { pgm: '#e5484d', pgmInk: '#b3261e', pvw: '#30a46c', pvwInk: '#17694a' } as const`.
  - CSS tokens on `:root`: `--pgm`, `--pgm-ink`, `--pvw`, `--pvw-ink`, `--cam1` … `--cam8`. Every `#2563eb` Preview use becomes `--pvw`; `.mon.pg.onair` uses `--pgm`.
  - `format.ts`: `formatClockTenths(tUs: number): string` — `formatClock` plus `.` and tenths (`01:12:29.4`).
  - `type MarkStamp = { edge: 'in' | 'out'; inputId: string; tUs: number }`. `App` sets it from each mark reply (buttons, hotkeys and the Preview menu all go through one `sendMark` helper) and clears it after 1500 ms; it also keeps `lastMark` (same shape, not cleared) for the timeline pin.
  - `Monitors` prop `stamp?: MarkStamp | null`; `ControlsBar` prop `target?: { num: number; color: string }`; `Timeline` props `pin?: MarkStamp | null` and it reads the Output playhead from `status.players.output`.

What each piece shows (mockup `mark-target-v2.html`):
- Preview monitor: 3 px frame `--pvw`; tag `PVW` + number chip of the Preview camera, on `--pvw-ink`, white 800 12 px. Output monitor: frame and tag `PGM` + number on `--pgm-ink` while on air; muted tag `PGM` with no chip when idle. The sentences `PREVIEW · only you see this`, `ON AIR · …` and `Nothing on air. Load a replay…` go; the timecode, remaining time and progress bar stay.
- Stamp on Preview (top right, ~1.5 s): `IN` or `OUT`, the camera number chip in its colour, `formatClockTenths(tUs)`.
- Angle tiles: number chip (camera colour, dark text `#0b0e12`) + name in the footer; `PGM` chip (on `--pgm-ink`) on the Output camera's tile while on air; `PVW` chip (on `--pvw-ink`) on the Preview camera's tile. The Preview camera's tile is the target: 3 px frame in its colour and the ◎ badge (`.aim`, 26 px, ring + dot in its colour); other tiles `.dim` (scene `saturate(.4) brightness(.6)`). The F-key `kbd` moves into the tile's `title`.
- Mark start / Mark end buttons: ◎ mini (14 px) and the target's number chip in its colour; border in its colour.
- Timeline: each angle block takes the colour of **its camera** (today it is coloured by block position: `c${i + 1}`, a bug); the target camera's recording lane is outlined (`box-shadow: inset 0 0 0 2px <colour>`); a 3 px pin in its colour at `lastMark.tUs` on that camera's lane; Preview playhead `--pvw`, Output playhead `--pgm` (shown while on air and in view). Labels, ruler and speed text go to 11 px (speed lane 14 px tall).
- Colour is never alone: number and name sit beside every colour.

- [ ] **Step 1: Write the failing tests**

`ui/src/screen/cameraColors.test.ts` — implement `hueOf(hex)` and `contrast(a, b)` (WCAG relative luminance) locally in the test:
```ts
test('camera colours stay off the tally hues and read on the track', () => {
  for (const c of CAMERA_COLORS) {
    for (const t of [TALLY.pgm, TALLY.pvw]) {
      const d = Math.abs(hueOf(c) - hueOf(t)) % 360;
      expect(Math.min(d, 360 - d)).toBeGreaterThanOrEqual(25);
    }
    expect(contrast(c, '#151a20')).toBeGreaterThanOrEqual(3);
    expect(contrast(c, '#0b0e12')).toBeGreaterThanOrEqual(4.5);
  }
  expect(contrast('#ffffff', TALLY.pgmInk)).toBeGreaterThanOrEqual(4.5);
  expect(contrast('#ffffff', TALLY.pvwInk)).toBeGreaterThanOrEqual(4.5);
});
```
Component tests (statuses built with two inputs `cam1`/`Cam 1`, `cam2`/`Cam 2`; Preview angle `cam2`, Output angle `cam1` with `eventId: 1`):
- `Monitors.test.tsx` `'tags name the camera on each monitor'`: Preview tag has text `PVW` and `2`; Output tag `PGM` and `1`; `queryByText(/only you see this/)` is null. `'a mark stamp shows edge, camera and time'`: with `stamp={{edge:'in', inputId:'cam2', tUs}}` → `IN`, `2`, `formatClockTenths(tUs)`.
- `AngleRow.test.tsx` `'the mark target is framed and the others dim'`: tile `Angle Cam 2` has class `target`, `Angle Cam 1` has `dim`; `'tally chips sit on their cameras'`: `PGM` inside the Cam 1 tile, `PVW` inside Cam 2.
- `ControlsBar.test.tsx` `'Mark buttons carry the target camera number'`: with `target={{num:2, color:'#e4a94b'}}` both mark buttons contain `2`.
- `Timeline.test.tsx` `'a block takes its camera colour'`: an event whose only angle is `cam2` → `angle-block-0` has style background `#e4a94b` (`rgb(228, 169, 75)`).
- `format.test.ts`: `formatClockTenths` of `local(1,12,29) + 400_000` is `01:12:29.4`.

- [ ] **Step 2: Run and see them fail**

Run: `npm --prefix ui test`
Expected: the new cases fail.

- [ ] **Step 3: Implement** as described. Keep the existing `title` tooltips.

- [ ] **Step 4: Run UI tests and build**

Run: `npm --prefix ui test; npm --prefix ui run build`
Expected: all pass; build succeeds.

- [ ] **Step 5: Commit**

```bash
git add ui/src
git commit -m "feat(ui): PGM/PVW tally and mark-target colours, mark stamp and pin"
```

(`git add ui/src` is safe here only after `git status --short` shows nothing unexpected under `ui/src`; never stage `ui/.impeccable/`.)

---

### Task 5: Merge

- [ ] **Step 1:** Run the full check on the branch: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build.ps1; ./build/release/br_tests.exe; npm --prefix ui test; npm --prefix ui run build`. Expected: all pass.
- [ ] **Step 2:** `git checkout main; git merge --no-ff br-editing-groups -m "merge: groups, multi-select and tally colours"`; run the full check again on `main`. Do not push.
