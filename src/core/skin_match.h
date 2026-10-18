#pragma once
#include <cstdint>
#include <optional>

#include "core/color_math.h"

namespace bc {
// spec D10/D13: Match measures skin, not the whole picture.
struct SkinStats {
  Rgb mean;              // linear RGB mean of the skin pixels
  int count = 0;         // skin pixels seen
  double frameLuma = 0;  // mean linear BT.709 luminance of every pixel
};

// A pixel is skin when its gamma-encoded 8-bit values fall in a fixed BT.601 full-range YCbCr box;
// with `alphaBgra` (the matte, same size) only pixels whose red byte (alpha after toBgra8's gamma
// encode) is >= 191 count. Returns nullopt for fewer than 0.5% skin pixels.
// `bgra` is BGRA8; `alphaBgra` may be null.
std::optional<SkinStats> skinStats(const uint8_t* bgra, const uint8_t* alphaBgra, uint32_t w, uint32_t h);

// spec D11: Match drives the existing ColorGrader's exposure/temperature/tint, clamped to a gentle
// range so a wrong match looks slightly off, never green or purple.
struct MatchCorrection {
  float exposure = 0;     // EV, clamped to ±1
  float temperature = 0;  // Colour field units, clamped to ±60
  float tint = 0;         // Colour field units, clamped to ±60
};

MatchCorrection solveMatch(const SkinStats& cam, const SkinStats& ref);
MatchCorrection autoBalance(const SkinStats& cam);
// spec D12: the correction eases towards its target over a few seconds and never jumps.
MatchCorrection ease(const MatchCorrection& cur, const MatchCorrection& target, double dtSeconds,
                     double tauSeconds = 3.0);
// spec D13: a natural skin chroma for auto balance (sRGB 224,172,145 in linear light).
Rgb skinTarget();
}  // namespace bc