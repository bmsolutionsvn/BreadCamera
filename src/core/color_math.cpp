#include "core/color_math.h"

#include <algorithm>
#include <cmath>

namespace bc {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double Xn = 0.95047, Yn = 1.0, Zn = 1.08883;
double f(double t) {
  constexpr double d = 6.0 / 29.0;
  return t > d * d * d ? std::cbrt(t) : t / (3 * d * d) + 4.0 / 29.0;
}
double finv(double t) {
  constexpr double d = 6.0 / 29.0;
  return t > d ? t * t * t : 3 * d * d * (t - 4.0 / 29.0);
}
double deg(double r) { return r * 180.0 / kPi; }
double rad(double d) { return d * kPi / 180.0; }
}  // namespace

float toGamma(float linear) { return std::pow(std::max(linear, 0.0f), 1.0f / 2.4f); }
float toLinearLight(float gamma) { return std::pow(std::max(gamma, 0.0f), 2.4f); }

Lab linearToLab(Rgb c) {
  const double X = 0.4124 * c.r + 0.3576 * c.g + 0.1805 * c.b;
  const double Y = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
  const double Z = 0.0193 * c.r + 0.1192 * c.g + 0.9505 * c.b;
  const double fx = f(X / Xn), fy = f(Y / Yn), fz = f(Z / Zn);
  return {116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)};
}

Rgb labToLinear(const Lab& lab) {
  const double fy = (lab.L + 16) / 116, fx = fy + lab.a / 500, fz = fy - lab.b / 200;
  const double X = Xn * finv(fx), Y = Yn * finv(fy), Z = Zn * finv(fz);
  return {static_cast<float>(3.2406 * X - 1.5372 * Y - 0.4986 * Z),
          static_cast<float>(-0.9689 * X + 1.8758 * Y + 0.0415 * Z),
          static_cast<float>(0.0557 * X - 0.2040 * Y + 1.0570 * Z)};
}

double deltaE2000(const Lab& p, const Lab& q) {
  const double C1 = std::hypot(p.a, p.b), C2 = std::hypot(q.a, q.b);
  const double Cbar = (C1 + C2) / 2, C7 = std::pow(Cbar, 7);
  const double G = 0.5 * (1 - std::sqrt(C7 / (C7 + std::pow(25.0, 7))));
  const double a1 = (1 + G) * p.a, a2 = (1 + G) * q.a;
  const double C1p = std::hypot(a1, p.b), C2p = std::hypot(a2, q.b);
  auto hue = [](double b, double a) {
    if (a == 0 && b == 0) return 0.0;
    double h = deg(std::atan2(b, a));
    return h < 0 ? h + 360 : h;
  };
  const double h1 = hue(p.b, a1), h2 = hue(q.b, a2);
  const double dL = q.L - p.L, dC = C2p - C1p;
  double dh = 0;
  if (C1p * C2p != 0) {
    dh = h2 - h1;
    if (dh > 180) dh -= 360;
    else if (dh < -180) dh += 360;
  }
  const double dH = 2 * std::sqrt(C1p * C2p) * std::sin(rad(dh / 2));
  const double Lbar = (p.L + q.L) / 2, Cbarp = (C1p + C2p) / 2;
  double hbar = h1 + h2;
  if (C1p * C2p != 0) {
    if (std::abs(h1 - h2) <= 180) hbar = (h1 + h2) / 2;
    else hbar = (h1 + h2 < 360) ? (h1 + h2 + 360) / 2 : (h1 + h2 - 360) / 2;
  }
  const double T = 1 - 0.17 * std::cos(rad(hbar - 30)) + 0.24 * std::cos(rad(2 * hbar)) +
                   0.32 * std::cos(rad(3 * hbar + 6)) - 0.20 * std::cos(rad(4 * hbar - 63));
  const double dTheta = 30 * std::exp(-std::pow((hbar - 275) / 25, 2));
  const double Cp7 = std::pow(Cbarp, 7);
  const double RC = 2 * std::sqrt(Cp7 / (Cp7 + std::pow(25.0, 7)));
  const double SL = 1 + 0.015 * std::pow(Lbar - 50, 2) / std::sqrt(20 + std::pow(Lbar - 50, 2));
  const double SC = 1 + 0.045 * Cbarp, SH = 1 + 0.015 * Cbarp * T;
  const double RT = -std::sin(rad(2 * dTheta)) * RC;
  const double tL = dL / SL, tC = dC / SC, tH = dH / SH;
  return std::sqrt(tL * tL + tC * tC + tH * tH + RT * tC * tH);
}
}  // namespace bc
