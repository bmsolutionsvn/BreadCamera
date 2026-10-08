#include "core/rate.h"

#include <charconv>

namespace bc {
std::optional<Rate> parseShowRate(std::string_view s) {
  auto slash = s.find('/');
  if (slash == std::string_view::npos) return std::nullopt;
  int num = 0, den = 0;
  auto a = s.substr(0, slash), b = s.substr(slash + 1);
  auto ra = std::from_chars(a.data(), a.data() + a.size(), num);
  auto rb = std::from_chars(b.data(), b.data() + b.size(), den);
  // The whole substring must be consumed, so "60x/1" is rejected.
  if (ra.ec != std::errc{} || ra.ptr != a.data() + a.size() ||
      rb.ec != std::errc{} || rb.ptr != b.data() + b.size()) {
    return std::nullopt;
  }
  Rate r{num, den};
  if (r == Rate{50, 1} || r == Rate{60000, 1001} || r == Rate{60, 1}) return r;
  return std::nullopt;
}
}  // namespace bc
