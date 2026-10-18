#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/rtx_video.h"
#include "gpu/texture_pool.h"
#include "support/requires.h"

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
};

// The centre pixel as {R, G, B}, through the same linear → BGRA8 path the other GPU tests read.
std::array<int, 3> centreRgb(bc::Converter& conv, const bc::TextureRef& t) {
  auto img = conv.readback(*conv.toBgra8(*t, t->width, t->height));
  const size_t c = (static_cast<size_t>(img.height / 2) * img.width + img.width / 2) * 4;
  return {img.bgra[c + 2], img.bgra[c + 1], img.bgra[c]};
}
}  // namespace

TEST_CASE("RtxVideo: not created on a non-NVIDIA device, with the plain reason") {
  // WARP is never NVIDIA, so this reports the plain reason whatever this build's SDK setting is
  // (spec D7): the exact string the Add effect row shows.
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Warp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  std::string why;
  CHECK(bc::RtxVideo::create(*gpu, pool, conv, why) == nullptr);
  CHECK(why == "Needs an NVIDIA RTX card.");
}

TEST_CASE("RtxVideo: upscales 640x360 to 1920x1080 and cleans at 1x" BC_REQUIRES(bc::test::Req::RtxVideo)) {
  Rig r;
  std::string why;
  auto rtx = bc::RtxVideo::create(*r.gpu, r.pool, r.conv, why);
  REQUIRE(rtx != nullptr);
  auto in = r.conv.solid(640, 360, {0.2f, 0.4f, 0.1f, 1});
  const auto expected = centreRgb(r.conv, in);
  auto out = rtx->run(*in, 1920, 1080);
  CHECK(out->width == 1920);
  CHECK(out->height == 1080);
  const auto got = centreRgb(r.conv, out);
  for (int i = 0; i < 3; ++i) CHECK(std::abs(got[i] - expected[i]) <= 3);
  auto same = rtx->run(*in, 640, 360);  // VSR at 1× is Clean up
  CHECK(same->width == 640);
  CHECK(same->height == 360);
}