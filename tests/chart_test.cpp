#include <doctest/doctest.h>

#include <algorithm>

#include "core/chart.h"
#include "support/chart_image.h"

namespace {
const bc::ChartQuad kTilted{{{{0.30, 0.25}, {0.72, 0.22}, {0.74, 0.70}, {0.28, 0.72}}}};

void checkMatchesReference(const bc::ChartColors& got) {
  for (size_t i = 0; i < 24; ++i) {
    const auto& want = bc::colorCheckerSrgb()[i];
    CHECK(std::abs(bc::toGamma(got[i].r) * 255 - want[0]) <= 2.5);
    CHECK(std::abs(bc::toGamma(got[i].g) * 255 - want[1]) <= 2.5);
    CHECK(std::abs(bc::toGamma(got[i].b) * 255 - want[2]) <= 2.5);
  }
}
}  // namespace

TEST_CASE("chartToPicture maps the corners, inverts, and refuses a degenerate quad") {
  auto h = bc::chartToPicture(kTilted);
  auto tl = h.map(0, 0), br = h.map(1, 1);
  CHECK(tl[0] == doctest::Approx(0.30));
  CHECK(br[1] == doctest::Approx(0.70));
  auto back = h.inverse().map(0.74, 0.70);
  CHECK(back[0] == doctest::Approx(1.0).epsilon(1e-6));
  CHECK(back[1] == doctest::Approx(1.0).epsilon(1e-6));
  CHECK_THROWS_AS(bc::chartToPicture(bc::ChartQuad{{{{0.1, 0.1}, {0.1, 0.1}, {0.1, 0.1}, {0.1, 0.1}}}}), std::invalid_argument);
  CHECK(bc::validQuad(kTilted));
  CHECK_FALSE(bc::validQuad(bc::ChartQuad{{{{0.3, 0.3}, {0.31, 0.3}, {0.31, 0.31}, {0.3, 0.31}}}}));  // too small
}

TEST_CASE("sampleChart reads every patch of a tilted chart") {
  auto img = bc::test::renderChart(640, 360, kTilted, bc::colorCheckerSrgb());
  checkMatchesReference(bc::sampleChart({img}, kTilted));
}

TEST_CASE("sampleChart turns an upside-down chart to the normal order") {
  auto reversed = bc::colorCheckerSrgb();
  std::reverse(reversed.begin(), reversed.end());  // the card held upside down
  auto img = bc::test::renderChart(640, 360, kTilted, reversed);
  checkMatchesReference(bc::sampleChart({img}, kTilted));
}

TEST_CASE("sampleChart refuses corners that are not on the chart") {
  auto img = bc::test::renderChart(640, 360, kTilted, bc::colorCheckerSrgb());
  bc::ChartQuad off{{{{0.02, 0.02}, {0.2, 0.02}, {0.2, 0.15}, {0.02, 0.15}}}};
  CHECK_THROWS_WITH_AS(bc::sampleChart({img}, off), doctest::Contains("not on the chart"), std::invalid_argument);
}
