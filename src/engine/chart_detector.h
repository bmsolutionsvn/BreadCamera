#pragma once
#include <optional>

#include "core/chart.h"
#include "gpu/converter.h"

namespace bc {
bool chartDetectionAvailable();
// Finds a ColorChecker 24 in the frame; corners come back in chart order (spec §5.3).
std::optional<ChartQuad> findChart(const CpuImage& frame);
}  // namespace bc
