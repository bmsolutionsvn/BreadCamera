#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "core/color_math.h"
#include "core/skin_match.h"

namespace {
std::vector<uint8_t> solidBgra(uint32_t w, uint32_t h, uint8_t b, uint8_t g, uint8_t r) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
    px[i * 4] = b;
    px[i * 4 + 1] = g;
    px[i * 4 + 2] = r;
    px[i * 4 + 3] = 255;
  }
  return px;
}
bc::Rgb gains(const bc::MatchCorrection& c) {
  return {1.0f + c.temperature / 400.0f, 1.0f - c.tint / 400.0f, 1.0f - c.temperature / 400.0f};
}
}  // namespace

TEST_CASE("skinStats: skin pixels are counted, a blue background is not") {
  const uint32_t w = 128, h = 72;
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
      if (x < w / 2) {
        p[0] = 145;
        p[1] = 172;
        p[2] = 224;  // sRGB(224,172,145), skin
      } else {
        p[0] = 200;
        p[1] = 60;
        p[2] = 40;  // sRGB(40,60,200), blue background
      }
      p[3] = 255;
    }
  }
  auto s = bc::skinStats(px.data(), nullptr, w, h);
  REQUIRE(s.has_value());
  CHECK(s->count == 64 * 72);
  CHECK(s->mean.r == doctest::Approx(bc::toLinearLight(224.0f / 255)).epsilon(0.01));
  CHECK(s->mean.g == doctest::Approx(bc::toLinearLight(172.0f / 255)).epsilon(0.01));
  CHECK(s->mean.b == doctest::Approx(bc::toLinearLight(145.0f / 255)).epsilon(0.01));
}

TEST_CASE("skinStats: too little skin gives nothing; alpha excludes skin outside the person") {
  const uint32_t w = 128, h = 72;
  // Blue everywhere but 30 skin pixels: below the 0.5% floor.
  auto few = solidBgra(w, h, 200, 60, 40);
  for (uint32_t i = 0; i < 30; ++i) {
    uint8_t* p = &few[static_cast<size_t>(i) * 4];
    p[0] = 145;
    p[1] = 172;
    p[2] = 224;
  }
  CHECK_FALSE(bc::skinStats(few.data(), nullptr, w, h).has_value());

  // Full skin image, but the matte (alpha) covers only the right half.
  auto skin = solidBgra(w, h, 145, 172, 224);
  std::vector<uint8_t> alpha(static_cast<size_t>(w) * h * 4, 0);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = w / 2; x < w; ++x) alpha[(static_cast<size_t>(y) * w + x) * 4 + 2] = 255;
  }
  auto s = bc::skinStats(skin.data(), alpha.data(), w, h);
  REQUIRE(s.has_value());
  CHECK(s->count == 64 * 72);
}

TEST_CASE("skinTarget: is sRGB 224,172,145 in linear light") {
  bc::Rgb t = bc::skinTarget();
  CHECK(t.r == doctest::Approx(bc::toLinearLight(224.0f / 255)).epsilon(0.001));
  CHECK(t.g == doctest::Approx(bc::toLinearLight(172.0f / 255)).epsilon(0.001));
  CHECK(t.b == doctest::Approx(bc::toLinearLight(145.0f / 255)).epsilon(0.001));
}

TEST_CASE("solveMatch: identical cameras need no correction") {
  bc::SkinStats s;
  s.mean = {0.4f, 0.3f, 0.25f};
  s.count = 100;
  s.frameLuma = 0.3;
  auto c = bc::solveMatch(s, s);
  CHECK(std::abs(c.exposure) < 1e-3f);
  CHECK(std::abs(c.temperature) < 1e-3f);
  CHECK(std::abs(c.tint) < 1e-3f);
}

TEST_CASE("solveMatch: a blue-cast camera is warmed until its skin chroma matches the reference") {
  bc::SkinStats ref;
  ref.mean = {0.5f, 0.35f, 0.28f};
  ref.count = 100;
  ref.frameLuma = 0.4;
  bc::SkinStats cam;
  cam.mean = {ref.mean.r * 0.85f, ref.mean.g, ref.mean.b * 1.2f};
  cam.count = 100;
  cam.frameLuma = 0.4;
  auto c = bc::solveMatch(cam, ref);
  CHECK(c.temperature > 0);  // warmed
  // The exact match needs ~68.3, past the ±60 clamp (spec D11). The clamp is what keeps the
  // picture out of green and purple, so the chroma moves toward the reference but not all the way.
  CHECK(c.temperature == 60.0f);
  auto k = gains(c);
  const float wantRg = ref.mean.r / ref.mean.g, wantBg = ref.mean.b / ref.mean.g;
  const float gotRg = cam.mean.r * k.r / (cam.mean.g * k.g);
  const float gotBg = cam.mean.b * k.b / (cam.mean.g * k.g);
  const float camRg = cam.mean.r / cam.mean.g, camBg = cam.mean.b / cam.mean.g;
  CHECK(std::abs(gotRg - wantRg) < std::abs(camRg - wantRg));  // strictly closer to the reference
  CHECK(std::abs(gotBg - wantBg) < std::abs(camBg - wantBg));
}

TEST_CASE("solveMatch: an in-range cast matches the reference's skin chroma") {
  bc::SkinStats ref;
  ref.mean = {0.5f, 0.35f, 0.28f};
  ref.count = 100;
  ref.frameLuma = 0.4;
  bc::SkinStats cam;
  cam.mean = {ref.mean.r * 0.92f, ref.mean.g, ref.mean.b * 1.08f};  // inside the ±60 clamp
  cam.count = 100;
  cam.frameLuma = 0.4;
  auto c = bc::solveMatch(cam, ref);
  auto k = gains(c);
  CHECK(cam.mean.r * k.r / (cam.mean.g * k.g) == doctest::Approx(ref.mean.r / ref.mean.g).epsilon(0.01));
  CHECK(cam.mean.b * k.b / (cam.mean.g * k.g) == doctest::Approx(ref.mean.b / ref.mean.g).epsilon(0.01));
}

TEST_CASE("solveMatch: twice as bright a reference is +1 EV, and extremes clamp") {
  bc::SkinStats cam;
  cam.mean = {0.2f, 0.15f, 0.1f};
  cam.count = 100;
  cam.frameLuma = 0.15;
  bc::SkinStats twice = cam;
  twice.mean = {cam.mean.r * 2, cam.mean.g * 2, cam.mean.b * 2};
  CHECK(bc::solveMatch(cam, twice).exposure == doctest::Approx(1.0f).epsilon(0.02));
  bc::SkinStats sixteen = cam;
  sixteen.mean = {cam.mean.r * 16, cam.mean.g * 16, cam.mean.b * 16};
  CHECK(bc::solveMatch(cam, sixteen).exposure == 1.0f);
  bc::SkinStats red = cam;
  red.mean.r = cam.mean.r * 3;  // a 3x red cast
  CHECK(std::abs(bc::solveMatch(red, cam).temperature) == 60.0f);
}

TEST_CASE("autoBalance: a wood-panelled room stays inside the clamps") {
  const uint32_t w = 128, h = 72;
  auto px = solidBgra(w, h, 70, 120, 181);  // sRGB(181,120,70): B=70, G=120, R=181
  auto s = bc::skinStats(px.data(), nullptr, w, h);
  REQUIRE(s.has_value());  // the wood reads as skin — that is the case this pins
  auto c = bc::autoBalance(*s);
  CHECK(std::abs(c.exposure) <= 1.0f);
  CHECK(std::abs(c.temperature) <= 60.0f);
  CHECK(std::abs(c.tint) <= 60.0f);
  // The clamp keeps the wood warm: neither green (G largest) nor purple (B largest).
  auto k = gains(c);
  const float r = s->mean.r * k.r, g = s->mean.g * k.g, b = s->mean.b * k.b;
  CHECK(r >= g);
  CHECK(g >= b);
}

TEST_CASE("ease: 63% of the way after one time constant, never overshoots, dt 0 does nothing") {
  bc::MatchCorrection cur{}, target{};
  target.exposure = 1.0f;
  target.temperature = 60.0f;
  target.tint = -60.0f;
  const double k = 1 - std::exp(-1.0);
  auto after = bc::ease(cur, target, 3.0, 3.0);
  CHECK(after.exposure == doctest::Approx(static_cast<float>(k)).epsilon(0.01));
  CHECK(after.temperature == doctest::Approx(static_cast<float>(60.0 * k)).epsilon(0.01));
  CHECK(after.tint == doctest::Approx(static_cast<float>(-60.0 * k)).epsilon(0.01));
  auto far = bc::ease(cur, target, 1e6, 3.0);  // never overshoots
  CHECK(far.exposure <= 1.0f);
  CHECK(far.temperature <= 60.0f);
  CHECK(far.tint >= -60.0f);
  auto none = bc::ease(cur, target, 0.0, 3.0);  // dt 0 does nothing
  CHECK(none.exposure == 0.0f);
  CHECK(none.temperature == 0.0f);
  CHECK(none.tint == 0.0f);
}