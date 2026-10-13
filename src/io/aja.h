#pragma once
#include <string>
#include <vector>

namespace bc {
struct AjaInfo {
  int index = 0;
  std::string name;
};
std::vector<AjaInfo> listAjaDevices();
}  // namespace bc