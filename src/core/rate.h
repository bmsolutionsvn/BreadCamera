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

// Accepts "50/1", "60000/1001" or "60/1" only (spec §1.3).
std::optional<Rate> parseShowRate(std::string_view s);
}  // namespace bc
