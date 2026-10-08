#include <doctest/doctest.h>

#include <wrl/client.h>

#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/image_io.h"
#include "gpu/texture_pool.h"

using Microsoft::WRL::ComPtr;

namespace {
std::array<float, 3> apply(const bc::YuvMatrix& m, float y, float cb, float cr) {
  std::array<float, 3> out{};
  for (int r = 0; r < 3; ++r) {
    out[r] = m.rows[r][0] * y + m.rows[r][1] * cb + m.rows[r][2] * cr + m.rows[r][3];
  }
  return out;
}

struct Gpu {
  std::unique_ptr<bc::GpuDevice> dev = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*dev};
  bc::Converter conv{*dev, pool};
};

bc::TextureRef makeNv12(bc::GpuDevice& dev, uint32_t w, uint32_t h, uint8_t y, uint8_t cb, uint8_t cr) {
  std::vector<uint8_t> data(w * h * 3 / 2);
  std::fill(data.begin(), data.begin() + w * h, y);
  for (size_t i = w * h; i < data.size(); i += 2) {
    data[i] = cb;
    data[i + 1] = cr;
  }
  D3D11_TEXTURE2D_DESC d{w, h, 1, 1, DXGI_FORMAT_NV12, {1, 0}, D3D11_USAGE_DEFAULT,
                         D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_SUBRESOURCE_DATA init{data.data(), w, 0};
  ComPtr<ID3D11Texture2D> tex;
  REQUIRE(SUCCEEDED(dev.device()->CreateTexture2D(&d, &init, &tex)));
  return bc::wrapTexture(dev.device(), tex);
}

void checkAll(const bc::CpuImage& img, int b, int g, int r, int tol) {
  REQUIRE(img.bgra.size() == img.width * img.height * 4);
  for (size_t i = 0; i < img.bgra.size(); i += 4) {
    CHECK(std::abs(img.bgra[i] - b) <= tol);
    CHECK(std::abs(img.bgra[i + 1] - g) <= tol);
    CHECK(std::abs(img.bgra[i + 2] - r) <= tol);
  }
}
}  // namespace

TEST_CASE("YuvMatrix: BT.709 limited range white, black and red") {
  auto m = bc::yuvToRgbMatrix(1080, false);
  auto white = apply(m, 235 / 255.f, 128 / 255.f, 128 / 255.f);
  for (float v : white) CHECK(v == doctest::Approx(1.0f).epsilon(0.01));
  auto black = apply(m, 16 / 255.f, 128 / 255.f, 128 / 255.f);
  for (float v : black) CHECK(v == doctest::Approx(0.0f).epsilon(0.01));
  auto red = apply(m, 63 / 255.f, 102 / 255.f, 240 / 255.f);
  CHECK(red[0] == doctest::Approx(1.0f).epsilon(0.03));
  CHECK(std::abs(red[1]) < 0.03f);
  CHECK(std::abs(red[2]) < 0.03f);
}

TEST_CASE("YuvMatrix: full range maps Y=255 to white") {
  auto m = bc::yuvToRgbMatrix(1080, true);
  auto white = apply(m, 1.0f, 128 / 255.f, 128 / 255.f);
  for (float v : white) CHECK(v == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("GpuDevice: creates a device and names the adapter") {
  auto dev = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  CHECK_FALSE(dev->info().adapterName.empty());
  MESSAGE("adapter: " << dev->info().adapterName << (dev->info().warp ? " (WARP)" : ""));
}

TEST_CASE("TexturePool: a released texture is reused for the same key") {
  Gpu g;
  ID3D11Texture2D* first = nullptr;
  {
    auto t = g.pool.acquire(64, 64, bc::Converter::kLinearFormat,
                            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    first = t->tex.Get();
  }
  CHECK(g.pool.freeCount() == 1);
  auto again = g.pool.acquire(64, 64, bc::Converter::kLinearFormat,
                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  CHECK(again->tex.Get() == first);
  CHECK(g.pool.freeCount() == 0);
  CHECK_THROWS(g.pool.acquire(63, 64, DXGI_FORMAT_NV12, D3D11_BIND_SHADER_RESOURCE));
}

TEST_CASE("Converter: NV12 white and black survive the round trip") {
  Gpu g;
  auto white = makeNv12(*g.dev, 16, 16, 235, 128, 128);
  auto lin = g.conv.toLinear(*white, bc::PixelLayout::NV12, false);
  checkAll(g.conv.readback(*g.conv.toBgra8(*lin, 16, 16)), 255, 255, 255, 2);
  auto black = makeNv12(*g.dev, 16, 16, 16, 128, 128);
  checkAll(g.conv.readback(*g.conv.toBgra8(*g.conv.toLinear(*black, bc::PixelLayout::NV12, false), 16, 16)), 0, 0, 0, 2);
}

TEST_CASE("Converter: NV12 BT.709 red lands in the red channel") {
  Gpu g;
  // Height must be >= 720: yuvToRgbMatrix picks BT.709 for height >= 720 (BT.601
  // below), and these codes are BT.709 — a shorter fixture decodes through BT.601.
  auto red = makeNv12(*g.dev, 16, 720, 63, 102, 240);
  auto img = g.conv.readback(*g.conv.toBgra8(*g.conv.toLinear(*red, bc::PixelLayout::NV12, false), 16, 720));
  checkAll(img, 0, 0, 255, 4);
}

TEST_CASE("Converter: downscaling blends in linear light") {
  Gpu g;
  // 2x2 BGRA: left column black, right column white.
  const uint8_t px[] = {0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255, 255, 255, 255, 255};
  auto src = g.conv.uploadBgra8(px, 2, 2, 8);
  auto lin = g.conv.toLinear(*src, bc::PixelLayout::BGRA8, true);
  auto img = g.conv.readback(*g.conv.toBgra8(*lin, 1, 1));
  checkAll(img, 191, 191, 191, 3);  // 0.5 linear -> 0.5^(1/2.4) = 0.749
}

TEST_CASE("Converter: solid fill and BGRA upload round trips") {
  Gpu g;
  checkAll(g.conv.readback(*g.conv.toBgra8(*g.conv.solid(4, 4, {0.214f, 0.214f, 0.214f, 1}), 4, 4)), 134, 134, 134, 2);
  std::vector<uint8_t> px(4 * 4 * 4);
  for (size_t i = 0; i < px.size(); i += 4) {
    px[i] = 10;
    px[i + 1] = 120;
    px[i + 2] = 230;
    px[i + 3] = 255;
  }
  auto up = g.conv.uploadBgra8(px.data(), 4, 4, 16);
  auto img = g.conv.readback(*g.conv.toBgra8(*g.conv.toLinear(*up, bc::PixelLayout::BGRA8, true), 4, 4));
  checkAll(img, 10, 120, 230, 2);
}

TEST_CASE("loadImageBgra: a missing file throws") {
  CHECK_THROWS(bc::loadImageBgra("C:/definitely/not/here.png"));
}

TEST_CASE("Converter: fit scales to the target size and letterboxes another aspect") {
  Gpu g;
  std::vector<uint8_t> red(36 * 36 * 4);
  for (size_t i = 0; i < red.size(); i += 4) {
    red[i] = 0;
    red[i + 1] = 0;
    red[i + 2] = 255;
    red[i + 3] = 255;
  }
  auto sq = g.conv.toLinear(*g.conv.uploadBgra8(red.data(), 36, 36, 36 * 4), bc::PixelLayout::BGRA8, true);
  auto fitted = g.conv.fit(*sq, 64, 36);
  CHECK(fitted->width == 64);
  CHECK(fitted->height == 36);
  auto img = g.conv.readback(*g.conv.toBgra8(*fitted, 64, 36));
  auto px = [&](int x, int y) { return &img.bgra[(static_cast<size_t>(y) * 64 + x) * 4]; };
  CHECK(px(2, 18)[2] < 5);     // left bar: black
  CHECK(px(61, 18)[2] < 5);    // right bar: black
  CHECK(px(32, 18)[2] > 250);  // centre: the red picture
  CHECK(px(32, 18)[3] == 255);
}
