#include <doctest/doctest.h>

#include <memory>
#include <set>
#include <string>

#include "engine/surface_hub.h"
#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "support/requires.h"

namespace {
struct Gpu {
  std::unique_ptr<bc::GpuDevice> dev = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool{*dev};
  bc::Converter conv{*dev, pool};
};
}  // namespace

TEST_CASE("SurfaceHub: subscribe creates, replaces and resizes" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Gpu g;
  bc::SurfaceHub hub(*g.dev, g.conv);
  auto grants = hub.subscribe("c1", {{"cam:A", 320, 180}, {"cam:B", 320, 180}});
  REQUIRE(grants.size() == 2);
  CHECK(grants[0].handle != 0);
  CHECK(hub.presenterCount() == 2);
  auto again = hub.subscribe("c1", {{"cam:A", 640, 360}});
  CHECK(hub.presenterCount() == 1);
  CHECK(again[0].handle == grants[0].handle);  // kept, only resized
  CHECK(again[0].w == 640);
  CHECK(hub.wanted() == std::set<std::string>{"cam:A"});
}

TEST_CASE("SurfaceHub: an empty subscription and dropClient free everything" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Gpu g;
  bc::SurfaceHub hub(*g.dev, g.conv);
  hub.subscribe("c1", {{"cam:A", 320, 180}});
  hub.subscribe("c2", {{"cam:A", 320, 180}});
  CHECK(hub.presenterCount() == 2);  // per client, never shared
  hub.subscribe("c1", {});
  CHECK(hub.presenterCount() == 1);
  hub.dropClient("c2");
  CHECK(hub.presenterCount() == 0);
  CHECK(hub.wanted().empty());
}

TEST_CASE("SurfaceHub: present to an id nobody watches is a no-op" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Gpu g;
  auto linear = g.conv.solid(320, 180, {1, 0, 0, 1});
  bc::SurfaceHub hub(*g.dev, g.conv);
  hub.present("cam:Gone", *linear);  // no throw
  CHECK_FALSE(hub.anyBroken());
}