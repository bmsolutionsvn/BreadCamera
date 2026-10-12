#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_MAXINE
#include "engine/maxine_matte.h"
#include "gpu/device.h"

TEST_CASE("MaxineMatte: a 4-camera batch returns a 1280x720 matte per camera" BC_REQUIRES(bc::test::Req::Maxine)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::MaxineMatte m(*gpu, pool, conv, bc::MaxineMatte::defaultModelDir());
  std::vector<bc::MatteJob> jobs;
  for (int i = 0; i < 4; ++i)
    jobs.push_back({"cam" + std::to_string(i), conv.solid(1920, 1080, {0.3f, 0.25f, 0.2f, 1}), bc::MatteQuality::Quality, nullptr});
  std::vector<bc::MatteJob*> ptrs;
  for (auto& j : jobs) ptrs.push_back(&j);
  for (int run = 0; run < 3; ++run) m.process(ptrs);
  for (auto& j : jobs) {
    REQUIRE(j.alpha);
    CHECK(j.alpha->width == 1280);
    CHECK(j.alpha->height == 720);
  }
  m.forget("cam0");
  ptrs.erase(ptrs.begin());
  m.process(ptrs);  // a smaller batch after a camera left
}
#else
TEST_CASE("MaxineMatte: a 4-camera batch returns a 1280x720 matte per camera" BC_REQUIRES(bc::test::Req::Maxine)) {}
#endif
