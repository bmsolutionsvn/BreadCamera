#pragma once

namespace bc {
struct Rgb {
  float r = 0, g = 0, b = 0;
};
struct Lab {
  double L = 0, a = 0, b = 0;
};

// The pipeline's transfer function (spec §3.2): gamma 2.4.
float toGamma(float linear);
float toLinearLight(float gamma);
// Linear BT.709 RGB (D65) ↔ CIELAB.
Lab linearToLab(Rgb c);
Rgb labToLinear(const Lab& lab);
double deltaE2000(const Lab& x, const Lab& y);
}  // namespace bc
