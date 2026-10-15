#include <doctest/doctest.h>

#include <cstdlib>
#include <memory>
#include <vector>

#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/readback.h"
#include "gpu/texture_pool.h"

namespace {
struct Gpu {
  std::unique_ptr<bc::GpuDevice> dev = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*dev};
  bc::Converter conv{*dev, pool};
};

// The slot never blocks: poll tryFinish until the GPU has finished the copy (bounded, so a
// stall fails the test instead of hanging it).
bool pollFinish(bc::ReadbackSlot& slot, bc::Converter& conv, bc::CpuImage& out) {
  for (int i = 0; i < 100000; ++i) {
    if (slot.tryFinish(conv, out)) return true;
  }
  return false;
}
}  // namespace

TEST_CASE("ReadbackSlot: begin then tryFinish yields the frame") {
  Gpu g;
  auto rgba8 = g.conv.toBgra8(*g.conv.solid(8, 8, {0.214f, 0.214f, 0.214f, 1}), 8, 8);

  bc::ReadbackSlot slot;
  CHECK_FALSE(slot.pending());
  slot.begin(g.conv, *rgba8);
  CHECK(slot.pending());

  // The first tryFinish may return "not ready" (non-blocking) or the finished frame. Either
  // way it must not tear: if it is not ready, nothing changes.
  bc::CpuImage img;
  if (!slot.tryFinish(g.conv, img)) {
    CHECK(slot.pending());
    CHECK(img.width == 0);
    CHECK(img.height == 0);
    CHECK(img.bgra.empty());
  }

  REQUIRE(pollFinish(slot, g.conv, img));
  CHECK_FALSE(slot.pending());
  CHECK(img.width == 8);
  CHECK(img.height == 8);
  REQUIRE(img.bgra.size() == 8u * 8u * 4u);
  for (size_t i = 0; i < img.bgra.size(); i += 4) {
    CHECK(std::abs(img.bgra[i] - 134) <= 2);
    CHECK(std::abs(img.bgra[i + 1] - 134) <= 2);
    CHECK(std::abs(img.bgra[i + 2] - 134) <= 2);
    CHECK(img.bgra[i + 3] == 255);
  }
}

TEST_CASE("ReadbackSlot: a fresh begin after a finish works, even at a new size") {
  Gpu g;
  bc::ReadbackSlot slot;

  auto grey = g.conv.toBgra8(*g.conv.solid(8, 8, {0.214f, 0.214f, 0.214f, 1}), 8, 8);
  slot.begin(g.conv, *grey);
  bc::CpuImage first;
  REQUIRE(pollFinish(slot, g.conv, first));
  CHECK(first.width == 8);
  CHECK_FALSE(slot.pending());

  // A different size reacquires the slot's staging texture and still produces the right frame.
  auto black = g.conv.toBgra8(*g.conv.solid(4, 4, {0, 0, 0, 1}), 4, 4);
  slot.begin(g.conv, *black);
  CHECK(slot.pending());
  bc::CpuImage second;
  REQUIRE(pollFinish(slot, g.conv, second));
  CHECK_FALSE(slot.pending());
  CHECK(second.width == 4);
  CHECK(second.height == 4);
  REQUIRE(second.bgra.size() == 4u * 4u * 4u);
  for (size_t i = 0; i < second.bgra.size(); i += 4) {
    CHECK(second.bgra[i] <= 2);
    CHECK(second.bgra[i + 1] <= 2);
    CHECK(second.bgra[i + 2] <= 2);
  }
}