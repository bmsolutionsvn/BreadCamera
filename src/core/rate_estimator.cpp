#include "core/rate_estimator.h"

#include <cmath>
#include <numeric>

namespace bc {
namespace {
std::optional<Rate> snap(double fps) {
  struct Known {
    double fps;
    Rate rate;
  };
  static const Known kNtsc[] = {
      {24000.0 / 1001, {24000, 1001}},
      {30000.0 / 1001, {30000, 1001}},
      {60000.0 / 1001, {60000, 1001}},
      {120000.0 / 1001, {120000, 1001}},
  };
  for (const auto& k : kNtsc) {
    if (std::abs(fps - k.fps) <= k.fps * 0.0005) return k.rate;
  }
  const double whole = std::round(fps);
  if (whole > 0 && std::abs(fps - whole) <= whole * 0.005) return Rate{static_cast<int>(whole), 1};
  if (fps <= 0) return std::nullopt;
  int num = static_cast<int>(std::lround(fps * 1000));
  int den = 1000;
  const int g = std::gcd(num, den);
  return Rate{num / g, den / g};
}
}  // namespace

void RateEstimator::reset() {
  last_.reset();
  meanNs_ = 0;
  m2Ns_ = 0;
  n_ = 0;
}

void RateEstimator::push(std::chrono::steady_clock::time_point arrival) {
  if (last_) {
    const double dt = std::chrono::duration<double, std::nano>(arrival - *last_).count();
    if (dt <= 0) return;
    if (dt > 1e9) {  // a gap longer than a second is a new session, not a 1 fps input
      reset();
      last_ = arrival;
      return;
    }
    ++n_;
    const double delta = dt - meanNs_;
    meanNs_ += delta / static_cast<double>(n_);
    m2Ns_ += delta * (dt - meanNs_);
  }
  last_ = arrival;
}

std::optional<Rate> RateEstimator::rate() const {
  if (n_ < kSamples || meanNs_ <= 0) return std::nullopt;
  const double sd = std::sqrt(m2Ns_ / static_cast<double>(n_));
  if (sd / meanNs_ >= 0.05) return std::nullopt;
  return snap(1e9 / meanNs_);
}
}  // namespace bc
