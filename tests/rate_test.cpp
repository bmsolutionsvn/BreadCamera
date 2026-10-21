#include <doctest/doctest.h>

#include "core/rate.h"

using namespace std::chrono_literals;

TEST_CASE("Rate: parseShowRate accepts the eleven project rates") {
  const bc::Rate want[] = {
      {15, 1}, {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1},
      {50, 1}, {60000, 1001}, {60, 1}, {120000, 1001}, {120, 1},
  };
  for (const auto& r : want) CHECK(bc::parseShowRate(r.str()) == r);
  for (const char* s : {"14/1", "23/1", "48/1", "72/1", "90/1", "96/1", "100/1",
                        "121/1", "144/1", "60", "60/0", "abc/1", ""}) {
    CHECK_FALSE(bc::parseShowRate(s));
  }
}

TEST_CASE("Rate: rationals from protocols reject zero and negative parts") {
  CHECK(bc::rateFromRational(30000, 1001) == bc::Rate{30000, 1001});
  CHECK_FALSE(bc::rateFromRational(0, 1));
  CHECK_FALSE(bc::rateFromRational(30, 0));
  CHECK_FALSE(bc::rateFromRational(-30, 1));
}

TEST_CASE("Rate: period and string form") {
  CHECK(bc::Rate{60, 1}.period() == 16'666'666ns);
  CHECK(bc::Rate{50, 1}.period() == 20ms);
  CHECK(bc::Rate{60000, 1001}.period() == 16'683'333ns);
  CHECK(bc::Rate{60000, 1001}.str() == "60000/1001");
  CHECK(bc::Rate{60000, 1001}.fps() == doctest::Approx(59.94).epsilon(0.001));
}
