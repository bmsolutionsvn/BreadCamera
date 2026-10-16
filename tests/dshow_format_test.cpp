#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "io/dshow_format.h"

// MEDIASUBTYPE_* live in <uuids.h> via <dshow.h>; the pure converter takes a GUID, so the test
// declares the few it needs and stays free of DirectShow.
static const GUID kRgb32 = {0xe436eb7d, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
static const GUID kRgb24 = {0xe436eb7e, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
static const GUID kNv12 = {0x3231564e, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kYuy2 = {0x32595559, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kMjpg = {0x47504a4d, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

TEST_CASE("dshowToBgra8: RGB32 is a BGRX pass-through") {
  const uint8_t src[16] = {1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0, 10, 11, 12, 0};  // 2x2, top-down
  auto out = bc::dshowToBgra8(kRgb32, src, 2, 2, 8, false, false);
  CHECK(out.size() == 16u);
  CHECK(out[0] == 1);
  CHECK(out[1] == 2);
  CHECK(out[2] == 3);
  CHECK(out[12] == 10);
  CHECK(out[13] == 11);
  CHECK(out[14] == 12);
}

TEST_CASE("dshowToBgra8: RGB32 bottom-up is flipped upright") {
  const uint8_t src[16] = {1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0, 10, 11, 12, 0};
  auto out = bc::dshowToBgra8(kRgb32, src, 2, 2, 8, true, false);
  CHECK(out[0] == 7);  // the source's last row comes first
  CHECK(out[1] == 8);
  CHECK(out[2] == 9);
}

TEST_CASE("dshowToBgra8: RGB24 expands to BGRA") {
  const uint8_t src[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};  // 2x2
  auto out = bc::dshowToBgra8(kRgb24, src, 2, 2, 6, false, false);
  CHECK(out.size() == 16u);
  CHECK(out[0] == 1);
  CHECK(out[1] == 2);
  CHECK(out[2] == 3);
  CHECK(out[3] == 255);
}

TEST_CASE("dshowToBgra8: limited-range NV12 black and white") {
  // A realistically sized frame with tail padding: swscale reads past the nominal end of a tiny
  // image, so a 2x2 buffer would fault.
  const int w = 16, h = 16;
  auto nv12 = [&](uint8_t y) {
    std::vector<uint8_t> v(static_cast<size_t>(w) * h * 3 / 2 + 64, 0);
    for (int i = 0; i < w * h; ++i) v[i] = y;
    for (int i = w * h; i < w * h * 3 / 2; ++i) v[i] = 128;  // neutral chroma
    return v;
  };
  auto black = nv12(16);  // limited-range black
  auto b = bc::dshowToBgra8(kNv12, black.data(), w, h, w, false, true);
  CHECK(b[0] <= 2);
  CHECK(b[1] <= 2);
  CHECK(b[2] <= 2);
  auto white = nv12(235);  // limited-range white
  auto wout = bc::dshowToBgra8(kNv12, white.data(), w, h, w, false, true);
  CHECK(wout[0] >= 253);
  CHECK(wout[1] >= 253);
  CHECK(wout[2] >= 253);
}

TEST_CASE("dshowToBgra8: YUY2 neutral maps to grey") {
  const int w = 16, h = 16;
  std::vector<uint8_t> src(static_cast<size_t>(w) * h * 2 + 64, 128);  // all bytes 128
  auto out = bc::dshowToBgra8(kYuy2, src.data(), w, h, w * 2, false, true);
  CHECK(out[0] >= 124);
  CHECK(out[0] <= 132);
}

TEST_CASE("dshowRowBytes: per-subtype row size (NV12 is the luma stride, not 1.5x)") {
  CHECK(bc::dshowRowBytes(kRgb32, 1920) == 1920 * 4);
  CHECK(bc::dshowRowBytes(kRgb24, 1920) == 1920 * 3);
  CHECK(bc::dshowRowBytes(kNv12, 1920) == 1920);
  CHECK(bc::dshowRowBytes(kYuy2, 1920) == 1920 * 2);
  CHECK(bc::dshowRowBytes(kMjpg, 1920) == 0);
}

TEST_CASE("dshowFrameBytes: NV12 includes its interleaved chroma plane") {
  CHECK(bc::dshowFrameBytes(kRgb32, 1920, 1080, 1920 * 4) == static_cast<size_t>(1920) * 1080 * 4);
  CHECK(bc::dshowFrameBytes(kNv12, 1920, 1080, 1920) == static_cast<size_t>(1920) * 1080 * 3 / 2);
  CHECK(bc::dshowFrameBytes(kYuy2, 1920, 1080, 1920 * 2) == static_cast<size_t>(1920) * 1080 * 2);
  CHECK(bc::dshowFrameBytes(kMjpg, 1920, 1080, 1920) == 0);
}

TEST_CASE("dshowSubtypeSupported: recognises the raw set, rejects MJPG") {
  CHECK(bc::dshowSubtypeSupported(kRgb32));
  CHECK(bc::dshowSubtypeSupported(kNv12));
  CHECK_FALSE(bc::dshowSubtypeSupported(kMjpg));
}
