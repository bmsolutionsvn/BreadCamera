#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/upscale_size.h"
#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "gpu/upscaler.h"

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Upscaler up{*gpu, pool};
  // The centre pixel as {R, G, B}, through the same linear → BGRA8 path the tests read.
  std::array<int, 3> centerRgb(const bc::TextureRef& t) {
    auto img = conv.readback(*conv.toBgra8(*t, t->width, t->height));
    const size_t c = (static_cast<size_t>(img.height / 2) * img.width + img.width / 2) * 4;
    return {img.bgra[c + 2], img.bgra[c + 1], img.bgra[c]};
  }
};
}  // namespace

TEST_CASE("upscaleTarget: keeps aspect, rounds to even, never shrinks") {
  CHECK(bc::upscaleTarget(640, 360, 1080).w == 1920);
  CHECK(bc::upscaleTarget(640, 360, 1080).h == 1080);
  CHECK(bc::upscaleTarget(640, 480, 1080).w == 1440);  // 4:3
  CHECK(bc::upscaleTarget(854, 480, 1080).w == 1922);  // odd width, rounded up to even
  auto same = bc::upscaleTarget(1280, 720, 720);       // already at the target
  CHECK(same.w == 1280);
  CHECK(same.h == 720);
  auto big = bc::upscaleTarget(1920, 1080, 720);  // larger than the target: never shrinks
  CHECK(big.w == 1920);
  CHECK(big.h == 1080);
}

TEST_CASE("Upscaler: output is the asked size and a flat colour stays flat") {
  Rig r;
  auto in = r.conv.solid(640, 360, {0.2f, 0.4f, 0.1f, 1});
  auto expected = r.centerRgb(in);
  auto out = r.up.apply(*in, 1920, 1080);
  CHECK(out->width == 1920);
  CHECK(out->height == 1080);
  auto got = r.centerRgb(out);
  for (int i = 0; i < 3; ++i) CHECK(std::abs(got[i] - expected[i]) <= 2);
}

TEST_CASE("Upscaler: a hard edge stays sharp and does not ring") {
  Rig r;
  std::vector<uint8_t> px(static_cast<size_t>(64) * 36 * 4);
  for (uint32_t y = 0; y < 36; ++y) {
    for (uint32_t x = 0; x < 64; ++x) {
      const uint8_t v = x < 32 ? 0 : 255;
      uint8_t* p = &px[(static_cast<size_t>(y) * 64 + x) * 4];
      p[0] = p[1] = p[2] = v;
      p[3] = 255;
    }
  }
  auto up = r.conv.uploadBgra8(px.data(), 64, 36, 64 * 4);
  auto lin = r.conv.toLinear(*up, bc::PixelLayout::BGRA8, true);
  auto out = r.up.apply(*lin, 192, 108);
  CHECK(out->width == 192);
  CHECK(out->height == 108);
  auto img = r.conv.readback(*r.conv.toBgra8(*out, 192, 108));
  const uint32_t row = 54;  // the middle row
  std::vector<int> v(192);
  for (uint32_t x = 0; x < 192; ++x) v[x] = img.bgra[(static_cast<size_t>(row) * 192 + x) * 4 + 1];
  // Non-decreasing along the row (a little noise allowed): no ringing on either side of the edge.
  for (uint32_t x = 1; x < 192; ++x) CHECK(v[x] + 2 >= v[x - 1]);
  // Sharp: from the last pixel below 10 to the first above 245 within 6 px.
  int lastLow = -1, firstHigh = -1;
  for (uint32_t x = 0; x < 192; ++x) {
    if (v[x] < 10) lastLow = static_cast<int>(x);
    if (firstHigh < 0 && v[x] > 245) firstHigh = static_cast<int>(x);
  }
  REQUIRE(lastLow >= 0);
  REQUIRE(firstHigh >= 0);
  CHECK(lastLow < firstHigh);
  CHECK(firstHigh - lastLow <= 6);
}