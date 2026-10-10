#include <doctest/doctest.h>

#include "core/stage_guard.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

TEST_CASE("StageGuard: a healthy stage stays active") {
  bc::StageGuard g;
  auto t = Clock::now();
  CHECK(g.shouldRun(t));
  g.succeeded(5ms, 10ms, t);
  CHECK_FALSE(g.bypassed());
  CHECK(g.stateName(true) == "active");
  CHECK(g.stateName(false) == "off");
}

TEST_CASE("StageGuard: sustained overrun bypasses, then retries after 5 s") {
  bc::StageGuard g;
  auto t = Clock::now();
  for (int i = 0; i < 29; ++i) g.succeeded(20ms, 10ms, t);
  CHECK_FALSE(g.bypassed());
  g.succeeded(20ms, 10ms, t);
  CHECK(g.bypassed());
  REQUIRE(g.error());
  CHECK(g.error()->code == "stage-overrun");
  CHECK_FALSE(g.shouldRun(t + 4s));
  CHECK(g.shouldRun(t + 5s));
}

TEST_CASE("StageGuard: one in-budget run clears the overrun streak") {
  bc::StageGuard g;
  auto t = Clock::now();
  for (int i = 0; i < 29; ++i) g.succeeded(20ms, 10ms, t);
  g.succeeded(5ms, 10ms, t);
  for (int i = 0; i < 29; ++i) g.succeeded(20ms, 10ms, t);
  CHECK_FALSE(g.bypassed());
}

TEST_CASE("StageGuard: a failed trial bypasses again at once; a good trial recovers") {
  bc::StageGuard g;
  auto t = Clock::now();
  g.failed("boom", t);
  CHECK(g.bypassed());
  CHECK(g.error()->code == "stage-failed");
  CHECK(g.error()->message == "boom");
  g.succeeded(20ms, 10ms, t + 5s);  // trial over budget
  CHECK(g.bypassed());
  CHECK_FALSE(g.shouldRun(t + 9s));
  CHECK(g.shouldRun(t + 10s));
  g.succeeded(5ms, 10ms, t + 10s);  // good trial
  CHECK_FALSE(g.bypassed());
  CHECK_FALSE(g.error());
}
