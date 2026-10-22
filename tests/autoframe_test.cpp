#include <doctest/doctest.h>

#include "core/autoframe.h"

using bc::AutoFramer;
using bc::AutoFrameSettings;
using bc::CropRect;
using bc::FaceBox;

namespace {
// A face box centred at (cx, cy), `size` high.
FaceBox faceAt(float cx, float cy, float size) {
  return FaceBox{cx - size * 0.5f, cy - size * 0.5f, size, size};
}
double side(const CropRect &r) { return r.w; }
} // namespace

TEST_CASE("AutoFramer: no face keeps the whole picture") {
  AutoFramer f;
  const auto s = AutoFrameSettings{};
  for (int i = 0; i < 60; ++i) {
    const CropRect r = f.update(std::nullopt, s, 1.0 / 60.0);
    CHECK(r.x == doctest::Approx(0.0f));
    CHECK(r.y == doctest::Approx(0.0f));
    CHECK(r.w == doctest::Approx(1.0f));
  }
}

TEST_CASE("AutoFramer: a face is framed and then held still") {
  AutoFramer f;
  const auto s = AutoFrameSettings{};
  // A face at the centre, a fifth of the frame high: medium framing wants 0.38,
  // so it zooms in.
  for (int i = 0; i < 600; ++i)
    f.update(faceAt(0.5f, 0.45f, 0.20f), s, 1.0 / 60.0);

  const CropRect settled = f.current();
  CHECK(settled.w < 0.9f);
  CHECK(settled.w > 0.4f);
  // The face centre lands near the anchor (0.5, 0.42): y = crop.y + anchorY *
  // crop.h.
  const float faceCentreX = settled.x + settled.w * 0.5f;
  const float faceCentreY = settled.y + settled.w * 0.42f;
  CHECK(faceCentreX == doctest::Approx(0.5f).epsilon(0.03));
  CHECK(faceCentreY == doctest::Approx(0.45f).epsilon(0.05));

  // One more second of the same face moves it by less than a pixel at 1080p.
  for (int i = 0; i < 60; ++i)
    f.update(faceAt(0.5f, 0.45f, 0.20f), s, 1.0 / 60.0);
  CHECK(std::abs(side(f.current()) - side(settled)) < 0.002f);
  CHECK(std::abs(f.current().x - settled.x) < 0.002f);
}

TEST_CASE(
    "AutoFramer: small jitter inside the dead zone does not move the crop") {
  AutoFramer f;
  const auto s = AutoFrameSettings{};
  for (int i = 0; i < 600; ++i)
    f.update(faceAt(0.5f, 0.45f, 0.20f), s, 1.0 / 60.0);
  const CropRect settled = f.current();

  for (int i = 0; i < 300; ++i) {
    const float jitter = (i % 2) ? 0.006f : -0.006f; // ~6 px at 1080p
    f.update(faceAt(0.5f + jitter, 0.45f, 0.20f), s, 1.0 / 60.0);
  }

  CHECK(std::abs(f.current().x - settled.x) < 0.002f);
  CHECK(std::abs(f.current().w - settled.w) < 0.002f);
}

TEST_CASE(
    "AutoFramer: a lost face holds, then eases back to the full picture") {
  AutoFramer f;
  const auto s = AutoFrameSettings{};
  for (int i = 0; i < 600; ++i)
    f.update(faceAt(0.5f, 0.45f, 0.20f), s, 1.0 / 60.0);
  const CropRect tracked = f.current();
  CHECK(tracked.w < 0.9f);

  // Half a second without a face: the framing is held.
  for (int i = 0; i < 30; ++i)
    f.update(std::nullopt, s, 1.0 / 60.0);
  CHECK(f.current().w == doctest::Approx(tracked.w).epsilon(0.01));

  // After the hold, it returns to the whole picture.
  for (int i = 0; i < 240; ++i)
    f.update(std::nullopt, s, 1.0 / 60.0);
  CHECK(f.current().w > 0.98f);
  CHECK(f.current().x == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("AutoFramer: a face at the edge keeps the crop inside the picture") {
  AutoFramer f;
  const auto s = AutoFrameSettings{};
  for (int i = 0; i < 600; ++i)
    f.update(faceAt(0.03f, 0.97f, 0.20f), s, 1.0 / 60.0);

  const CropRect r = f.current();
  CHECK(r.x >= 0.0f);
  CHECK(r.y >= 0.0f);
  CHECK(r.x + r.w <= 1.0001f);
  CHECK(r.y + r.h <= 1.0001f);
}

TEST_CASE("AutoFramer: smoothing is frame-rate independent") {
  bc::AutoFramer at60, at30;
  const auto s = AutoFrameSettings{};
  for (int i = 0; i < 60; ++i)
    at60.update(faceAt(0.8f, 0.5f, 0.2f), s, 1.0 / 60.0);
  for (int i = 0; i < 30; ++i)
    at30.update(faceAt(0.8f, 0.5f, 0.2f), s, 1.0 / 30.0);
  CHECK(at60.current().x == doctest::Approx(at30.current().x).epsilon(0.05));
  CHECK(at60.current().w == doctest::Approx(at30.current().w).epsilon(0.05));
}
