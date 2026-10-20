#pragma once
#include <chrono>
#include <cstddef>
#include <optional>

#include "core/rate.h"

namespace bc {
// Derives a stable nominal input rate from frame arrival times. Needs 15 intervals and returns a
// rate only when the coefficient of variation is under 5 %. Snaps the NTSC family (23.976, 29.97,
// 59.94, 119.88) and whole rates; otherwise uses a 1/1000 rational.
class RateEstimator {
 public:
  void reset();
  void push(std::chrono::steady_clock::time_point arrival);
  std::optional<Rate> rate() const;

 private:
  static constexpr std::size_t kSamples = 15;
  std::optional<std::chrono::steady_clock::time_point> last_;
  double meanNs_ = 0;
  double m2Ns_ = 0;
  std::size_t n_ = 0;
};
}  // namespace bc
