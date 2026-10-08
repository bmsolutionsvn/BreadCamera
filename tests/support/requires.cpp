#include "support/requires.h"

#include <cstdio>
#include <mutex>
#include <set>

namespace bc::test {
namespace {
bool check(Req r, const char*& reason) {
  switch (r) {
    case Req::Ndi:
      reason = "NDI SDK not built (BC_WITH_NDI=OFF)";
      return BC_WITH_NDI == 0;
    case Req::HardwareGpu:
      reason = "no hardware GPU check yet (Task 5)";
      return true;
    case Req::H264Encoder:
      reason = "no H.264 encoder check yet (Task 10)";
      return true;
  }
  return true;
}
}  // namespace

bool missing(Req r) {
  static std::mutex m;
  static std::set<int> printed;
  const char* reason = "";
  bool miss = check(r, reason);
  std::lock_guard lock(m);
  if (miss && printed.insert(static_cast<int>(r)).second) {
    std::fprintf(stderr, "[skip] requirement %d missing: %s\n", static_cast<int>(r), reason);
  }
  return miss;
}
}  // namespace bc::test
