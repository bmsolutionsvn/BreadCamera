#pragma once
#include <string_view>

#include "core/chart.h"
#include "core/lut.h"

namespace bc {
struct Calibration {
  Lut3D lut;
  double deltaEBefore = 0;
  double deltaEAfter = 0;
};

// measured → target (spec §5.3): tone curves from the neutrals, a 3x3 matrix by least squares,
// baked into a lutSize³ LUT. Throws std::invalid_argument when the chart is too dark or clipped.
Calibration solveCalibration(const ChartColors& measured, const ChartColors& target, int lutSize = 33);
double meanDeltaE(const ChartColors& a, const ChartColors& b);
std::string_view calibrationRating(double meanDeltaE);
}  // namespace bc
