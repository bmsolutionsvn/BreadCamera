#pragma once
#include <algorithm>
#include <array>
#include <cmath>

#include "core/chart.h"
#include "core/color_math.h"
#include "gpu/converter.h"

namespace bc::test {
// Draws a 6x4 chart (patches with small gaps) into a grey picture at the given corners.
// `cast` multiplies each patch in linear light (a camera's colour cast).
inline bc::CpuImage renderChart(uint32_t w, uint32_t h, const bc::ChartQuad& q, const std::array<std::array<uint8_t, 3>, 24>& srgb,
                                std::array<float, 3> cast = {1, 1, 1}) {
  bc::CpuImage img{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4)};
  const auto inv = bc::chartToPicture(q).inverse();
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const auto uv = inv.map((x + 0.5) / w, (y + 0.5) / h);
      std::array<uint8_t, 3> rgb{60, 60, 60};  // background
      if (uv[0] >= 0 && uv[0] < 1 && uv[1] >= 0 && uv[1] < 1) {
        rgb = {20, 20, 20};  // the card between patches
        const int col = std::min(5, static_cast<int>(uv[0] * 6)), row = std::min(3, static_cast<int>(uv[1] * 4));
        const double fu = uv[0] * 6 - col, fv = uv[1] * 4 - row;
        if (fu > 0.08 && fu < 0.92 && fv > 0.08 && fv < 0.92) {
          const auto& p = srgb[static_cast<size_t>(row * 6 + col)];
          for (int c = 0; c < 3; ++c) {
            const float lin = std::min(1.0f, bc::toLinearLight(p[c] / 255.0f) * cast[c]);
            rgb[c] = static_cast<uint8_t>(std::lround(bc::toGamma(lin) * 255.0f));
          }
        }
      }
      uint8_t* px = &img.bgra[(static_cast<size_t>(y) * w + x) * 4];
      px[0] = rgb[2];
      px[1] = rgb[1];
      px[2] = rgb[0];
      px[3] = 255;
    }
  }
  return img;
}
}  // namespace bc::test
