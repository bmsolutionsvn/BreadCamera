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

// The swscale context is cached per direction now: a second call at another size must resize it
// (a context left at the old size would corrupt the frame), so each size is compared against
// another run at that size in between.
TEST_CASE("UYVY: calls at two sizes in a row stay independent") {
  const auto encode = [](int w, int h) {
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        uint8_t* p = &bgra[(static_cast<size_t>(y) * w + x) * 4];
        p[0] = static_cast<uint8_t>(x * 3);
        p[1] = static_cast<uint8_t>(y * 7);
        p[2] = static_cast<uint8_t>(x * 5 + y);
        p[3] = 255;
      }
    std::vector<uint8_t> uyvy(static_cast<size_t>(w) * h * 2);
    bc::bgraToUyvy(bgra.data(), w, h, uyvy.data(), w * 2, false);
    return uyvy;
  };
  const auto small = encode(16, 2);
  const auto big = encode(64, 8);
  CHECK(encode(16, 2) == small);
  CHECK(encode(64, 8) == big);

  const auto decode = [](int w, int h) {
    std::vector<uint8_t> uyvy(static_cast<size_t>(w) * h * 2);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        uint8_t* p = &uyvy[(static_cast<size_t>(y) * w + x) * 2];
        p[0] = static_cast<uint8_t>(x * 9);
        p[1] = static_cast<uint8_t>(y * 11 + x);
      }
    return bc::uyvyToBgra(uyvy.data(), w, h, w * 2, false);
  };
  const auto smallBack = decode(16, 2);
  const auto bigBack = decode(64, 8);
  CHECK(decode(16, 2) == smallBack);
  CHECK(decode(64, 8) == bigBack);
}