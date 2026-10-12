#include <doctest/doctest.h>

#include <cmath>

#include "core/color_math.h"

TEST_CASE("deltaE2000 matches the Sharma reference pair") {
  bc::Lab a{50.0, 2.6772, -79.7751}, b{50.0, 0.0, -82.7485};
  CHECK(bc::deltaE2000(a, b) == doctest::Approx(2.0425).epsilon(0.0005));
  bc::Lab c{50.0, 2.5, 0.0}, d{73.0, 25.0, -18.0};
  CHECK(bc::deltaE2000(c, d) == doctest::Approx(27.1492).epsilon(0.0005));
  CHECK(bc::deltaE2000(a, a) == doctest::Approx(0.0));
}

TEST_CASE("Lab round trip and white point") {
  auto white = bc::linearToLab({1, 1, 1});
  CHECK(white.L == doctest::Approx(100.0).epsilon(0.001));
  CHECK(std::abs(white.a) < 0.01);
  bc::Rgb x{0.2f, 0.5f, 0.1f};
  auto back = bc::labToLinear(bc::linearToLab(x));
  CHECK(back.r == doctest::Approx(0.2).epsilon(0.001));
  CHECK(back.g == doctest::Approx(0.5).epsilon(0.001));
  CHECK(back.b == doctest::Approx(0.1).epsilon(0.001));
  CHECK(bc::toLinearLight(bc::toGamma(0.3f)) == doctest::Approx(0.3f).epsilon(0.0001));
}
