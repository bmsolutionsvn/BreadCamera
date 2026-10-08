#include <doctest/doctest.h>

#include "core/rate.h"

using namespace std::chrono_literals;

TEST_CASE("Rate: parseShowRate accepts only the three show rates") {
  CHECK(bc::parseShowRate("60/1") == bc::Rate{60, 1});
  CHECK(bc::parseShowRate("50/1") == bc::Rate{50, 1});
  CHECK(bc::parseShowRate("60000/1001") == bc::Rate{60000, 1001});
  CHECK_FALSE(bc::parseShowRate("30/1"));
  CHECK_FALSE(bc::parseShowRate("60"));
  CHECK_FALSE(bc::parseShowRate("60/0"));
  CHECK_FALSE(bc::parseShowRate("abc/1"));
  CHECK_FALSE(bc::parseShowRate(""));
}

TEST_CASE("Rate: period and string form") {
  CHECK(bc::Rate{60, 1}.period() == 16'666'666ns);
  CHECK(bc::Rate{50, 1}.period() == 20ms);
  CHECK(bc::Rate{60000, 1001}.period() == 16'683'333ns);
  CHECK(bc::Rate{60000, 1001}.str() == "60000/1001");
  CHECK(bc::Rate{60000, 1001}.fps() == doctest::Approx(59.94).epsilon(0.001));
}
