#include "engine/chart_detector.h"

#include <array>

#if BC_WITH_CHART_DETECT
#include <opencv2/imgproc.hpp>
#include <opencv2/mcc.hpp>
#endif

namespace bc {
bool chartDetectionAvailable() { return BC_WITH_CHART_DETECT != 0; }

std::optional<ChartQuad> findChart(const CpuImage& frame) {
#if BC_WITH_CHART_DETECT
  cv::Mat bgra(static_cast<int>(frame.height), static_cast<int>(frame.width), CV_8UC4, const_cast<uint8_t*>(frame.bgra.data()));
  cv::Mat bgr;
  cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
  auto detector = cv::mcc::CCheckerDetector::create();
  if (!detector->process(bgr, cv::mcc::MCC24, 1)) return std::nullopt;
  auto checker = detector->getBestColorChecker();
  if (!checker) return std::nullopt;
  const auto pts = checker->getColorCharts();  // 4 points per patch, patch order
  if (pts.size() != 96) return std::nullopt;
  auto centre = [&](int k) {
    double x = 0, y = 0;
    for (int i = 0; i < 4; ++i) {
      x += pts[static_cast<size_t>(k * 4 + i)].x;
      y += pts[static_cast<size_t>(k * 4 + i)].y;
    }
    return std::array<double, 2>{x / 4 / frame.width, y / 4 / frame.height};
  };
  // Corner patch centres → the outer corners of the 6x4 grid.
  const auto inner = chartToPicture(ChartQuad{{centre(0), centre(5), centre(23), centre(18)}});
  ChartQuad q{{inner.map(-0.1, -1.0 / 6), inner.map(1.1, -1.0 / 6), inner.map(1.1, 7.0 / 6), inner.map(-0.1, 7.0 / 6)}};
  for (int turn = 0; turn < 2; ++turn) {
    if (validQuad(q) && chartUpright(sampleChartRaw({frame}, q))) return q;
    q.c = {q.c[2], q.c[3], q.c[0], q.c[1]};  // mcc numbered the patches from the other end
  }
  return std::nullopt;
#else
  (void)frame;
  return std::nullopt;
#endif
}
}  // namespace bc
