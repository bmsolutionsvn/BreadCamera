#include <doctest/doctest.h>

#include <chrono>

#include "core/rate_estimator.h"

using Clock = std::chrono::steady_clock;

TEST_CASE("RateEstimator: 60 Hz and 29.97 arrivals latch exact rates") {
  bc::RateEstimator e;
  auto t = Clock::now();
  for (int i = 0; i < 40; ++i) {
    e.push(t);
    t += std::chrono::nanoseconds(16'666'666);
  }
  REQUIRE(e.rate());
  CHECK(*e.rate() == bc::Rate{60, 1});

  e.reset();
  t = Clock::now();
  for (int i = 0; i < 40; ++i) {
    e.push(t);
    t += std::chrono::nanoseconds(33'366'667);
  }
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
