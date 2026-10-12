#pragma once
#include <chrono>
#include <optional>
#include <string>

#include "core/signal_state.h"

namespace bc {
// Per-camera, per-stage health (spec §3.6): bypass on failure or sustained overrun,
// retry after a pause. Used only from the tick thread.
class StageGuard {
 public:
  using Clock = std::chrono::steady_clock;
  struct Policy {
    int overrunTicks = 30;
    std::chrono::milliseconds retryAfter{5000};
  };
  StageGuard() = default;
  explicit StageGuard(Policy p) : policy_(p) {}

  bool shouldRun(Clock::time_point now) const { return !bypassed_ || now >= retryAt_; }
  void succeeded(std::chrono::nanoseconds took, std::chrono::nanoseconds budget, Clock::time_point now);
  void failed(std::string message, Clock::time_point now);
  void failed(ErrorInfo e, Clock::time_point now);
  bool bypassed() const { return bypassed_; }
  const std::optional<ErrorInfo>& error() const { return error_; }
  std::string stateName(bool enabled) const { return !enabled ? "off" : bypassed_ ? "bypassed" : "active"; }

 private:
  void bypass(ErrorInfo e, Clock::time_point now);
  Policy policy_;
  bool bypassed_ = false;
  int overruns_ = 0;
  Clock::time_point retryAt_{};
  std::optional<ErrorInfo> error_;
};
}  // namespace bc
