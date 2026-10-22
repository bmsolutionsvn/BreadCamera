#pragma once

#include <optional>

namespace bc {
// A face found in the picture, normalized to 0..1 of the frame (x/y =
// top-left).
struct FaceBox {
  float x = 0, y = 0, w = 0, h = 0;
};

struct AutoFrameSettings {
  // The face's height as a fraction of the crop: wide 0.26, medium 0.38, close
  // 0.52.
  float targetFace = 0.38f;
  // Where the face sits in the crop (0.5 = centre, 0.33 = left third, 0.67 =
  // right third).
  float anchorX = 0.5f;
  float anchorY = 0.42f;
  // Time constant of the glide. Larger is calmer and later.
  float smoothingMs = 350.0f;
  // Movement smaller than this fraction of the crop is ignored (no hunting on a
  // still face).
  float deadZone = 0.04f;
  // Fastest travel, in crop widths per second.
  float maxStep = 1.2f;
};

struct CropRect {
  float x = 0, y = 0, w = 1, h = 1;
};

// Turns face detections into a crop that keeps the face at the anchor, moving
// smoothly and ignoring small jitter. Pure and frame-rate independent: the
// caller supplies elapsed seconds.
class AutoFramer {
public:
  void reset();
  // `face` empty means none was found this tick: hold for a second, then ease
  // back to full frame.
  CropRect update(const std::optional<FaceBox> &face,
                  const AutoFrameSettings &s, double dtSeconds);
  CropRect current() const { return crop_; }

private:
  CropRect crop_{0, 0, 1, 1};
  bool tracked_ = false;
  double missing_ = 0;
};
} // namespace bc
