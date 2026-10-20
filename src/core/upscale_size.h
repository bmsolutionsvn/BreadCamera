#pragma once
#include <cmath>
#include <cstdint>

namespace bc {
struct FrameSize {
  uint32_t w = 0, h = 0;
};

// Upscale's target size (spec D2/D3): the target is a height (720 or 1080); the width follows the
// input's aspect, rounded to an even number. A target not larger than the input changes nothing
// (Upscale never shrinks a picture), so the input size is returned unchanged.
inline FrameSize upscaleTarget(uint32_t inW, uint32_t inH, uint32_t targetH) {
  if (targetH <= inH) return {inW, inH};
  const uint32_t w = static_cast<uint32_t>(2 * std::lround(inW * targetH / (2.0 * inH)));
  return {w, targetH};
}
}  // namespace bc