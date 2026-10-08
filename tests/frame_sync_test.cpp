// bc::toString(FeedPhase) would otherwise be found by ADL inside doctest's own
// stringifyBinaryExpr, yielding `const char* + const char*` (C2110). Qualifying the
// macro keeps every CHECK below verbatim.
#define DOCTEST_STRINGIFY(...) doctest::toString(__VA_ARGS__)
#include <doctest/doctest.h>

#include "core/frame_sync.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST_CASE("FrameSync: no frame yet is NoSignal with no frame") {
  bc::FrameSync<int> sync({250ms, 5000ms});
  auto p = sync.pick(Clock::now());
  CHECK(p.phase == bc::FeedPhase::NoSignal);
  CHECK_FALSE(p.frame);
}

TEST_CASE("FrameSync: newest frame wins, older unpicked frames count as drops") {
  bc::FrameSync<int> sync({250ms, 5000ms});
  auto t0 = Clock::now();
  sync.push(1, t0);
  sync.push(2, t0);
  auto p = sync.pick(t0);
  CHECK(p.frame == 2);
  CHECK(p.fresh);
  CHECK(p.phase == bc::FeedPhase::Live);
  CHECK(sync.drops() == 1);
}

TEST_CASE("FrameSync: a slow input repeats its last frame") {
  bc::FrameSync<int> sync({250ms, 5000ms});
  auto t0 = Clock::now();
  sync.push(7, t0);
  sync.pick(t0);
  auto p = sync.pick(t0 + 16ms);
  CHECK(p.frame == 7);
  CHECK_FALSE(p.fresh);
  CHECK(sync.repeats() == 1);
}

TEST_CASE("FrameSync: stalled input holds, then goes to slate") {
  bc::FrameSync<int> sync({250ms, 1000ms});
  auto t0 = Clock::now();
  sync.push(3, t0);
  CHECK(sync.pick(t0 + 100ms).phase == bc::FeedPhase::Live);
  auto hold = sync.pick(t0 + 300ms);
  CHECK(hold.phase == bc::FeedPhase::Holding);
  CHECK(hold.frame == 3);
  auto slate = sync.pick(t0 + 1300ms);
  CHECK(slate.phase == bc::FeedPhase::Slate);
  CHECK_FALSE(slate.frame);
  sync.push(4, t0 + 1400ms);
  CHECK(sync.pick(t0 + 1400ms).phase == bc::FeedPhase::Live);
}

TEST_CASE("FrameSync: reset forgets the last frame") {
  bc::FrameSync<int> sync({250ms, 1000ms});
  auto t0 = Clock::now();
  sync.push(3, t0);
  sync.reset();
  CHECK(sync.pick(t0).phase == bc::FeedPhase::NoSignal);
}
