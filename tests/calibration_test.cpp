#include <doctest/doctest.h>

#include <cmath>

#include "core/calibration.h"

namespace {
// A camera with channel crosstalk, more gain and a slightly different tone response.
bc::ChartColors withCast(const bc::ChartColors& ref) {
  bc::ChartColors out;
  for (size_t i = 0; i < 24; ++i) {
    const auto c = ref[i];
    const float r = 0.85f * c.r + 0.10f * c.g + 0.02f * c.b;
    const float g = 0.05f * c.r + 0.80f * c.g + 0.05f * c.b;
    const float b = 0.02f * c.r + 0.08f * c.g + 0.65f * c.b;
    out[i] = {std::pow(std::min(1.0f, r * 1.1f), 1.08f), std::pow(std::min(1.0f, g * 1.1f), 1.08f), std::pow(std::min(1.0f, b * 1.1f), 1.08f)};
  }
  return out;
}
}  // namespace

TEST_CASE("solveCalibration: a camera with a known cast is corrected below 2 dE") {
  const auto& ref = bc::colorCheckerReference();
  auto cal = bc::solveCalibration(withCast(ref), ref);
  MESSAGE("mean dE2000 before " << cal.deltaEBefore << ", after " << cal.deltaEAfter);
  CHECK(cal.deltaEBefore > 5);
  CHECK(cal.deltaEAfter < 2);
  CHECK(cal.lut.size == 33);
  CHECK(bc::calibrationRating(cal.deltaEAfter) == "good");
}

TEST_CASE("solveCalibration: a camera that already matches gets a near-identity LUT") {
  const auto& ref = bc::colorCheckerReference();
  auto cal = bc::solveCalibration(ref, ref);
  CHECK(cal.deltaEAfter < 0.5);
  auto mid = cal.lut.apply({0.5f, 0.5f, 0.5f});
  CHECK(mid.r == doctest::Approx(0.5).epsilon(0.02));
  CHECK(mid.b == doctest::Approx(0.5).epsilon(0.02));
}

TEST_CASE("solveCalibration: too dark or clipped charts are refused; an extreme cast stays bounded") {
  const auto& ref = bc::colorCheckerReference();
  bc::ChartColors dark;
  for (size_t i = 0; i < 24; ++i) dark[i] = {ref[i].r * 0.005f, ref[i].g * 0.005f, ref[i].b * 0.005f};
  CHECK_THROWS_WITH_AS(bc::solveCalibration(dark, ref), doctest::Contains("too dark"), std::invalid_argument);
  auto clipped = ref;
  clipped[18] = {1, 1, 1};
  CHECK_THROWS_WITH_AS(bc::solveCalibration(clipped, ref), doctest::Contains("clipped"), std::invalid_argument);
  bc::ChartColors blue;
  for (size_t i = 0; i < 24; ++i) blue[i] = {ref[i].r * 0.3f, ref[i].g * 0.5f, std::min(0.9f, ref[i].b * 3.0f)};
  auto cal = bc::solveCalibration(blue, ref);
  for (auto& e : cal.lut.data) {
    CHECK(std::isfinite(e.r));
    CHECK(e.g >= 0.0f);
    CHECK(e.b <= 1.0f);
  }
}

TEST_CASE("calibrationRating words") {
  CHECK(bc::calibrationRating(1.9) == "good");
  CHECK(bc::calibrationRating(3.5) == "fair");
  CHECK(bc::calibrationRating(6.0) == "poor");
}
