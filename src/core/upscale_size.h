#pragma once
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

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

// Enhance's VSR quality level (decision E3): 1 Fast to 4 Ultra; 3 High is what every show used
// before, so an old show looks the same. An unknown string falls back to High.
inline int vsrLevel(std::string_view quality) {
  if (quality == "fast") return 1;
  if (quality == "balanced") return 2;
  if (quality == "ultra") return 4;
  return 3;
}

// What one Enhance pass does with a frame (decision E4). `targetH` 0 means "Same size".
struct EnhancePlan {
  FrameSize want;
  bool resize = false;
  bool vsr = false;
};

inline EnhancePlan enhancePlan(uint32_t inW, uint32_t inH, uint32_t targetH, bool rtx) {
  EnhancePlan p;
  p.want = targetH == 0 ? FrameSize{inW, inH} : upscaleTarget(inW, inH, targetH);
  p.resize = p.want.w != inW || p.want.h != inH;
  // One VSR pass per fresh frame, and none above 1080 lines unless it also resizes: a 4K
  // clean-up pass would overrun the budget.
  p.vsr = rtx && (p.resize || inH <= 1080);
  return p;
}

// The note the Inspector shows for the frame in hand (decision E7), or nothing.
inline std::optional<std::string> enhanceNote(uint32_t inW, uint32_t inH, uint32_t targetH, bool rtx) {
  const EnhancePlan p = enhancePlan(inW, inH, targetH, rtx);
  if (p.resize) return std::nullopt;
  if (!rtx) {
    if (targetH == 0) return std::string("Same size needs an NVIDIA RTX card");
    return std::string(targetH == 720 ? "Input is already 720p" : "Input is already 1080p");
  }
  if (!p.vsr) return std::string("Input is above 1080p");
  return std::string("AI clean-up only");
}
}  // namespace bc