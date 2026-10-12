#include <doctest/doctest.h>

#include "core/lut.h"
#include "core/stage_params.h"
#include "gpu/color_grader.h"
#include "gpu/converter.h"
#include "gpu/device.h"

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::ColorGrader grader{*gpu, pool};
  std::array<int, 3> rgb(const bc::TextureRef& t) {
    auto img = conv.readback(*conv.toBgra8(*t, t->width, t->height));
    return {img.bgra[2], img.bgra[1], img.bgra[0]};
  }
};
}  // namespace

TEST_CASE("ColorGrader: an identity grade returns the frame itself") {
  Rig r;
  auto f = r.conv.solid(8, 8, {0.2f, 0.2f, 0.2f, 1});
  CHECK(r.grader.apply(f, bc::ColorGrade{}) == f);
  CHECK(bc::ColorGrader::isIdentity(bc::gradeFrom(bc::normalizeStageParams("color", nlohmann::json::object()))));
}

TEST_CASE("ColorGrader: exposure +1 doubles linear light") {
  Rig r;
  bc::ColorGrade g;
  g.exposure = 1;
  auto out = r.rgb(r.grader.apply(r.conv.solid(8, 8, {0.2f, 0.2f, 0.2f, 1}), g));
  for (int v : out) CHECK(std::abs(v - 174) <= 2);  // 0.4^(1/2.4)
}

TEST_CASE("ColorGrader: identity LUT changes nothing; a constant LUT paints everything") {
  Rig r;
  auto f = r.conv.solid(8, 8, {0.05f, 0.3f, 0.7f, 1});
  const auto before = r.rgb(f);
  bc::ColorGrade id;
  id.lut = r.grader.upload(bc::Lut3D::identity(33));
  auto after = r.rgb(r.grader.apply(f, id));
  for (int i = 0; i < 3; ++i) CHECK(std::abs(after[i] - before[i]) <= 1);
  auto red = bc::Lut3D::identity(2);
  for (auto& e : red.data) e = {1, 0, 0};
  bc::ColorGrade paint;
  paint.lut = r.grader.upload(red);
  auto p = r.rgb(r.grader.apply(f, paint));
  CHECK(p[0] >= 253);
  CHECK(p[1] <= 2);
  CHECK(p[2] <= 2);
}

TEST_CASE("ColorGrader: saturation 0 gives grey; GPU LUT matches the CPU LUT") {
  Rig r;
  bc::ColorGrade grey;
  grey.saturation = 0;
  auto g = r.rgb(r.grader.apply(r.conv.solid(8, 8, {0.5f, 0.1f, 0.1f, 1}), grey));
  CHECK(std::abs(g[0] - g[1]) <= 1);
  CHECK(std::abs(g[1] - g[2]) <= 1);
  auto curve = bc::Lut3D::identity(33);
  for (auto& e : curve.data) e = {std::pow(e.r, 0.8f), std::pow(e.g, 1.2f), e.b};
  bc::ColorGrade cg;
  cg.lut = r.grader.upload(curve);
  bc::Rgb lin{0.1f, 0.4f, 0.6f};
  auto gpu = r.rgb(r.grader.apply(r.conv.solid(8, 8, {lin.r, lin.g, lin.b, 1}), cg));
  auto cpu = curve.apply({bc::toGamma(lin.r), bc::toGamma(lin.g), bc::toGamma(lin.b)});
  CHECK(std::abs(gpu[0] - static_cast<int>(cpu.r * 255 + 0.5f)) <= 2);
  CHECK(std::abs(gpu[1] - static_cast<int>(cpu.g * 255 + 0.5f)) <= 2);
  CHECK(std::abs(gpu[2] - static_cast<int>(cpu.b * 255 + 0.5f)) <= 2);
}
