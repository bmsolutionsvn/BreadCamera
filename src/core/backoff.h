#pragma once
#include <algorithm>
#include <chrono>

namespace bc {
// Reconnect delay: initial, doubling, capped (spec §3.6).
class Backoff {
 public:
  explicit Backoff(std::chrono::milliseconds initial = std::chrono::milliseconds(500),
                   std::chrono::milliseconds max = std::chrono::milliseconds(5000))
      : initial_(initial), max_(max), next_(initial) {}
  std::chrono::milliseconds next() {
    auto d = next_;
    next_ = std::min(next_ * 2, max_);
    return d;
  }
  void reset() { next_ = initial_; }

 private:
  std::chrono::milliseconds initial_, max_, next_;
};
}  // namespace bc
