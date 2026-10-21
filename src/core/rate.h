#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bc {
struct Rate {
  int num = 60;
  int den = 1;
  double fps() const { return static_cast<double>(num) / den; }
  // One tick, truncated to whole nanoseconds. Use TickSchedule for exact offsets.
  std::chrono::nanoseconds period() const {
    return std::chrono::nanoseconds(1'000'000'000LL * den / num);
  }
  std::string str() const { return std::to_string(num) + "/" + std::to_string(den); }
  bool operator==(const Rate&) const = default;
};

// Accepts only 15, 23.976, 24, 25, 29.97, 30, 50, 59.94, 60, 119.88 or 120 fps, as exact
// rationals (`15/1`, `24000/1001`, ...). HFR is limited to 119.88 and 120.
std::optional<Rate> parseShowRate(std::string_view s);
// A source's declared rational; nullopt unless both parts are positive.
std::optional<Rate> rateFromRational(int num, int den);
}  // namespace bc
