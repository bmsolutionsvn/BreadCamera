#include <doctest/doctest.h>

#include <algorithm>

#include "gpu/compositor.h"
#include "gpu/converter.h"
#include "gpu/device.h"

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};

  // 16x4 R8 alpha: columns < split are `left`, the rest `right`.
  bc::TextureRef alphaStep(uint8_t left, uint8_t right, uint32_t w = 16, uint32_t h = 4, uint32_t split = 8) {
    std::vector<uint8_t> px(w * h);
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) px[y * w + x] = x < split ? left : right;
    auto t = pool.acquire(w, h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    auto lock = gpu->lock();
    gpu->context()->UpdateSubresource(t->tex.Get(), 0, nullptr, px.data(), w, 0);
    return t;
  }
  // Solid linear frame, or a two-colour split frame.
  bc::TextureRef frame(std::array<float, 4> c, uint32_t w = 16, uint32_t h = 4) { return conv.solid(w, h, c); }
  bc::TextureRef split(std::array<uint8_t, 4> leftBgra, std::array<uint8_t, 4> rightBgra, uint32_t w, uint32_t h) {
    std::vector<uint8_t> px(w * h * 4);
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        auto& c = x < w / 2 ? leftBgra : rightBgra;
        std::copy(c.begin(), c.end(), px.begin() + (y * w + x) * 4);
      }
    return conv.toLinear(*conv.uploadBgra8(px.data(), w, h, w * 4), bc::PixelLayout::BGRA8, true);
  }
  bc::CpuImage read(const bc::TextureRef& t) { return conv.readback(*conv.toBgra8(*t, t->width, t->height)); }
  // Alpha of a refined matte, through a keyed white frame.
  std::vector<int> alphaRow(const bc::TextureRef& alpha) {
    bc::BackgroundSettings keyed{bc::BackgroundMode::Keyed};
    auto img = read(comp.apply(frame({1, 1, 1, 1}, alpha->width, alpha->height), alpha, keyed));
    std::vector<int> row;
    for (uint32_t x = 0; x < img.width; ++x) row.push_back(img.bgra[x * 4 + 3]);
    return row;
  }
};
}  // namespace

TEST_CASE("hexToLinear and parameter parsing") {
  auto w = bc::hexToLinear("#ffffff");
  CHECK(w[0] == doctest::Approx(1.0));
  auto g = bc::hexToLinear("#808080");
  CHECK(g[1] == doctest::Approx(0.191).epsilon(0.03));
  auto bg = bc::backgroundFrom({{"mode", "blur"}, {"blur", 20}, {"color", "#000000"}, {"image", ""}});
  CHECK(bg.mode == bc::BackgroundMode::Blur);
  CHECK(bg.blur == doctest::Approx(20));
  auto r = bc::refineFrom({{"enabled", true}, {"quality", "quality"}, {"choke", -2}, {"feather", 1.5}, {"threshold", 0.4}});
  CHECK(r.choke == doctest::Approx(-2));
  CHECK(r.feather == doctest::Approx(1.5));
  CHECK(r.threshold == doctest::Approx(0.4));
}

TEST_CASE("Compositor: refine with no choke or feather keeps a hard edge") {
  Rig r;
  auto a = r.comp.refine(*r.alphaStep(0, 255), 16, 4, {0, 0, 0.5f});
  auto row = r.alphaRow(a);
  CHECK(row[7] <= 2);
  CHECK(row[8] >= 253);
}

TEST_CASE("Compositor: positive choke shrinks the person, negative grows it") {
  Rig r;
  auto shrunk = r.alphaRow(r.comp.refine(*r.alphaStep(0, 255), 16, 4, {2, 0, 0.5f}));
  CHECK(shrunk[9] <= 2);
  CHECK(shrunk[10] >= 253);
  auto grown = r.alphaRow(r.comp.refine(*r.alphaStep(0, 255), 16, 4, {-2, 0, 0.5f}));
  CHECK(grown[6] >= 253);
  CHECK(grown[5] <= 2);
}

TEST_CASE("Compositor: feather softens the edge") {
  Rig r;
  auto row = r.alphaRow(r.comp.refine(*r.alphaStep(0, 255), 16, 4, {0, 2, 0.5f}));
  CHECK(row[7] > 25);
  CHECK(row[7] < 128);
  CHECK(row[8] > 128);
  CHECK(row[8] < 230);
  CHECK(row[0] <= 2);
  CHECK(row[15] >= 253);
}

TEST_CASE("Compositor: none and missing alpha leave the frame untouched") {
  Rig r;
  auto f = r.frame({0.3f, 0.3f, 0.3f, 1});
  CHECK(r.comp.apply(f, r.alphaStep(0, 255), {bc::BackgroundMode::None}) == f);
  CHECK(r.comp.apply(f, nullptr, {bc::BackgroundMode::Blur}) == f);
}

TEST_CASE("Compositor: replace with a colour behind the person") {
  Rig r;
  auto alpha = r.comp.refine(*r.alphaStep(0, 255), 16, 4, {0, 0, 0.5f});
  bc::BackgroundSettings s{bc::BackgroundMode::Replace};
  s.colorLinear = bc::hexToLinear("#ff0000");
  auto img = r.read(r.comp.apply(r.frame({0, 1, 0, 1}), alpha, s));
  CHECK(img.bgra[2] >= 253);         // x=0: red background
  CHECK(img.bgra[1] <= 2);
  CHECK(img.bgra[15 * 4 + 1] >= 253);  // x=15: green person
  CHECK(img.bgra[15 * 4 + 2] <= 2);
  CHECK(img.bgra[3] == 255);         // replace is opaque
}

TEST_CASE("Compositor: masked blur has no halo of person colour") {
  Rig r;
  const uint32_t w = 128, h = 32;
  auto f = r.split({0, 0, 255, 255}, {0, 255, 0, 255}, w, h);  // left red background, right green person
  auto alpha = r.comp.refine(*r.alphaStep(0, 255, w, h, w / 2), w, h, {0, 0, 0.5f});
  bc::BackgroundSettings s{bc::BackgroundMode::Blur};
  s.blur = 8 * 1080.0f / h;  // σ = 8 px at this height
  auto img = r.read(r.comp.apply(f, alpha, s));
  for (uint32_t x = 0; x < w / 2; ++x) {
    const auto* p = &img.bgra[(h / 2 * w + x) * 4];
    CHECK(p[1] <= 6);    // no green bled into the background
    CHECK(p[2] >= 249);  // still red
  }
  const auto* person = &img.bgra[(h / 2 * w + w - 1) * 4];
  CHECK(person[1] >= 253);
}
