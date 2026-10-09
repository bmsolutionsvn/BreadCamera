#include "io/monitors.h"

#include <windows.h>

#include <algorithm>

#include "core/utf8.h"

namespace bc {
std::vector<MonitorInfo> listMonitors() {
  std::vector<MonitorInfo> out;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
          MonitorInfo m;
          m.name = toUtf8(mi.szDevice);
          m.x = mi.rcMonitor.left;
          m.y = mi.rcMonitor.top;
          m.width = mi.rcMonitor.right - mi.rcMonitor.left;
          m.height = mi.rcMonitor.bottom - mi.rcMonitor.top;
          m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
          reinterpret_cast<std::vector<MonitorInfo>*>(lp)->push_back(m);
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&out));
  std::stable_sort(out.begin(), out.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
    if (a.primary != b.primary) return a.primary;
    if (a.x != b.x) return a.x < b.x;
    return a.y < b.y;
  });
  for (size_t i = 0; i < out.size(); ++i) out[i].index = static_cast<int>(i);
  return out;
}
}  // namespace bc
