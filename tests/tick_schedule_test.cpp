#include <doctest/doctest.h>

#include "core/tick_schedule.h"

using namespace std::chrono_literals;

TEST_CASE("TickSchedule: offsets are exact, with no accumulated rounding") {
  bc::TickSchedule s({60000, 1001});
  CHECK(s.offset(0) == 0ns);
  CHECK(s.offset(60000) == std::chrono::nanoseconds(1001'000'000'000LL));  // exactly 1001 s
  CHECK(s.offset(1) == 16'683'333ns);
  bc::TickSchedule s60({60, 1});
  CHECK(s60.offset(60) == 1s);
  CHECK(s60.offset(3) == 50ms);
}

TEST_CASE("TickSchedule: nextIndexAtOrAfter skips missed ticks instead of bursting") {
  bc::TickSchedule s({60, 1});
  CHECK(s.nextIndexAtOrAfter(0ns) == 0);
  CHECK(s.nextIndexAtOrAfter(1ns) == 1);
  CHECK(s.nextIndexAtOrAfter(50ms) == 3);  // exactly on tick 3
  CHECK(s.nextIndexAtOrAfter(51ms) == 4);
}

TEST_CASE("TickSchedule: audio frames per tick sum exactly to the sample rate") {
  bc::TickSchedule s({60000, 1001});
  uint64_t sum = 0;
  for (uint64_t i = 0; i < 5; ++i) sum += s.audioFramesForTick(i, 48000);
  CHECK(sum == 4004);  // 48000 * 1001 * 5 / 60000
  bc::TickSchedule s60({60, 1});
  CHECK(s60.audioFramesForTick(0, 48000) == 800);
}
