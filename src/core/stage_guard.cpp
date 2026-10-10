#include "core/stage_guard.h"

namespace bc {
namespace {
std::string ms(std::chrono::nanoseconds d) {
  return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(d).count() / 1000.0).substr(0, 5);
}
}  // namespace

void StageGuard::bypass(ErrorInfo e, Clock::time_point now) {
  bypassed_ = true;
  error_ = std::move(e);
  retryAt_ = now + policy_.retryAfter;
  overruns_ = 0;
}

void StageGuard::succeeded(std::chrono::nanoseconds took, std::chrono::nanoseconds budget, Clock::time_point now) {
  if (took > budget) {
    // A trial run (we were bypassed) gets no second chance; otherwise count the streak.
    if (bypassed_ || ++overruns_ >= policy_.overrunTicks) {
      bypass({"stage-overrun", "took " + ms(took) + " ms per tick; the budget is " + ms(budget) + " ms"}, now);
    }
    return;
  }
  overruns_ = 0;
  bypassed_ = false;
  error_.reset();
}

void StageGuard::failed(std::string message, Clock::time_point now) { bypass({"stage-failed", std::move(message)}, now); }
}  // namespace bc
