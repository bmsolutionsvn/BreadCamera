#include "core/chart.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bc {
namespace {
double luma(const Rgb& c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
double chroma(const Rgb& c) {
  const double hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b});
  return (hi - lo) / (hi + 1e-6);
}
double rowChroma(const ChartColors& p, int row) {
  double s = 0;
  for (int i = 0; i < 6; ++i) s += chroma(p[static_cast<size_t>(row * 6 + i)]);
  return s;
}
}  // namespace

const std::array<std::array<uint8_t, 3>, 24>& colorCheckerSrgb() {
  static const std::array<std::array<uint8_t, 3>, 24> v{{
      {115, 82, 68},   {194, 150, 130}, {98, 122, 157},  {87, 108, 67},   {133, 128, 177}, {103, 189, 170},
      {214, 126, 44},  {80, 91, 166},   {193, 90, 99},   {94, 60, 108},   {157, 188, 64},  {224, 163, 46},
      {56, 61, 150},   {70, 148, 73},   {175, 54, 60},   {231, 199, 31},  {187, 86, 149},  {8, 133, 161},
      {243, 243, 242}, {200, 200, 200}, {160, 160, 160}, {122, 122, 121}, {85, 85, 85},    {52, 52, 52},
  }};
  return v;
}

const ChartColors& colorCheckerReference() {
  static const ChartColors lin = [] {
    ChartColors out;
    for (size_t i = 0; i < 24; ++i) {
      const auto& s = colorCheckerSrgb()[i];
      out[i] = {toLinearLight(s[0] / 255.0f), toLinearLight(s[1] / 255.0f), toLinearLight(s[2] / 255.0f)};
    }
    return out;
  }();
  return lin;
}

std::array<double, 2> Homography::map(double u, double v) const {
  const double w = m[6] * u + m[7] * v + m[8];
  return {(m[0] * u + m[1] * v + m[2]) / w, (m[3] * u + m[4] * v + m[5]) / w};
}

Homography Homography::inverse() const {
  const auto& a = m;
  const double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
  if (std::abs(det) < 1e-12) throw std::invalid_argument("the chart corners are degenerate");
  Homography r;
  r.m = {(a[4] * a[8] - a[5] * a[7]) / det, (a[2] * a[7] - a[1] * a[8]) / det, (a[1] * a[5] - a[2] * a[4]) / det,
         (a[5] * a[6] - a[3] * a[8]) / det, (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det,
         (a[3] * a[7] - a[4] * a[6]) / det, (a[1] * a[6] - a[0] * a[7]) / det, (a[0] * a[4] - a[1] * a[3]) / det};
  return r;
}

// Unit square → quad (Heckbert): (0,0)→c0, (1,0)→c1, (1,1)→c2, (0,1)→c3.
Homography chartToPicture(const ChartQuad& q) {
  const auto& [p0, p1, p2, p3] = q.c;
  const double dx1 = p1[0] - p2[0], dx2 = p3[0] - p2[0], dx3 = p0[0] - p1[0] + p2[0] - p3[0];
  const double dy1 = p1[1] - p2[1], dy2 = p3[1] - p2[1], dy3 = p0[1] - p1[1] + p2[1] - p3[1];
  const double den = dx1 * dy2 - dx2 * dy1;
  if (std::abs(den) < 1e-12) throw std::invalid_argument("the chart corners are degenerate");
  const double g = (dx3 * dy2 - dx2 * dy3) / den;
  const double h = (dx1 * dy3 - dx3 * dy1) / den;
  Homography r;
  r.m = {p1[0] - p0[0] + g * p1[0], p3[0] - p0[0] + h * p3[0], p0[0],
         p1[1] - p0[1] + g * p1[1], p3[1] - p0[1] + h * p3[1], p0[1],
         g, h, 1.0};
  return r;
}

bool validQuad(const ChartQuad& q) {
  double area = 0;
  int sign = 0;
  for (int i = 0; i < 4; ++i) {
    const auto& a = q.c[static_cast<size_t>(i)];
    const auto& b = q.c[static_cast<size_t>((i + 1) % 4)];
    const auto& c = q.c[static_cast<size_t>((i + 2) % 4)];
    if (a[0] < -0.05 || a[0] > 1.05 || a[1] < -0.05 || a[1] > 1.05) return false;
    area += a[0] * b[1] - b[0] * a[1];
    const double cross = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]);
    const int s = cross > 0 ? 1 : cross < 0 ? -1 : 0;
    if (s == 0 || (sign != 0 && s != sign)) return false;
    sign = s;
  }
  return std::abs(area) / 2 >= 0.002;
}

ChartColors sampleChartRaw(const std::vector<CpuImage>& frames, const ChartQuad& q) {
  if (frames.empty()) throw std::invalid_argument("no frames to sample");
  const auto h = chartToPicture(q);
  ChartColors out{};
  for (int p = 0; p < 24; ++p) {
    const double cu = (p % 6 + 0.5) / 6.0, cv = (p / 6 + 0.5) / 4.0;
    double r = 0, g = 0, b = 0;
    int n = 0;
    for (const auto& f : frames) {
      for (int sy = -3; sy <= 3; ++sy) {
        for (int sx = -3; sx <= 3; ++sx) {
          const auto xy = h.map(cu + sx * (0.2 / 6.0) / 3.0, cv + sy * (0.2 / 4.0) / 3.0);
          const int x = std::clamp(static_cast<int>(xy[0] * f.width), 0, static_cast<int>(f.width) - 1);
          const int y = std::clamp(static_cast<int>(xy[1] * f.height), 0, static_cast<int>(f.height) - 1);
          const uint8_t* px = &f.bgra[(static_cast<size_t>(y) * f.width + x) * 4];
          r += toLinearLight(px[2] / 255.0f);
          g += toLinearLight(px[1] / 255.0f);
          b += toLinearLight(px[0] / 255.0f);
          ++n;
        }
      }
    }
    out[static_cast<size_t>(p)] = {static_cast<float>(r / n), static_cast<float>(g / n), static_cast<float>(b / n)};
  }
  return out;
}

bool chartUpright(const ChartColors& c) {
  return rowChroma(c, 3) < rowChroma(c, 0) && rowChroma(c, 3) < rowChroma(c, 1) && luma(c[18]) >= 1.5 * luma(c[23]);
}

ChartColors sampleChart(const std::vector<CpuImage>& frames, const ChartQuad& q) {
  if (!validQuad(q)) throw std::invalid_argument("place the four corners on the chart");
  auto out = sampleChartRaw(frames, q);
  if (chartUpright(out)) return out;
  std::reverse(out.begin(), out.end());  // held upside down
  if (chartUpright(out)) return out;
  throw std::invalid_argument("the corners are not on the chart");
}
}  // namespace bc
