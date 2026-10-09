#pragma once
#include <string>
#include <vector>

namespace bc {
struct MonitorInfo {
  int index = 0;
  std::string name;  // e.g. "\\\\.\\DISPLAY2"
  int x = 0, y = 0, width = 0, height = 0;
  bool primary = false;
};
// Primary first, then left to right, top to bottom. Index = position in this list.
std::vector<MonitorInfo> listMonitors();
}  // namespace bc
