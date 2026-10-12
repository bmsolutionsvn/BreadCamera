#include "core/calibration.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bc {
namespace {
using Mat3 = std::array<std::array<double, 3>, 3>;

double luma(const Rgb& c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
float ch(const Rgb& c, int i) { return i == 0 ? c.r : i == 1 ? c.g : c.b; }

// One tone curve: sorted, non-decreasing points in the gamma domain.
struct Curve {
  std::vector<std::pair<double, double>> pts;
  double operator()(double x) const {
    if (x <= pts.front().first) return pts.front().second;
    for (size_t i = 1; i < pts.size(); ++i) {
      if (x <= pts[i].first) {
        const auto [x0, y0] = pts[i - 1];
        const auto [x1, y1] = pts[i];
        return x1 - x0 < 1e-9 ? y1 : y0 + (y1 - y0) * (x - x0) / (x1 - x0);
      }
    }
    return pts.back().second;
  }
};

Curve neutralCurve(const ChartColors& measured, const ChartColors& target, int c) {
  std::vector<std::pair<double, double>> p{{0.0, 0.0}, {1.0, 1.0}};
  for (size_t i = 18; i < 24; ++i) p.push_back({toGamma(ch(measured[i], c)), toGamma(ch(target[i], c))});
  std::sort(p.begin(), p.end());
  Curve out;
  for (auto& [x, y] : p) {
    if (!out.pts.empty() && std::abs(out.pts.back().first - x) < 1e-6) {
      out.pts.back().second = (out.pts.back().second + y) / 2;
      continue;
    }
    out.pts.push_back({x, y});
  }
  for (size_t i = 1; i < out.pts.size(); ++i) out.pts[i].second = std::max(out.pts[i].second, out.pts[i - 1].second);
  for (auto& [x, y] : out.pts) y = std::clamp(y, x - 0.25, x + 0.25);
  return out;
}

Rgb curved(const std::array<Curve, 3>& curves, Rgb lin) {
  return {toLinearLight(static_cast<float>(curves[0](toGamma(lin.r)))), toLinearLight(static_cast<float>(curves[1](toGamma(lin.g)))),
          toLinearLight(static_cast<float>(curves[2](toGamma(lin.b))))};
}

double det3(const Mat3& a) {
  return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
         a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
}

// Solves A x = b for 3x3 A (Cramer).
std::array<double, 3> solve3(const Mat3& a, const std::array<double, 3>& b) {
  const double d = det3(a);
  std::array<double, 3> x{};
  for (int k = 0; k < 3; ++k) {
    Mat3 m = a;
    for (int r = 0; r < 3; ++r) m[static_cast<size_t>(r)][static_cast<size_t>(k)] = b[static_cast<size_t>(r)];
    x[static_cast<size_t>(k)] = det3(m) / d;
  }
  return x;
}

// Least squares x_i·M ≈ t_i, pulled towards identity: (XᵀX + λI) M_k = Xᵀt_k + λ e_k.
Mat3 fitMatrix(const std::vector<Rgb>& x, const ChartColors& t) {
  constexpr double kLambda = 0.24;
  Mat3 xtx{};
  for (auto& v : x)
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) xtx[static_cast<size_t>(r)][static_cast<size_t>(c)] += ch(v, r) * ch(v, c);
  for (int i = 0; i < 3; ++i) xtx[static_cast<size_t>(i)][static_cast<size_t>(i)] += kLambda;
  Mat3 m{};
  for (int k = 0; k < 3; ++k) {
    std::array<double, 3> xty{};
    for (size_t i = 0; i < x.size(); ++i)
      for (int r = 0; r < 3; ++r) xty[static_cast<size_t>(r)] += ch(x[i], r) * ch(t[i], k);
    xty[static_cast<size_t>(k)] += kLambda;
    m[static_cast<size_t>(k)] = solve3(xtx, xty);  // row k: output channel k from (r, g, b)
  }
  if (det3(m) < 0.05) m = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  return m;
}

Rgb apply(const Mat3& m, Rgb v) {
  auto row = [&](int k) {
    const auto& r = m[static_cast<size_t>(k)];
    return static_cast<float>(std::clamp(r[0] * v.r + r[1] * v.g + r[2] * v.b, 0.0, 1.0));
  };
  return {row(0), row(1), row(2)};
}
}  // namespace

double meanDeltaE(const ChartColors& a, const ChartColors& b) {
  double s = 0;
  for (size_t i = 0; i < 24; ++i) s += deltaE2000(linearToLab(a[i]), linearToLab(b[i]));
  return s / 24;
}

std::string_view calibrationRating(double d) { return d <= 2 ? "good" : d <= 4 ? "fair" : "poor"; }

Calibration solveCalibration(const ChartColors& measured, const ChartColors& target, int lutSize) {
  double neutral = 0;
  for (size_t i = 18; i < 24; ++i) neutral += luma(measured[i]);
  if (neutral / 6 < 0.01) throw std::invalid_argument("the chart is too dark — add light on it");
  if (luma(measured[18]) > 0.95) throw std::invalid_argument("the white patch is clipped — lower the exposure or the light on the chart");

  const std::array<Curve, 3> curves{neutralCurve(measured, target, 0), neutralCurve(measured, target, 1), neutralCurve(measured, target, 2)};
  std::vector<Rgb> x;
  for (auto& m : measured) x.push_back(curved(curves, m));
  const Mat3 mat = fitMatrix(x, target);

  Calibration out;
  out.lut.size = lutSize;
  out.lut.data.resize(static_cast<size_t>(lutSize) * lutSize * lutSize);
  const float n1 = static_cast<float>(lutSize - 1);
  for (int b = 0; b < lutSize; ++b)
    for (int g = 0; g < lutSize; ++g)
      for (int r = 0; r < lutSize; ++r) {
        const Rgb lin{toLinearLight(r / n1), toLinearLight(g / n1), toLinearLight(b / n1)};
        const Rgb v = apply(mat, curved(curves, lin));
        out.lut.data[static_cast<size_t>((b * lutSize + g) * lutSize + r)] = {toGamma(v.r), toGamma(v.g), toGamma(v.b)};
      }

  ChartColors corrected;
  for (size_t i = 0; i < 24; ++i) {
    const auto gOut = out.lut.apply({toGamma(measured[i].r), toGamma(measured[i].g), toGamma(measured[i].b)});
    corrected[i] = {toLinearLight(gOut.r), toLinearLight(gOut.g), toLinearLight(gOut.b)};
  }
  out.deltaEBefore = meanDeltaE(measured, target);
  out.deltaEAfter = meanDeltaE(corrected, target);
  return out;
}
}  // namespace bc
