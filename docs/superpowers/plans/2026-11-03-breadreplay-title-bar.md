# BreadReplay: a Title Bar to Move, Maximise, Minimise and Close the Window — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** BreadReplay's window can be dragged, double-clicked to maximise, snapped, minimised, maximised/restored and closed, as BreadCamera's can.

**Architecture:** BreadReplay's shell already removes the system caption (`WM_NCCALCSIZE` in `src/shell/shell_main.cpp`), turns on CSS `app-region: drag` (`IsNonClientRegionSupportEnabled`), answers `{type: "window", action: "minimize" | "maximize" | "close"}` page messages, and posts `{type: "windowState", maximized}` on every resize. With composition hosting, BreadCamera's `CompositionHost` maps `WM_NCHITTEST` to the page's drag regions. What is missing is the page's side: no element is a drag region and there are no caption buttons. Add a `TitleBar` like BreadCamera's (`BreadCamera/ui/src/shell/TitleBar.tsx`) at the top of BreadReplay's page.

**Tech Stack:** React + TypeScript, MUI (as the rest of BreadReplay's UI), vitest.

**Spec:** none; owner's report 2026-11-03: "BreadReplay has no menu bar: difficult to move, maximise, minimise, close." The reference is BreadCamera's title bar (its spec §6.4).

**Runs after:** the BreadReplay native-preview plan is merged (it changes the same page and the shell's hosting).

## Global Constraints

- 32 px tall, the full window width, the first row of the page; everything else moves down by 32 px (no overlap with the monitors).
- Drag region: the whole bar except the buttons (`app-region: drag` on the bar, `no-drag` on the buttons). Double-click on the bar maximises/restores (the OS does it for a drag region).
- Buttons on the right, 46 px wide each: Minimise, Maximise/Restore (icon follows `windowState`), Close (red hover `#C42B1C`). Close sends `close`, which the shell turns into its "Stop recording and quit?" dialog, as today.
- Left: the app name "BreadReplay" in the bar's text colour. No menus (BreadReplay's settings stay where they are).
- Labels in the UI's plain words: "Minimise", "Maximise" / "Restore", "Quit BreadReplay".
- Talk to the shell through the bridge the page already uses (`nativeVideoRegistry.ts`'s `bridge()`), or a small shared helper next to it; do not add a second way.
- Hotkeys keep working: the bar takes no keyboard focus on click.
- Never push. BreadCamera files are used as a reference, not edited.

## Review Focus

1. **Native video layers** line up with their DOM boxes after the 32 px shift (the layers follow DOM rects). Test in Task 1 (the Monitors test's rects, if it checks them) and Task 2 (look).
2. **Maximised window:** the bar stays fully visible (the shell already adds the frame inset on top when zoomed). Task 2.
3. **`BC_NO_COMPOSITION=1`** (plain WebView2 hosting): dragging and the buttons work too. Task 2.
4. **The quit dialog** still appears from the Close button and Alt+F4. Task 2.
5. **A page opened in a normal browser** (no shell bridge): the bar renders and the buttons do nothing, with no error. Test in Task 1.

---

### Task 1: `TitleBar`

**Files:** Create `BreadReplay/ui/src/screen/TitleBar.tsx`, `BreadReplay/ui/src/screen/TitleBar.test.tsx`; modify `BreadReplay/ui/src/App.tsx` (render it first), the page's layout styles, and the bridge helper if a shared send/listen is added.

**Interfaces:**
- Produces: `export function TitleBar(): JSX.Element`. Sends `{type: 'window', action}` through the bridge; listens for `{type: 'windowState', maximized}`.

- [ ] **Step 1: Failing tests** (`TitleBar.test.tsx`, with the bridge mocked as `NativeVideo.test.tsx` mocks it):
  - each button posts `{"type":"window","action":"minimize"|"maximize"|"close"}`;
  - a `windowState` message with `maximized: true` turns the middle button's label to "Restore", and `false` back to "Maximise";
  - with no bridge (`window.chrome` undefined), clicking does not throw;
  - the bar element has the drag style and the buttons the no-drag style (check the class names or inline style the component uses);
  - `App.test.tsx`: the title bar is the first element of the page.
- [ ] **Step 2: Run** `npm --prefix ui test -- TitleBar` → fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `npm --prefix ui test` and `npm --prefix ui run build` → green; then the BreadReplay verification command (build + `br_tests.exe`) → green.
- [ ] **Step 5: Commit** on `br-title-bar`: `feat(ui): a title bar to move, maximise, minimise and close the window`.

### Task 2: Check it in the app, then merge

- [ ] **Step 1:** Launch BreadReplay (`build/release` shell). With UI Automation or `SendInput`, check: a drag on the bar moves the window (its `GetWindowRect` changes); a double-click maximises (`IsZoomed`) and again restores; Minimise minimises (`IsIconic`); Close shows the quit dialog (find it, press "Keep running"). Record each result in the ledger. Repeat with `BC_NO_COMPOSITION=1`.
- [ ] **Step 2:** A screenshot of the top of the window, normal and maximised, saved under `.superpowers/sdd/<plan-stem>/`; note in the ledger whether the monitors' pictures sit inside their boxes.
- [ ] **Step 3: Merge into `main`** (`merge: BreadReplay title bar`) and run the verification command once more on `main`.
