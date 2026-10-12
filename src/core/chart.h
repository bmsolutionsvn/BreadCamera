#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "core/color_math.h"
#include "gpu/converter.h"  // CpuImage

namespace bc {
// Outer corners of the 24-patch grid, normalised to the picture (0..1), in chart order:
// top-left (dark skin), top-right, bottom-right, bottom-left.
struct ChartQuad {
  std::array<std::array<double, 2>, 4> c;
};
using ChartColors = std::array<Rgb, 24>;  // linear light, row by row, 6 per row

// ColorChecker Classic 24, published sRGB (D65) values (spec §5.3).
const std::array<std::array<uint8_t, 3>, 24>& colorCheckerSrgb();
const ChartColors& colorCheckerReference();  // the same, in linear light

// Chart space (u across the 6 columns, v down the 4 rows, both 0..1) to the picture.
struct Homography {
  std::array<double, 9> m;  // a b c / d e f / g h 1
  std::array<double, 2> map(double u, double v) const;
  Homography inverse() const;
};
Homography chartToPicture(const ChartQuad& q);  // throws std::invalid_argument for a degenerate quad
bool validQuad(const ChartQuad& q);              // convex, inside the picture, at least 0.2 % of it

ChartColors sampleChartRaw(const std::vector<CpuImage>& frames, const ChartQuad& q);
bool chartUpright(const ChartColors& c);
ChartColors sampleChart(const std::vector<CpuImage>& frames, const ChartQuad& q);
}  // namespace bc
