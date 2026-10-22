#include "core/autoframe.h"

#include <algorithm>
#include <cmath>

namespace bc {
namespace {
float clampf(float v, float lo, float hi) {
  return std::max(lo, std::min(hi, v));
}
} // namespace

void AutoFramer::reset() {
  crop_ = CropRect{0, 0, 1, 1};
  tracked_ = false;
  missing_ = 0;
}

CropRect AutoFramer::update(const std::optional<FaceBox> &face,
                            const AutoFrameSettings &s, double dtSeconds) {
  const double dt = std::clamp(dtSeconds, 0.0, 0.5);
  if (dt <= 0)
    return crop_;

  CropRect target = crop_;
  bool findFace = false;

  if (face && face->w > 0 && face->h > 0) {
    missing_ = 0;
    // The crop is as large as keeps the face at the chosen share of its height,
    // with hysteresis so a face that drifts slightly in size does not pump the
    // framing.
    float w = clampf(face->h / std::max(s.targetFace, 0.01f), 0.25f, 1.0f);
    if (tracked_ && std::abs(w - crop_.w) <= 0.12f * crop_.w)
      w = crop_.w;

    const float cx = face->x + face->w * 0.5f;
    const float cy = face->y + face->h * 0.5f;
    target = CropRect{clampf(cx - s.anchorX * w, 0.0f, 1.0f - w),
                      clampf(cy - s.anchorY * w, 0.0f, 1.0f - w), w, w};

    // Dead zone: a face inside it is already framed; move nothing.
    if (tracked_ && std::abs(target.x - crop_.x) < s.deadZone * crop_.w &&
        std::abs(target.y - crop_.y) < s.deadZone * crop_.h &&
        std::abs(target.w - crop_.w) < s.deadZone * crop_.w) {
      target = crop_;
    } else {
      findFace = true;
    }
    tracked_ = true;
  } else {
    missing_ += dt;
    // Hold the last framing for a second (a blink or a turned head is not a
    // reason to whip back), then ease out to the full picture.
    if (missing_ > 1.0)
      target = CropRect{0, 0, 1, 1};
  }

  // Exponential glide, then a speed cap so a face that jumps across the frame
  // is followed at a human pace rather than a snap.
  const float tau = std::max(s.smoothingMs, 1.0f) / 1000.0f;
  const float k = 1.0f - std::exp(-static_cast<float>(dt) / tau);
  const float maxStep = std::max(s.maxStep, 0.01f) * static_cast<float>(dt);
  auto step = [&](float now, float want) {
    float d = (want - now) * k;
    return now + clampf(d, -maxStep, maxStep);
  };

  crop_.x = step(crop_.x, target.x);
  crop_.y = step(crop_.y, target.y);
  crop_.w = clampf(step(crop_.w, target.w), 0.25f, 1.0f);
  crop_.h = crop_.w;

  // Keep the crop inside the picture at all times.
  crop_.x = clampf(crop_.x, 0.0f, 1.0f - crop_.w);
  crop_.y = clampf(crop_.y, 0.0f, 1.0f - crop_.h);

  if (!findFace && target.w >= 0.999f && crop_.w >= 0.999f)
    crop_ = CropRect{0, 0, 1, 1};
  return crop_;
}
} // namespace bc
