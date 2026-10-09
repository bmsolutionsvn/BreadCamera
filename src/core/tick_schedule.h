#pragma once
#include <chrono>
#include <cstdint>

#include "core/rate.h"

namespace bc {
// Exact tick deadlines for the master clock (spec §3.3). Offsets are computed from the
// tick index, so rounding never accumulates over a long show.
class TickSchedule {
 public:
  explicit TickSchedule(Rate r) : rate_(r) {}
  Rate rate() const { return rate_; }
  std::chrono::nanoseconds offset(uint64_t index) const;
  // First index whose offset is >= elapsed. After an overrun the tick loop jumps here
  // instead of running the missed ticks back to back.
  uint64_t nextIndexAtOrAfter(std::chrono::nanoseconds elapsed) const;
  // Audio sample frames that belong to tick `index`, so the total per second is exact
  // (800.8 per tick at 59.94).
  uint32_t audioFramesForTick(uint64_t index, int sampleRate) const;

 private:
  Rate rate_;
};
}  // namespace bc
