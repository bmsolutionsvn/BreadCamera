#include <doctest/doctest.h>

#include <dxgi1_4.h>

#include <memory>
#include <string>

#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/surface_presenter.h"
#include "gpu/texture_pool.h"
#include "support/requires.h"

namespace {
struct Gpu {
  std::unique_ptr<bc::GpuDevice> dev = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool{*dev};
  bc::Converter conv{*dev, pool};
};
}  // namespace

TEST_CASE("SurfacePresenter: creates a handle and presents a picture" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Gpu g;
  auto linear = g.conv.solid(320, 180, {1, 0, 0, 1});
  bc::SurfacePresenter p(*g.dev, g.conv, 200, 100);
  CHECK(p.handle() != nullptr);
  CHECK(p.present(*linear));  // first present is never "still drawing"
  CHECK_FALSE(p.broken());
}

TEST_CASE("SurfacePresenter: resize takes effect on the next present" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Gpu g;
  auto linear = g.conv.solid(320, 180, {0, 1, 0, 1});
  bc::SurfacePresenter p(*g.dev, g.conv, 200, 100);
  p.resize(640, 360);
  p.present(*linear);
  DXGI_SWAP_CHAIN_DESC1 d{};
  p.swapChainForTest()->GetDesc1(&d);
  CHECK(d.Width == 640);
  CHECK(d.Height == 360);
  CHECK(d.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
}

TEST_CASE("SurfacePresenter: sizes are clamped" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Gpu g;
  bc::SurfacePresenter p(*g.dev, g.conv, 1, 99999);
  CHECK(p.width() == 16);
  CHECK(p.height() == 4096);
}

TEST_CASE("SurfacePresenter: available() reports true on a hardware GPU" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Gpu g;
  std::string why;
  CHECK(bc::SurfacePresenter::available(*g.dev, g.conv, &why));
  CHECK(why.empty());
}