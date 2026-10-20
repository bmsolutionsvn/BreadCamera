# Project Frame Rate as Effect Budget — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Project frame rate an effect-budget knob only: allow the eleven chosen rates, stop re-timing inputs, emit one output frame per input frame, and make every output follow the camera's own rate.

**Architecture:** Replace the timer-driven master tick with an arrival-driven pass on the one GPU thread. `Camera::onVideo` posts the newest frame and wakes the engine; the engine processes only the cameras that are ready. The Project rate is stored in `settings.rate` and feeds only the stage/matte budgets. Each `VideoFrameIn` carries an optional declared rate; the camera latches it (or measures one), and `OutFrame.rate` carries it to every output.

**Tech Stack:** C++20 / MSVC (`/W4 /permissive-`), CMake + Ninja + vcpkg, Direct3D 11, React + TypeScript + Vite + Vitest, WebView2.

**Spec:** `docs/superpowers/specs/2026-10-20-project-rate-effect-budget-design.md`

## Global Constraints

- MSVC C++20, `/W4 /permissive-`; one D3D11 GPU thread; a pass never blocks; `start()`/`stop()` prompt.
- The Project rate is exactly one of: `15/1`, `24000/1001`, `24/1`, `25/1`, `30000/1001`, `30/1`, `50/1`, `60000/1001`, `60/1`, `120000/1001`, `120/1`. Nothing else.
- The Project rate feeds budgets only: Background/Upscale/Clean up `period()/4`, Matte `period()*6/10`. It never paces input or output.
- While live: one output frame per input frame, no repeat/drop against a clock.
- Before the first input frame: no output frame, phase `nosignal` (D16).
- Outputs use the camera's latched nominal rate, never the Project rate.
- Show `schemaVersion` stays `1`; old `50/1`, `60000/1001`, `60/1` shows load unchanged.
- UK English copy; conventional commits.
- Build and test from `F:\Coding\BreadMedia\BreadCamera`:
  - C++ filtered: `scripts/test.ps1 -Filter "<doctest name>"`
  - C++ all: `scripts/test.ps1`
  - UI: `cd ui; npm test`
  - Full rebuild: `scripts/build.ps1`

## Review Focus

The spec implies these conditions; each gets its test in the owning task.

1. **An input that never delivers a frame** (NDI connecting, unplugged SDI): outputs stay silent, phase stays `nosignal`, the UI does not claim live, and the preview can still show the slate (Task 4).
2. **A stalled input that resumes**: held/slate repeats use the camera's rate, then the first fresh frame returns to `live` with no backlog (Task 5).
3. **A live input whose rate changes** (NDI renegotiation): NDI/OMT metadata follow it, SRT rebuilds, SDI stops with a clear error (Task 6).
4. **A GPU slower than the input**: superseded frames are dropped, counted, and the emitted index advances so timestamps and 48 kHz audio stay on wall clock (Task 5).
5. **119.88 and 120**: NDI/OMT/SRT carry them; DeckLink maps 120 but not 119.88; AJA maps neither and errors clearly (Task 6).

---

### Task 1: Project rate allowlist and Settings dropdown

**Files:**
- Modify: `src/core/rate.cpp:5-21`, `src/core/rate.h:21-22`
- Modify: `src/core/show.cpp:162-163`
- Modify: `src/engine/command_router.cpp:471-474`
- Modify: `ui/src/state/labels.ts:23-24`
- Modify: `ui/src/dialogs/SettingsDialog.tsx:10-14,76-86`
- Test: `tests/rate_test.cpp`, `tests/show_test.cpp:55`, `tests/command_router_test.cpp:98`, `ui/src/dialogs/SettingsDialog.test.tsx`

**Interfaces:**
- Produces: `bc::parseShowRate(std::string_view) -> std::optional<Rate>` accepts exactly the eleven rationals.
- Produces: `RATE_OPTIONS: readonly [string, string][]` in `ui/src/state/labels.ts`, values as stored rationals, labels as display text.

- [ ] **Step 1: Write the failing C++ test**

Replace the first test in `tests/rate_test.cpp` with:

```cpp
TEST_CASE("Rate: parseShowRate accepts the eleven project rates") {
  const bc::Rate want[] = {
      {15, 1},   {24000, 1001}, {24, 1},        {25, 1},        {30000, 1001}, {30, 1},
      {50, 1},   {60000, 1001}, {60, 1},        {120000, 1001}, {120, 1},
  };
  for (const auto& r : want) CHECK(bc::parseShowRate(r.str()) == r);
  for (const char* s : {"14/1", "23/1", "48/1", "72/1", "90/1", "96/1", "100/1",
                        "121/1", "144/1", "60", "60/0", "abc/1", ""}) {
    CHECK_FALSE(bc::parseShowRate(s));
  }
}
```

- [ ] **Step 2: Run it and verify it fails**

Run: `scripts/test.ps1 -Filter "Rate:*parseShowRate*"`
Expected: FAIL on `30/1` (and the other new rates).

- [ ] **Step 3: Implement the allowlist**

In `src/core/rate.cpp`, replace the final three-rate check with:

```cpp
constexpr Rate kShowRates[] = {
    {15, 1},   {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1},
    {50, 1},   {60000, 1001}, {60, 1}, {120000, 1001}, {120, 1},
};
for (const auto& allowed : kShowRates) {
  if (r == allowed) return r;
}
return std::nullopt;
```

Update the comment in `src/core/rate.h` to name the eleven rates.
Update the error strings: `src/core/show.cpp` uses
`"settings.rate must be 15, 23.976, 24, 25, 29.97, 30, 50, 59.94, 60, 119.88 or 120 fps"`;
`src/engine/command_router.cpp` uses
`"rate must be 15, 23.976, 24, 25, 29.97, 30, 50, 59.94, 60, 119.88 or 120 fps"`.

- [ ] **Step 4: Update the show and router tests**

In `tests/show_test.cpp:55`, change the bad rate from `"30/1"` to `"100/1"`.
In `tests/command_router_test.cpp:98`, change the invalid rate from `"30/1"` to `"100/1"`; after the existing `50/1` assertion, add:

```cpp
REQUIRE(call(r, {{"cmd", "setSettings"}, {"args", {{"rate", "120/1"}}}})["ok"] == true);
CHECK(r.show().settings.rate == bc::Rate{120, 1});
```

- [ ] **Step 5: Run the C++ tests**

Run: `scripts/test.ps1 -Filter "Rate:*"`
Expected: PASS for the rate, show and command-router cases.

- [ ] **Step 6: Write the failing UI test**

Append to `ui/src/dialogs/SettingsDialog.test.tsx`:

```tsx
test('offers exactly the eleven project rates and sends the stored rational', () => {
  render(<SettingsDialog client={client()} onClose={() => {}} />);
  const options = within(screen.getByLabelText('Frame rate')).getAllByRole('option');
  expect(options.map((o) => o.getAttribute('value'))).toEqual([
    '15/1', '24000/1001', '24/1', '25/1', '30000/1001', '30/1',
    '50/1', '60000/1001', '60/1', '120000/1001', '120/1',
  ]);
  fireEvent.change(screen.getByLabelText('Frame rate'), { target: { value: '30/1' } });
  fireEvent.click(screen.getByRole('button', { name: 'Save' }));
  fireEvent.click(within(screen.getByRole('dialog', { name: 'Change the show format?' })).getByRole('button', { name: 'Change format' }));
  expect(sent('setSettings')[0].args).toEqual({ rate: '30/1' });
  expect(screen.getByText(/effect time budgets only/i)).toBeInTheDocument();
});
```

- [ ] **Step 7: Implement the UI list and hint**

In `ui/src/state/labels.ts` add:

```ts
export const RATE_OPTIONS = [
  ['15/1', '15'],
  ['24000/1001', '23.976'],
  ['24/1', '24'],
  ['25/1', '25'],
  ['30000/1001', '29.97'],
  ['30/1', '30'],
  ['50/1', '50'],
  ['60000/1001', '59.94'],
  ['60/1', '60'],
  ['120000/1001', '119.88'],
  ['120/1', '120'],
] as const;
const RATES: Record<string, string> = Object.fromEntries(RATE_OPTIONS);
```

In `SettingsDialog.tsx`, import `RATE_OPTIONS` and render it in the existing `<select>`; replace the local `RATES` constant. Add to the `Prop`:
`hint="Sets effect time budgets only — inputs and outputs keep their own rates."`

- [ ] **Step 8: Run the UI test**

Run: `cd ui; npm test -- SettingsDialog`
Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add src/core/rate.cpp src/core/rate.h src/core/show.cpp src/engine/command_router.cpp \
  ui/src/state/labels.ts ui/src/dialogs/SettingsDialog.tsx \
  tests/rate_test.cpp tests/show_test.cpp tests/command_router_test.cpp \
  ui/src/dialogs/SettingsDialog.test.tsx
git commit -m "feat(rate): allow the eleven project frame rates"
```

---

### Task 2: Frame rate metadata, estimator and camera nominal rate

**Files:**
- Modify: `src/io/input.h:11-16`
- Create: `src/core/rate_estimator.h`, `src/core/rate_estimator.cpp`
- Modify: `src/engine/camera.h`, `src/engine/camera.cpp:225-329`
- Modify: `tests/support/fake_input.h:19-76,92-108`
- Create: `tests/rate_estimator_test.cpp`
- Modify: `CMakeLists.txt:152-160,413-470`

**Interfaces:**
- Produces: `VideoFrameIn::rate` (`std::optional<Rate>`) — the source's declared nominal rate, empty when unknown.
- Produces: `bc::RateEstimator` with `void reset()`, `void push(std::chrono::steady_clock::time_point arrival)`, `std::optional<Rate> rate() const`.
- Produces: `Camera::setBudgetRate(Rate)`, `Camera::nominalRate() const`, `Camera::projectRate() const`.
- Consumes: `bc::Rate` and `bc::parseShowRate` from Task 1.

- [ ] **Step 1: Write the failing estimator test**

Create `tests/rate_estimator_test.cpp`:

```cpp
#include <doctest/doctest.h>

#include "core/rate_estimator.h"

#include <chrono>

using Clock = std::chrono::steady_clock;

TEST_CASE("RateEstimator: 60 Hz and 29.97 arrivals latch exact rates") {
  bc::RateEstimator e;
  auto t = Clock::now();
  for (int i = 0; i < 40; ++i) { e.push(t); t += std::chrono::nanoseconds(16'666'666); }
  REQUIRE(e.rate());
  CHECK(*e.rate() == bc::Rate{60, 1});

  e.reset();
  t = Clock::now();
  for (int i = 0; i < 40; ++i) { e.push(t); t += std::chrono::nanoseconds(33'366'667); }
  REQUIRE(e.rate());
  CHECK(*e.rate() == bc::Rate{30000, 1001});
}

TEST_CASE("RateEstimator: unstable arrivals do not latch") {
  bc::RateEstimator e;
  auto t = Clock::now();
  for (int i = 0; i < 40; ++i) {
    e.push(t);
    t += std::chrono::nanoseconds(i % 2 ? 8'000'000 : 60'000'000);
  }
  CHECK_FALSE(e.rate());
}
```

- [ ] **Step 2: Run it and verify it fails**

Run: `scripts/test.ps1 -Filter "RateEstimator:*"`
Expected: FAIL to compile (`core/rate_estimator.h` missing).

- [ ] **Step 3: Implement `RateEstimator`**

`src/core/rate_estimator.h`:

```cpp
#pragma once
#include <chrono>
#include <cstddef>
#include <optional>
#include "core/rate.h"

namespace bc {
// Derives a stable nominal input rate from frame arrival times. Needs 15 intervals; returns a
// rate only when the coefficient of variation is under 5 %. Snaps the NTSC family (23.976,
// 29.97, 59.94, 119.88) and whole rates; otherwise uses a 1/1000 rational.
class RateEstimator {
 public:
  void reset();
  void push(std::chrono::steady_clock::time_point arrival);
  std::optional<Rate> rate() const;
 private:
  static constexpr std::size_t kSamples = 15;
  std::optional<std::chrono::steady_clock::time_point> last_;
  double meanNs_ = 0, m2Ns_ = 0;
  std::size_t n_ = 0;
};
}  // namespace bc
```

Implementation notes: update the running mean/variance of intervals (Welford). When
`n_ >= kSamples` and `sqrt(m2_/n_)/mean_ < 0.05`, convert `fps = 1e9/meanNs_`; snap when within
0.5 % of `24000/1001`, `30000/1001`, `60000/1001`, `120000/1001`, or an integer; otherwise
`Rate{static_cast<int>(std::lround(fps * 1000)), 1000}` reduced by `std::gcd`. Reset on `push`
gaps over 1 s.

Add `src/core/rate_estimator.cpp` to `bc_core` and `tests/rate_estimator_test.cpp` to `bc_tests`
in `CMakeLists.txt`.

- [ ] **Step 4: Run the estimator tests**

Run: `scripts/test.ps1 -Filter "RateEstimator:*"`
Expected: PASS.

- [ ] **Step 5: Add the optional rate to `VideoFrameIn`**

In `src/io/input.h`, include `core/rate.h` and append to `VideoFrameIn`:

```cpp
  std::optional<Rate> rate;  // the source's declared nominal rate; empty when unknown
```

Aggregate initialisers that pass four fields keep compiling.

- [ ] **Step 6: Wire the nominal rate into `Camera`**

In `camera.h` add:

```cpp
  void setBudgetRate(Rate r) { budgetRate_ = r; }
  Rate nominalRate() const { return nominalRate_.value_or(budgetRate_); }
  Rate projectRate() const { return budgetRate_; }
```

Add members `Rate budgetRate_{60, 1}; std::optional<Rate> declaredRate_; std::optional<Rate> nominalRate_; RateEstimator estimator_;`.
In `Camera::onVideo`:

```cpp
if (frame.rate) {
  declaredRate_ = frame.rate;
  nominalRate_ = frame.rate;
} else if (!nominalRate_) {
  estimator_.push(now);
  if (auto r = estimator_.rate()) nominalRate_ = r;
}
```

In `Camera::startInput`, reset `declaredRate_.reset(); nominalRate_.reset(); estimator_.reset();`
before creating the input. In `Engine::apply`, call `cam->setBudgetRate(show.settings.rate)` for
every camera (existing and new).

- [ ] **Step 7: Make the fake input declare its rate**

In `tests/support/fake_input.h`, add a `bc::Rate rate_` member and constructor parameter; set
`f.rate = rate_`. In `FakeFactory::makeInput`, pass
`p.contains("rateNum") ? bc::Rate{p.value("rateNum", 30), p.value("rateDen", 1)} : bc::Rate{p.value("fps", 30), 1}`.

- [ ] **Step 8: Verify the build and tests**

Run: `scripts/test.ps1 -Filter "RateEstimator:*"`
Expected: PASS, and the engine tests still compile.

- [ ] **Step 9: Commit**

```bash
git add src/io/input.h src/core/rate_estimator.h src/core/rate_estimator.cpp \
  src/engine/camera.h src/engine/camera.cpp tests/support/fake_input.h \
  tests/rate_estimator_test.cpp CMakeLists.txt
git commit -m "feat(camera): declare and latch the input frame rate"
```

---

### Task 3: Real inputs declare their rate

**Files:**
- Modify: `src/core/rate.h`, `src/core/rate.cpp`
- Modify: `src/io/ndi_input.cpp:57-89`
- Modify: `src/io/decklink.h:16-18`, `src/io/decklink.cpp`, `src/io/decklink_input.h`, `src/io/decklink_input.cpp:80-127,140-150`
- Modify: `src/io/aja_output.h`, `src/io/aja_output.cpp`, `src/io/aja_input.cpp:55-114`
- Modify: `src/io/ffmpeg_input.cpp:265-300`
- Modify: `src/io/mf_capture_input.cpp:209-303`
- Test: `tests/rate_test.cpp`, `tests/decklink_test.cpp`

**Interfaces:**
- Produces: `std::optional<Rate> rateFromRational(int num, int den)` in `core/rate.h`.
- Produces: `std::optional<Rate> deckLinkRateFor(uint32_t displayMode)` in `io/decklink.h`.
- Produces: a file-local `std::optional<Rate> ajaRateFor(NTV2VideoFormat)` in each of
  `io/aja_input.cpp` and `io/aja_output.cpp` (no shared header; the enum belongs to the AJA SDK).
- Consumes: `VideoFrameIn::rate` from Task 2.

- [ ] **Step 1: Write the failing mapping tests**

In `tests/rate_test.cpp` add:

```cpp
TEST_CASE("Rate: rationals from protocols reject zero and negative parts") {
  CHECK(bc::rateFromRational(30000, 1001) == bc::Rate{30000, 1001});
  CHECK_FALSE(bc::rateFromRational(0, 1));
  CHECK_FALSE(bc::rateFromRational(30, 0));
  CHECK_FALSE(bc::rateFromRational(-30, 1));
}
```

In `tests/decklink_test.cpp`, inside its `#if BC_WITH_DECKLINK` block, add:

```cpp
TEST_CASE("DeckLink: display modes map to rates") {
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p2398) == bc::Rate{24000, 1001});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p24) == bc::Rate{24, 1});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p25) == bc::Rate{25, 1});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p2997) == bc::Rate{30000, 1001});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p30) == bc::Rate{30, 1});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p50) == bc::Rate{50, 1});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p5994) == bc::Rate{60000, 1001});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p6000) == bc::Rate{60, 1});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p120) == bc::Rate{120, 1});
  CHECK_FALSE(bc::deckLinkRateFor(bmdModeHD720p50));
}
```

- [ ] **Step 2: Run them and verify they fail**

Run: `scripts/test.ps1 -Filter "Rate:*rationals*"` and `scripts/test.ps1 -Filter "DeckLink:*display modes*"`
Expected: FAIL to compile.

- [ ] **Step 3: Implement the helpers**

In `core/rate.h/cpp`:

```cpp
std::optional<Rate> rateFromRational(int num, int den);  // nullopt unless both > 0
```

In `io/decklink.h/cpp`, implement `deckLinkRateFor` with the nine modes above; do **not**
reference `bmdModeHD1080p119_88` (the installed SDK has no 1080p119.88 mode). Change
`deckLinkModeFor` in Task 6.

In `io/aja_output.cpp`, add a file-local `ajaRateFor(NTV2VideoFormat)` for
`NTV2_FORMAT_1080p_2398`, `_2400`, `_2500`, `_2997`, `_3000`, `_5000_A`, `_5994_A`, `_6000_A`;
return `std::nullopt` for 119.88 and 120 (no 1080p mode in this AJA SDK). Add the same helper
(duplicated, or in an internal header) in `io/aja_input.cpp` for the input path. `formatFor` stays
as is until Task 6.

- [ ] **Step 4: Fill `VideoFrameIn::rate` in the inputs**

- NDI (`ndi_input.cpp`): `f.rate = rateFromRational(v.frame_rate_N, v.frame_rate_D);`
- DeckLink input: add `std::atomic<uint32_t> mode_{bmdModeHD1080p6000};` to the class. Set it
  in `onFormatChanged` (`mode_ = mode;`) and in `run()` where the initial `EnableVideoInput`
  uses `bmdModeHD1080p6000`. In `onFrame`:
  `f.rate = deckLinkRateFor(mode_.load());`
- AJA input: after `const NTV2VideoFormat vf = ...`, store `const auto rate = ajaRateFor(vf);`
  and set `f.rate = rate;` when building the frame.
- FFmpeg: in `emitVideo`, `if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
  out.rate = Rate{st->avg_frame_rate.num, st->avg_frame_rate.den};`
- MF capture: capture `UINT32 rateNum = 0, rateDen = 1;` from `best` next to `MF_MT_FRAME_SIZE`,
  and set `f.rate = rateFromRational(static_cast<int>(rateNum), static_cast<int>(rateDen));`
- DShow and BreadCall: leave `rate` empty; the estimator covers them.

- [ ] **Step 5: Run the focused tests**

Run: `scripts/test.ps1 -Filter "Rate:*rationals*"` then `scripts/test.ps1 -Filter "DeckLink:*display modes*"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/core/rate.h src/core/rate.cpp src/io/ndi_input.cpp src/io/decklink.h \
  src/io/decklink.cpp src/io/decklink_input.h src/io/decklink_input.cpp \
  src/io/aja_output.h src/io/aja_output.cpp src/io/aja_input.cpp \
  src/io/ffmpeg_input.cpp src/io/mf_capture_input.cpp \
  tests/rate_test.cpp tests/decklink_test.cpp
git commit -m "feat(io): declare the input frame rate at the source"
```

---

### Task 4: Arrival-driven engine and no output before the first frame

**Files:**
- Modify: `src/engine/engine.h`, `src/engine/engine.cpp:138-290`
- Modify: `src/engine/camera.h`, `src/engine/camera.cpp:225-329,433-506`
- Modify: `src/core/frame_sync.h:44-87`
- Test: `tests/engine_test.cpp`, `tests/e2e_m1_test.cpp`, `tests/e2e_m2_test.cpp`, `tests/e2e_m6_test.cpp`

**Interfaces:**
- Produces: `Camera::setWake(std::function<void()>)`.
- Produces: `Camera::ready(std::chrono::steady_clock::time_point now) const`.
- Produces: `Camera::emitIndex() const` (added here, used in Tasks 5-6).
- Produces: `FrameTap::tick` (`uint64_t`).
- Consumes: `Camera::nominalRate()` from Task 2.

- [ ] **Step 1: Write the failing engine tests**

Replace the first engine test in `tests/engine_test.cpp` with:

```cpp
TEST_CASE("Engine: a 30 fps input stays 30 fps at any Project rate") {
  for (const auto& project : {bc::Rate{15, 1}, bc::Rate{60, 1}}) {
    Rig r;
    bc::Show show = testShow();
    show.settings.rate = project;
    show.cameras.push_back(cam("a", {out("o1")}, {{"fps", 30}, {"bgra", {10, 20, 200, 255}}}));
    r.engine.apply(show);
    r.engine.start();
    auto t0 = Clock::now();
    std::this_thread::sleep_for(1500ms);
    auto seen = r.factory.output("o1")->seen();
    int n = countBetween(seen, t0 + 500ms, t0 + 1500ms);
    CHECK(n >= 27);
    CHECK(n <= 33);
    CHECK(maxGap(seen) < 60ms);
    CHECK(r.engine.stats().cameras.at("a").repeats == 0);
  }
}
```

Add:

```cpp
TEST_CASE("Engine: no output frame before the first input frame") {
  Rig r;
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("o1")}, {{"paused", true}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  CHECK(r.factory.output("o1")->seen().empty());
  CHECK(phaseOf(r.state, "a") == "nosignal");
  r.factory.input("a")->paused = false;
  std::this_thread::sleep_for(300ms);
  CHECK_FALSE(r.factory.output("o1")->seen().empty());
}
```

- [ ] **Step 2: Run them and verify they fail**

Run: `scripts/test.ps1 -Filter "Engine:*30 fps input*"` and `scripts/test.ps1 -Filter "Engine:*no output frame*"`
Expected: the first fails (≈60 frames), the second fails (slate arrives immediately).

- [ ] **Step 3: Add the wake/ready plumbing to `Camera`**

In `camera.h` add:

```cpp
  void setWake(std::function<void()> wake) { wake_ = std::move(wake); }
  bool ready(std::chrono::steady_clock::time_point now) const;
  uint64_t emitIndex() const { return emitIndex_; }
```

Add members `std::function<void()> wake_; std::atomic<uint64_t> arrivals_{0}; uint64_t taken_{0}; uint64_t emitIndex_ = 0;`.
Give `Camera::Pending` a `uint64_t seq` and set it in `onVideo`:

```cpp
const auto now = Clock::now();
const uint64_t seq = ++arrivals_;
sync_.push(Pending{std::move(frame), now, seq}, now);
if (wake_) wake_();
```

`Camera::ready`:

```cpp
bool Camera::ready(Clock::time_point now) const {
  if (arrivals_.load() > taken_) return true;
  return false;  // hold/slate deadlines are added in Task 5
}
```

In `beginTick`, after `sync_.pick(now)`, if `pick.fresh && pick.frame` set `taken_ = pick.frame->seq;`.
Keep `sawFrame_`; before the first frame `ready` is false because `arrivals_` is 0.

- [ ] **Step 4: Replace the timer loop with the arrival-driven loop**

In `engine.h`:

- Add `#include <condition_variable>` (already), `std::condition_variable wakeCv_; std::mutex wakeMutex_; std::atomic<bool> wake_{false};`.
- Change `FrameTap` to `{std::string id; uint64_t tick; TextureRef tex; std::shared_ptr<const CpuImage> cpu;}`.
- Keep `tap_` as `void(uint64_t pass, const std::vector<FrameTap>&)`; `pass` is a monotonic pass counter for the preview/tests.

In `engine.cpp` replace `run()` with this structure (keep the existing Match/matte/begin/end bodies):

```cpp
void Engine::run() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  uint64_t pass = 0;
  auto lastPublish = Clock::now();
  while (running_) {
    bool worked = false;
    std::optional<Clock::time_point> until;
    {
      std::lock_guard l(mutex_);
      const auto now = Clock::now();
      std::vector<Camera*> ready;
      for (auto& cam : cameras_) if (cam->ready(now)) ready.push_back(cam.get());
      if (now - lastPublish >= 1s) {
        lastPublish = now;
        for (auto& cam : cameras_) cam->publishStatus();
        state_.set("/engine/overruns", overruns_.load());
        if (matte_) state_.set("/engine/matteMs", matte_->lastInferenceMs());
      }
      if (!ready.empty()) {
        const auto t0 = Clock::now();
        processReady(ready, pass++);
        if (Clock::now() - t0 > settings_.rate.period()) ++overruns_;
        worked = true;
      } else {
        until = now + 1s;  // hold deadlines join this in Task 5
      }
    }
    if (worked) continue;
    std::unique_lock wl(wakeMutex_);
    wakeCv_.wait_until(wl, *until, [this] { return !running_ || wake_.exchange(false); });
  }
}
```

Refactor the existing per-pass body into `void Engine::processReady(const std::vector<Camera*>& ready, uint64_t pass)` (private, called under `mutex_`): Match wiring over all cameras, matte batch only over `ready`, begin/end only over `ready`, tap over `ready`.
`apply()` sets `cam->setWake([this] { wake_.store(true); wakeCv_.notify_all(); })` for every camera it creates or keeps, then sets `wake_ = true; wakeCv_.notify_all();` after the swap. `stop()` does the same after setting `running_ = false`.
Remove the waitable timer, `timeBeginPeriod`, `TickSchedule sched`, `index`, and `ticksPerSecond`.
For `capture()`, replace the global `index % capture_->every` check with `cam->emitIndex() % capture_->every`.

- [ ] **Step 5: Update the frame tap**

In `processReady`, push `FrameTap{cam->config().id, cam->emitIndex(), cam->lastFrame(), cam->tickCpu()}`.
`engine_main.cpp` needs no change: the tap still passes the pass counter and the frames, and the
preview rule changes in Task 6.

- [ ] **Step 6: Update the e2e count assumptions**

Any e2e test that expects output at the Project rate must expect the input rate instead. Search:
`rg "repeats|60 fps|countBetween|maxGap" tests/e2e_*.cpp`.
For fake inputs with `fps` 30, expect 30 fps; for 60, expect 60. Do not change the hold/slate assertions yet (Task 5).

- [ ] **Step 7: Run the engine tests**

Run: `scripts/test.ps1 -Filter "Engine:*"`
Expected: PASS for the two new tests; unrelated engine tests may still fail on timing and are fixed in Task 5.

- [ ] **Step 8: Commit**

```bash
git add src/engine/engine.h src/engine/engine.cpp src/engine/camera.h src/engine/camera.cpp \
  src/core/frame_sync.h tests/engine_test.cpp \
  tests/e2e_m1_test.cpp tests/e2e_m2_test.cpp tests/e2e_m6_test.cpp
git commit -m "feat(engine): process cameras as their inputs arrive"
```

---

### Task 5: Hold, slate, audio, drops and half rate on the emitted frame

**Files:**
- Modify: `src/engine/camera.h`, `src/engine/camera.cpp:433-601`
- Modify: `src/engine/engine.cpp` (`processReady`)
- Modify: `tests/engine_test.cpp`, `tests/frame_sync_test.cpp`
- Test: `tests/engine_test.cpp`

**Interfaces:**
- Consumes: `Camera::ready`, `Camera::emitIndex` from Task 4.
- Produces: `std::optional<Clock::time_point> Camera::nextRepeatAt(Clock::time_point now) const`.
- Produces: `Camera::drops()` (cumulative superseded frames, already in `FrameSync`) used to advance the index.

- [ ] **Step 1: Write the failing hold and audio tests**

Replace the audio test:

```cpp
TEST_CASE("Engine: each emitted frame carries one frame of audio at the input rate") {
  Rig r;
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("o1")}, {{"fps", 30}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  for (auto& s : r.factory.output("o1")->seen()) CHECK(s.audioSamples == 1600 * 2);
}
```

Keep the hold/slate test, but change `maxGap` to `< 60ms` (30 fps repeats) and assert the output
keeps arriving after the input pauses.

Add:

```cpp
TEST_CASE("Engine: half rate sends every second emitted frame and carries its audio") {
  Rig r;
  bc::Show show = testShow();
  auto o = out("o1");
  o.halfRate = true;
  show.cameras.push_back(cam("a", {o}, {{"fps", 30}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1000ms);
  auto seen = r.factory.output("o1")->seen();
  CHECK(seen.size() >= 13);
  CHECK(seen.size() <= 17);
  for (auto& s : seen) CHECK(s.audioSamples == 3200 * 2);
}
```

Add a drop/time test:

```cpp
TEST_CASE("Engine: a slow output keeps timestamps and audio on wall clock") {
  // fake output delayMs = 20 with a 60 fps input for 1 s: frames are coalesced, the visible
  // ticks still advance by one per elapsed 1/60 s.
  Rig r;
  bc::Show show = testShow();
  auto o = out("o1", {{"delayMs", 20}});
  show.cameras.push_back(cam("a", {o}, {{"fps", 60}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1200ms);
  auto seen = r.factory.output("o1")->seen();
  REQUIRE(seen.size() > 2);
  CHECK(r.engine.stats().cameras.at("a").drops > 0);
  CHECK(seen.back().tick >= seen.front().tick + static_cast<uint64_t>(seen.size() * 0.8));
}
```

- [ ] **Step 2: Run and verify they fail**

Run: `scripts/test.ps1 -Filter "Engine:*audio*"` and `scripts/test.ps1 -Filter "Engine:*half rate*"`
Expected: audio count is 800×2 (60 Hz show rate) and half rate is not tied to the camera.

- [ ] **Step 3: Make `ready` include hold/slate deadlines**

In `camera.h` add:

```cpp
  std::optional<std::chrono::steady_clock::time_point> nextRepeatAt(std::chrono::steady_clock::time_point now) const;
```

Add members `std::optional<Clock::time_point> nextRepeat_; std::optional<Clock::time_point> lastArrival_; SyncPolicy policy_;`.
Keep `policy_` in step with `setPolicy`: `{ policy_ = p; sync_.setPolicy(p); }`; initialise it from the
constructor argument.
`ready` becomes:

```cpp
bool Camera::ready(Clock::time_point now) const {
  if (arrivals_.load() > taken_) return true;
  if (!sawFrame_) return false;
  return nextRepeat_ && now >= *nextRepeat_;
}
```

`nextRepeatAt`:

```cpp
std::optional<Clock::time_point> Camera::nextRepeatAt(Clock::time_point now) const {
  if (!sawFrame_) return std::nullopt;
  const auto stallAt = *lastArrival_ + policy_.stallAfter;
  if (now < stallAt) return stallAt;
  return nextRepeat_ ? nextRepeat_ : std::optional<Clock::time_point>{stallAt};
}
```

Record `lastArrival_ = now;` in `onVideo`. In `beginTick`, after processing: if the phase is
`Holding` or `Slate`, set
`nextRepeat_ = (nextRepeat_ ? *nextRepeat_ : *lastArrival_) + nominalRate().period()`;
otherwise `nextRepeat_.reset()`.
When a repeat is emitted, advance `emitIndex_` by 1.

- [ ] **Step 4: Advance the emitted index across drops and drive audio from it**

In `beginTick`, after `sync_.pick(now)`, derive the dropped count from the sequence numbers:

```cpp
uint64_t dropped = 0;
if (pick.fresh && pick.frame) {
  dropped = pick.frame->seq - taken_ - 1;  // frames superseded since the last taken one
  taken_ = pick.frame->seq;
}
```

On any emitted frame (fresh or repeat):

```cpp
tickIndex_ = emitIndex_;
emitIndex_ += pick.fresh ? dropped + 1 : 1;
```

Add `uint64_t tickIndex_ = 0;` to the class.
In `endTick`, use `tickIndex_` and `nominalRate()`:

```cpp
OutFrame{tickIndex_, nominalRate(), frame, outCpu, audio, 48000, 2, hasAlpha}
```

Replace `sched.audioFramesForTick(index, 48000)` with
`audioSched_.audioFramesForTick(tickIndex_, 48000)`. Add a member
`TickSchedule audioSched_{Rate{60, 1}};` and rebuild it (`audioSched_ = TickSchedule(nominalRate());`)
whenever `nominalRate_` changes in `onVideo`. `endTick` no longer takes a `TickSchedule` parameter.

- [ ] **Step 5: Make half rate per-output**

In `OutputSlot` add `uint64_t emitted = 0;`. Replace the half-rate block in `endTick` with:

```cpp
std::shared_ptr<const std::vector<float>> outAudio = audio;
if (s.cfg.halfRate && s.emitted++ % 2 == 1) {
  s.carryAudio = *audio;
  continue;
}
if (s.cfg.halfRate) {
  auto both = std::make_shared<std::vector<float>>(std::move(s.carryAudio));
  both->insert(both->end(), audio->begin(), audio->end());
  s.carryAudio.clear();
  outAudio = both;
}
const Rate outRate = s.cfg.halfRate
    ? Rate{nominalRate().num, nominalRate().den * 2}
    : nominalRate();
s.out->submit(OutFrame{tickIndex_, outRate, frame, outCpu, outAudio, 48000, 2, hasAlpha});
```

Fresh and repeat frames both count.

- [ ] **Step 6: Make the engine wake for hold deadlines**

In `processReady`/`run`, compute the earliest `nextRepeatAt` across cameras and use it as the
`wait_until` deadline (instead of a fixed 1 s), still capped at 1 s for housekeeping.
Only call `cam->beginTick`/`endTick` for ready cameras, as in Task 4.

- [ ] **Step 7: Budgets come from the Project rate**

Replace `sched.rate().period() / 4` in `beginTick`/`endTick` and the timer polls with
`projectRate().period() / 4`. Replace `sched.rate().period() * 6 / 10` in `Engine::processReady`
with `settings_.rate.period() * 6 / 10`.

- [ ] **Step 8: Run the engine tests**

Run: `scripts/test.ps1 -Filter "Engine:*"`
Expected: PASS, including hold/slate, audio, half rate and drops.

- [ ] **Step 9: Commit**

```bash
git add src/engine/camera.h src/engine/camera.cpp src/engine/engine.cpp \
  tests/engine_test.cpp tests/frame_sync_test.cpp
git commit -m "feat(engine): pace hold, audio and half rate from the camera frame"
```

---

### Task 6: Every output follows the camera rate

**Files:**
- Modify: `src/engine/adapter_factory.h:13-34`, `src/engine/adapter_factory.cpp:136-157`
- Modify: `src/engine/camera.cpp:239-260`
- Modify: `src/engine/engine.cpp` (`apply` no longer calls `setShowRate`)
- Modify: `src/io/decklink.h`, `src/io/decklink.cpp:74-79`
- Modify: `src/io/aja_output.cpp:17-22`
- Modify: `src/io/srt_output.cpp:37-51,103-108`, `src/io/srt_output.h`
- Modify: `src/io/vcam_output.cpp:38-42`
- Modify: `src/engine/preview_service.h`, `src/engine/preview_service.cpp:170-172`
- Modify: `tests/support/fake_input.h`, `tests/e2e_*.cpp`, `tests/preview_test.cpp`
- Test: `tests/decklink_test.cpp`, `tests/engine_test.cpp`, `tests/srt_output_test.cpp`, `tests/preview_test.cpp`

**Interfaces:**
- Produces: `AdapterFactory::makeOutput(const OutputConfig&, const CameraConfig&, Rate videoRate)`.
- Consumes: `Camera::nominalRate()` from Task 2 and `OutFrame.rate` from Task 5.
- Produces: `PreviewService::onTick(uint64_t pass, const std::vector<FrameTap>&)` with a per-camera 33 ms cap.

- [ ] **Step 1: Write the failing output-rate tests**

In `tests/decklink_test.cpp` add:

```cpp
TEST_CASE("DeckLink: every mapped 1080p rate has a mode") {
  CHECK(bc::deckLinkModeFor(bc::Rate{24000, 1001}) == bmdModeHD1080p2398);
  CHECK(bc::deckLinkModeFor(bc::Rate{24, 1}) == bmdModeHD1080p24);
  CHECK(bc::deckLinkModeFor(bc::Rate{25, 1}) == bmdModeHD1080p25);
  CHECK(bc::deckLinkModeFor(bc::Rate{30000, 1001}) == bmdModeHD1080p2997);
  CHECK(bc::deckLinkModeFor(bc::Rate{30, 1}) == bmdModeHD1080p30);
  CHECK(bc::deckLinkModeFor(bc::Rate{50, 1}) == bmdModeHD1080p50);
  CHECK(bc::deckLinkModeFor(bc::Rate{60000, 1001}) == bmdModeHD1080p5994);
  CHECK(bc::deckLinkModeFor(bc::Rate{60, 1}) == bmdModeHD1080p6000);
  CHECK(bc::deckLinkModeFor(bc::Rate{120, 1}) == bmdModeHD1080p120);
  CHECK_THROWS_AS(bc::deckLinkModeFor(bc::Rate{120000, 1001}), std::invalid_argument);
}
```

In `tests/engine_test.cpp` add:

```cpp
TEST_CASE("Engine: the output frame carries the camera rate, not the Project rate") {
  Rig r;
  bc::Show show = testShow();
  show.settings.rate = bc::Rate{15, 1};
  show.cameras.push_back(cam("a", {out("o1")}, {{"rateNum", 30000}, {"rateDen", 1001}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(700ms);
  auto seen = r.factory.output("o1")->seen();
  REQUIRE_FALSE(seen.empty());
  CHECK(r.factory.output("o1")->lastRate() == bc::Rate{30000, 1001});
}
```

Add `bc::Rate lastRate() const` to `FakeOutput`, recording `f.rate`.

In `tests/preview_test.cpp` add a case that calls `onTick` twice for the same camera within 20 ms
and asserts one encode, then after 40 ms asserts a second. Use the existing preview harness.

- [ ] **Step 2: Run and verify they fail**

Run: `scripts/test.ps1 -Filter "DeckLink:*every mapped*"` and `scripts/test.ps1 -Filter "Engine:*carries the camera rate*"`
Expected: FAIL to compile / wrong rate.

- [ ] **Step 3: Pass the rate through the factory**

In `adapter_factory.h`:

```cpp
virtual std::unique_ptr<Output> makeOutput(const OutputConfig& cfg, const CameraConfig& cam, Rate videoRate) = 0;
```

Remove `setShowRate` and `rate_` from `DefaultAdapterFactory`; use `videoRate` for
`DeckLinkOutput(...)` and `AjaOutput(...)`. Update `Engine::apply` (remove the call) and every
fake factory in `tests/` (`e2e_m1`, `e2e_m2`, `e2e_m6`, `engine_test`, `support/fake_input.h`).

In `Camera::startOutput`, call `factory_.makeOutput(s.cfg, cfg_, nominalRate())`. For
`decklink`/`aja`, if `!nominalRate_` (the input has never delivered a frame), mark the slot
`deferred = true` and do not create it; on the first fresh frame in `beginTick`, create and start
deferred outputs. (D16 already keeps the output silent.)

- [ ] **Step 4: Map every supported SDI rate**

`deckLinkModeFor` (DeckLink): the nine modes from Step 1; throw for 119.88 and anything else.
`formatFor` (AJA): the eight modes from Task 3; throw for 119.88 and 120 with
`"AJA SDI does not support " + r.str()`.

- [ ] **Step 5: Rebuild SRT on a rate change**

In `srt_output.h` add `Rate encRate_;`. In `openEncoders`, set `encRate_ = f.rate;`.
In `SrtOutput::deliver`, change the guard to:

```cpp
if (!venc_ || w != encW_ || h != encH_ || !(f.rate == encRate_)) openEncoders(f);
```

Keep the existing timestamp `vpts_++`; the encoder time base is rebuilt with the rate.

- [ ] **Step 6: Stop halving the virtual camera**

Delete `if (++frames_ % 2) return;` in `VirtualCameraOutput::deliver`. Keep the writer open-retry
logic, but base it on `frames_ % 30 == 0 || frames_ == 1`.

- [ ] **Step 7: Cap the preview per camera by wall clock**

In `PreviewService`, replace the global `if (tick % 2 != 0) return;` with a per-camera
`std::map<std::string, Clock::time_point> lastEncode_;` and a 33 ms cap:

```cpp
const auto now = std::chrono::steady_clock::now();
for (const auto& f : frames) {
  auto it = lastEncode_.find(f.id);
  if (it != lastEncode_.end() && now - it->second < 33ms) continue;
  lastEncode_[f.id] = now;
  // existing per-camera encode path for f
}
```

If the current code loops frames only when the global tick is even, move the loop body inside this
filter. `onTick` keeps the `pass` parameter for compatibility; it is no longer used for pacing.

- [ ] **Step 8: Run the focused tests**

Run: `scripts/test.ps1 -Filter "DeckLink:*"`, `scripts/test.ps1 -Filter "Engine:*"`,
`scripts/test.ps1 -Filter "Preview:*"`, `scripts/test.ps1 -Filter "SRT:*"`
Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add src/engine/adapter_factory.h src/engine/adapter_factory.cpp src/engine/camera.cpp \
  src/engine/engine.cpp src/io/decklink.h src/io/decklink.cpp src/io/aja_output.cpp \
  src/io/srt_output.h src/io/srt_output.cpp src/io/vcam_output.cpp \
  src/engine/preview_service.h src/engine/preview_service.cpp \
  tests/support/fake_input.h tests/decklink_test.cpp tests/engine_test.cpp \
  tests/srt_output_test.cpp tests/preview_test.cpp tests/e2e_*.cpp
git commit -m "feat(io): outputs follow the camera frame rate"
```

---

### Task 7: Status, docs and the whole suite

**Files:**
- Modify: `ui/src/shell/StatusBar.tsx:35-48`
- Modify: `ui/src/dialogs/AddOutputDialog.tsx:33`
- Modify: `PRODUCT.md:71,81-82`
- Modify: `docs/superpowers/specs/2026-10-08-breadcamera-design.md:63-65`
- Modify: `docs/superpowers/specs/2026-10-17-no-master-clock-design.md` (note superseded decisions)
- Test: `ui/src/Workspace.test.tsx`, `ui/src/shell/StatusBar` if a test exists, `tests/rate_test.cpp`

**Interfaces:**
- Consumes: `/engine/rate` still holds the Project rate (Task 2/5).

- [ ] **Step 1: Write the failing status test**

In `ui/src/Workspace.test.tsx`, update the status-bar assertion to:

```tsx
expect(within(bar).getByText('Project rate')).toBeInTheDocument();
expect(within(bar).getByText('59.94')).toBeInTheDocument();
```

Add `expect(within(bar).queryByText(/tick overruns/)).toBeNull();` and
`expect(within(bar).getByText('Budget on time')).toBeInTheDocument();` for the zero-overrun
snapshot.

- [ ] **Step 2: Run it and verify it fails**

Run: `cd ui; npm test -- Workspace`
Expected: FAIL (the label is still the bare rate).

- [ ] **Step 3: Update the status bar and copy**

In `StatusBar.tsx`, render:

```tsx
<span>
  Project rate <b>{rateLabel(e.rate)}</b> fps
</span>
```

and replace `tick overruns` with `budget overruns`, and `Ticks on time` with `Budget on time`;
keep the heaviest-stage line.
In `AddOutputDialog.tsx`, change `FIXED_FORMAT_NOTE` to:
`'SDI uses 1080p at the camera rate; the virtual camera is 1080p30.'`
In `PRODUCT.md`, replace the three-rate claims with the eleven-rate allowlist and the
budget-only meaning.
In the 2026-10-08 spec, amend §1.3 to name the eleven rates and state that the Project rate sets
effect budgets only; add `Amended 2026-10-20: ...` at the top. In the 2026-10-17 design, add a
note at the top that 2026-10-20 supersedes D3 (budgets from the Project rate) and D9 (the rate
stays).

- [ ] **Step 4: Run the UI tests**

Run: `cd ui; npm test`
Expected: PASS.

- [ ] **Step 5: Run the whole C++ suite**

Run: `scripts/test.ps1`
Expected: PASS (hardware-dependent cases skip as today).

- [ ] **Step 6: Full rebuild**

Run: `scripts/build.ps1`
Expected: exit 0; the UI is staged to `build/release/ui`.

- [ ] **Step 7: Commit**

```bash
git add ui/src/shell/StatusBar.tsx ui/src/dialogs/AddOutputDialog.tsx \
  ui/src/Workspace.test.tsx PRODUCT.md \
  docs/superpowers/specs/2026-10-08-breadcamera-design.md \
  docs/superpowers/specs/2026-10-17-no-master-clock-design.md
git commit -m "docs(rate): record the project rate as an effect budget"
```

---

## Self-Review Notes

- **Spec coverage:** allowlist (Task 1), input rate metadata (Tasks 2-3), arrival-driven live path
  and D16 (Task 4), hold/slate/audio/half-rate/drops (Task 5), outputs/SDI/SRT/vcam/preview
  (Task 6), status/docs/full suite (Task 7).
- **Type consistency:** `Camera::ready`, `emitIndex`, `nextRepeatAt`, `nominalRate`,
  `setBudgetRate`, `makeOutput(..., Rate)`, `FrameTap::tick`, `PreviewService::onTick(pass, frames)`
  are introduced once and consumed by the named later tasks.
- **Review Focus tests:** no-frame startup (Task 4), stall/resume (Task 5), rate change (Task 6),
  coalescing/time (Task 5), 119.88/120 (Task 6).
- **Proportion:** code blocks are limited to test assertions, fixed signatures and the loop
  skeleton; the rest is file, interface and verification instructions.
