#include "core/skin_match.h"

#include <algorithm>
#include <cmath>

namespace bc {
namespace {
// spec D10 / Task 6: a fixed BT.601 full-range YCbCr skin box.
// ponytail: a fixed YCbCr skin box; a face detector if it misfires on real footage.
bool isSkin(uint8_t b, uint8_t g, uint8_t r) {
  const double y = 0.299 * r + 0.587 * g + 0.114 * b;
  const double cb = 128 - 0.168736 * r - 0.331264 * g + 0.5 * b;
  const double cr = 128 + 0.5 * r - 0.418688 * g - 0.081312 * b;
  return y >= 40 && cb >= 77 && cb <= 127 && cr >= 133 && cr <= 173;
}

double luma(const Rgb& c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }

// spec D11: the gentle range that keeps a wrong match slightly off, never green or purple.
MatchCorrection clampCorrection(MatchCorrection c) {
  constexpr float kExp = 1.0f, kTempTint = 60.0f;
  c.exposure = std::clamp(c.exposure, -kExp, kExp);
  c.temperature = std::clamp(c.temperature, -kTempTint, kTempTint);
  c.tint = std::clamp(c.tint, -kTempTint, kTempTint);
  return c;
}

// The white-balance solve (plan Task 6). ColorGrader multiplies R by 1 + T/400, G by 1 − tint/400
// and B by 1 − T/400, so the gains that make the camera's R/G and B/G equal the reference's are
// {u, w, 2 − u} with the u, w below.
struct Gains {
  double u, w;
};
Gains solveGains(const Rgb& cam, const Rgb& ref) {
  const double cg = std::max<double>(cam.g, 1e-9), rg = std::max<double>(ref.g, 1e-9);
  const double rho = (ref.r / rg) / std::max<double>(cam.r / cg, 1e-9);
  const double beta = (ref.b / rg) / std::max<double>(cam.b / cg, 1e-9);
  const double w = 2.0 / std::max(rho + beta, 1e-9);
  return {rho * w, w};
}
}  // namespace

std::optional<SkinStats> skinStats(const uint8_t* bgra, const uint8_t* alphaBgra, uint32_t w, uint32_t h) {
  if (bgra == nullptr || w == 0 || h == 0) return std::nullopt;
  const size_t n = static_cast<size_t>(w) * h;
  SkinStats s;
  double sr = 0, sg = 0, sb = 0, lumaSum = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t b = bgra[i * 4], g = bgra[i * 4 + 1], r = bgra[i * 4 + 2];
    const float lr = toLinearLight(r / 255.0f), lg = toLinearLight(g / 255.0f), lb = toLinearLight(b / 255.0f);
    lumaSum += 0.2126 * lr + 0.7152 * lg + 0.0722 * lb;
    if (alphaBgra != nullptr && alphaBgra[i * 4 + 2] < 191) continue;
    if (!isSkin(b, g, r)) continue;
    sr += lr;
    sg += lg;
    sb += lb;
    ++s.count;
  }
  s.frameLuma = lumaSum / static_cast<double>(n);
  if (s.count < 0.005 * static_cast<double>(n)) return std::nullopt;
  const double c = s.count;
  s.mean = {static_cast<float>(sr / c), static_cast<float>(sg / c), static_cast<float>(sb / c)};
  return s;
}

MatchCorrection solveMatch(const SkinStats& cam, const SkinStats& ref) {
  const Gains g = solveGains(cam.mean, ref.mean);
  MatchCorrection c;
  c.temperature = static_cast<float>(400 * (g.u - 1));
  c.tint = static_cast<float>(400 * (1 - g.w));
  const Rgb corrected{static_cast<float>(cam.mean.r * g.u), static_cast<float>(cam.mean.g * g.w),
                      static_cast<float>(cam.mean.b * (2 - g.u))};
  const double lr = luma(ref.mean), lc = luma(corrected);
  c.exposure = (lr > 0 && lc > 0) ? static_cast<float>(std::log2(lr / lc)) : 0.0f;
  return clampCorrection(c);
}

MatchCorrection autoBalance(const SkinStats& cam) {
  const Rgb target = skinTarget();
  const Gains g = solveGains(cam.mean, target);
  MatchCorrection c;
  c.temperature = static_cast<float>(400 * (g.u - 1));
  c.tint = static_cast<float>(400 * (1 - g.w));
  c.exposure = cam.frameLuma > 0 ? static_cast<float>(std::log2(0.18 / cam.frameLuma)) : 0.0f;
  return clampCorrection(c);
}

MatchCorrection ease(const MatchCorrection& cur, const MatchCorrection& target, double dtSeconds,
                     double tauSeconds) {
  const double k = tauSeconds > 0 ? 1 - std::exp(-dtSeconds / tauSeconds) : 1.0;
  auto mix = [k](float a, float b) { return static_cast<float>(a + (b - a) * k); };
  return {mix(cur.exposure, target.exposure), mix(cur.temperature, target.temperature),
          mix(cur.tint, target.tint)};
}

Rgb skinTarget() {
  // ponytail: one reference skin chroma for auto balance; tune on real webcams.
  return {toLinearLight(224.0f / 255), toLinearLight(172.0f / 255), toLinearLight(145.0f / 255)};
}
}  // namespace bc