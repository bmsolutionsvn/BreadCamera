#include "core/tick_schedule.h"

#include <cmath>

namespace bc {
std::chrono::nanoseconds TickSchedule::offset(uint64_t index) const {
  // index * den / num seconds, split to avoid 64-bit overflow.
  const uint64_t num = static_cast<uint64_t>(rate_.num);
  const uint64_t scaled = index * static_cast<uint64_t>(rate_.den);
  const uint64_t secs = scaled / num;
  const uint64_t rem = scaled % num;
  return std::chrono::nanoseconds(secs * 1'000'000'000ULL + rem * 1'000'000'000ULL / num);
}

uint64_t TickSchedule::nextIndexAtOrAfter(std::chrono::nanoseconds elapsed) const {
  if (elapsed.count() <= 0) return 0;
  long double approx = static_cast<long double>(elapsed.count()) * rate_.num /
                       (static_cast<long double>(rate_.den) * 1e9L);
  uint64_t idx = static_cast<uint64_t>(std::ceil(approx));
  while (offset(idx) < elapsed) ++idx;
  while (idx > 0 && offset(idx - 1) >= elapsed) --idx;
  return idx;
}

uint32_t TickSchedule::audioFramesForTick(uint64_t index, int sampleRate) const {
  auto total = [&](uint64_t i) {
    return i * static_cast<uint64_t>(sampleRate) * static_cast<uint64_t>(rate_.den) /
           static_cast<uint64_t>(rate_.num);
  };
  return static_cast<uint32_t>(total(index + 1) - total(index));
}
}  // namespace bc
