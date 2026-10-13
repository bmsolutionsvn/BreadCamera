#include "io/aja.h"

#include "ntv2card.h"
#include "ntv2devicescanner.h"

namespace bc {
std::vector<AjaInfo> listAjaDevices() {
  std::vector<AjaInfo> out;
  for (ULWord i = 0; i < 8; ++i) {
    CNTV2Card card;
    if (!CNTV2DeviceScanner::GetDeviceAtIndex(i, card)) break;
    out.push_back({static_cast<int>(i), card.GetDisplayName()});
  }
  return out;
}
}  // namespace bc