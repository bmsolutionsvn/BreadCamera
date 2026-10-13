#include <doctest/doctest.h>

#include "io/uyvy.h"

TEST_CASE("UYVY <-> BGRA round trip keeps colours (BT.709 limited)") {
  const int w = 16, h = 2;
  std::vector<uint8_t> bgra(w * h * 4);
  for (int i = 0; i < w * h; ++i) {
    bgra[i * 4] = 40;
    bgra[i * 4 + 1] = 120;
    bgra[i * 4 + 2] = 200;
    bgra[i * 4 + 3] = 255;
  }
  std::vector<uint8_t> uyvy(w * 2 * h);
  bc::bgraToUyvy(bgra.data(), w, h, uyvy.data(), w * 2, true);
  auto back = bc::uyvyToBgra(uyvy.data(), w, h, w * 2, true);
  for (int i = 0; i < w * h; ++i) {
    CHECK(std::abs(back[i * 4] - 40) <= 3);
    CHECK(std::abs(back[i * 4 + 1] - 120) <= 3);
    CHECK(std::abs(back[i * 4 + 2] - 200) <= 3);
  }
}