#pragma once
#include <chrono>
#include <string>
#include <vector>

namespace bc {
// NDI sources visible on the network, e.g. "STUDIO-PC (vMix - Output 1)".
std::vector<std::string> listNdiSources(std::chrono::milliseconds wait = std::chrono::milliseconds(1500));
}  // namespace bc