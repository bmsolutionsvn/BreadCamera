#pragma once
#include <cstdint>
#include <vector>

namespace bc {
// SDI cards speak 8-bit 4:2:2 UYVY. BreadCamera uploads UYVY as it is now, so only BreadReplay's
// Renderer calls these; kept in bc_io for it. Limited range, BT.709 at 720 lines and above.
std::vector<uint8_t> uyvyToBgra(const uint8_t* uyvy, int width, int height, int rowBytes, bool bt709);
void bgraToUyvy(const uint8_t* bgra, int width, int height, uint8_t* out, int outRowBytes, bool bt709);
}  // namespace bc