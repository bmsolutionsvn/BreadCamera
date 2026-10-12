#include <doctest/doctest.h>

#include <algorithm>

#include "core/chart.h"
#include "engine/chart_detector.h"
#include "support/chart_image.h"
#include "support/requires.h"

TEST_CASE("findChart finds a tilted chart, upright and upside down" BC_REQUIRES(bc::test::Req::ChartDetect)) {
  const bc::ChartQuad q{{{{0.30, 0.25}, {0.72, 0.22}, {0.74, 0.70}, {0.28, 0.72}}}};
  for (bool flipped : {false, true}) {
    auto colors = bc::colorCheckerSrgb();
    if (flipped) std::reverse(colors.begin(), colors.end());
    auto img = bc::test::renderChart(960, 540, q, colors);
    auto found = bc::findChart(img);
    REQUIRE(found);
    auto got = bc::sampleChart({img}, *found);
    for (size_t i = 0; i < 24; ++i) CHECK(std::abs(bc::toGamma(got[i].g) * 255 - bc::colorCheckerSrgb()[i][1]) <= 4);
  }
}

TEST_CASE("findChart returns nothing for a picture without a chart") {
  bc::CpuImage grey{960, 540, std::vector<uint8_t>(960 * 540 * 4, 90)};
  CHECK_FALSE(bc::findChart(grey));
}
