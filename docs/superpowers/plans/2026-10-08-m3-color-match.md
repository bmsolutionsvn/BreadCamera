# BreadCamera M3 — One-time colour calibration with a chart Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Each camera is calibrated once against a colour chart — a printed ColorChecker Classic 24 or a tablet showing the same chart. In the camera's Colour effect the operator clicks Calibrate, holds the chart up, BreadCamera finds it (or the operator drags four corners), Capture averages ~0.5 s, solves per-channel tone curves + a 3×3 matrix, bakes a locked 33³ LUT, and reports the result in plain words.

**Rewritten 2026-10-08 after the UI mockup review (owner):** the reference-camera match with region boxes, trims and `.cube` import is replaced by chart calibration (spec §5.3, amended). Tasks 1, 3 and 4 keep their tested code (colour maths, `.cube` files for the stored LUTs, the GPU grader, the colour stage and the frame capture); the trims stay in the schema as hidden fields at neutral values. Tasks 2, 5, 6, 7 and 8 are new.

**Architecture:** Pure CPU code in `bc_core` for colour maths, chart geometry (homography), patch sampling and the solver (unit-tested with a synthetic chart and a known cast, ΔE2000). An optional chart detector in `bc_engine` uses OpenCV's `mcc` module (`BC_WITH_CHART_DETECT`); without it the operator places the corners. A `ColorGrader` on the GPU applies the LUT as the **first** stage (before matte, spec §3.2). A `ColorCalibrationService` captures frames through `Engine::capture`, samples, solves, writes the LUT next to the show and keeps the tablet reference.

**Tech Stack:** as M1/M2, plus OpenCV 4 with the contrib `mcc` module from vcpkg (optional: chart detection only).

**Spec:** `docs/superpowers/specs/2026-10-08-breadcamera-design.md` (§5.3, §3.2, §6.4 "Calibrate", §8.1 calibration solver and chart detection).

**Branch:** `bcam-m3`, cut from `bcam-m2`.

## Global Constraints

- All M1 and M2 Global Constraints apply (English-only UI, add-in effects, `BC_WITH_*` + `BC_REQUIRES`).
- Stage order is **Color → Matte → Background**. The colour stage is a guarded stage like the others (bypass on failure/overrun, retry after 5 s); it is an add-in (`color` schema, `onAdd {"enabled": true}`).
- Calibration is **one-time and locked**: nothing changes until the operator calibrates again (spec §5.3). No continuous adaptation.
- The chart is the **ColorChecker Classic 24** layout (6 × 4). Printed card → target = the published sRGB values; tablet → target = the first tablet-calibrated camera's reading of the same tablet in this show (that camera keeps an identity LUT and is the reference).
- LUTs are **33³**, written as standard `.cube` files in `<show dir>\luts\` (gamma 2.4 domain, red fastest).
- Capture: 15 frames, every other tick, at 960×540, from the frame **before** any stage. A camera without picture fails with a clear message; no LUT changes.
- Result ratings: mean ΔE2000 ≤ 2 **Good match**, ≤ 4 **Fair**, otherwise **Poor** (the UI shows words; numbers in the tooltip).
- The solver's acceptance test: synthetic chart with a known cast → calibrate → **mean ΔE2000 < 2** (spec §8.1).
- OpenCV is Apache-2.0; only the `contrib` modules that need no extra licence are built. Detection is optional: a missing `mcc` module never fails the build.

## Review Focus

1. **The corners are not on the chart, or the chart is half out of the picture** — Capture refuses with "the corners are not on the chart" and changes nothing — owned by Task 2 (sampleChart check) and Task 6.
2. **The chart is upside down or tilted** — the result is the same as upright — owned by Task 2 (orientation test) and Task 5.
3. **A chart that is too dark or has a clipped white patch** — Capture refuses and says what to fix (add light / lower exposure) — owned by Task 2.
4. **The tablet reference camera is cleared or removed** — the next tablet calibration becomes the new reference; no camera is matched to a stale reading — owned by Task 6.
5. **The LUT file disappears from disk** — the colour stage shows `color-lut` and the camera runs uncorrected, never black — owned by Task 4.

## File Structure

```
src/core/color_math.h/.cpp     linear↔Lab, ΔE2000                         (Task 1)
src/core/lut.h/.cpp            Lut3D type, .cube read/write, CPU apply      (Task 1)
src/core/chart.h/.cpp          ColorChecker values, homography, sampling    (Task 2)
src/core/calibration.h/.cpp    solver, ratings                              (Task 2)
src/gpu/color_grader.h/.cpp    3D LUT upload + grade shader                 (Task 3)
src/gpu/shaders/color_grade.hlsl
src/engine/chart_detector.h/.cpp   OpenCV mcc (BC_WITH_CHART_DETECT)        (Task 5)
src/engine/color_calibration_service.h/.cpp                                (Task 6)
tests/support/chart_image.h    draws a chart into a CpuImage                (Task 2)
ui/src/calibration/*           Calibrate flow, chart overlay, chart image   (Task 7)
docs/superpowers/manual-checks/m3-calibration.md                           (Task 8)
```

## Execution notes for the controller

- 8 tasks → batched reviews (1–3, 4–6, 7–8).
- Task 5 adds OpenCV (contrib) to `vcpkg.json`: the next build compiles it once (30–60 min). Expected; do not cancel. If the vcpkg build has no `mcc` module, `BC_WITH_CHART_DETECT=OFF`, the detection tests skip with a printed reason, and the manual-corner path is the product (ledger it).

---

### Task 1: Colour maths and the `.cube` LUT format

**Files:**
- Create: `src/core/color_math.h`, `src/core/color_math.cpp`, `src/core/lut.h`, `src/core/lut.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/color_math_test.cpp`, `tests/lut_test.cpp`

**Interfaces:**
- Produces:
  - `bc::Rgb { float r, g, b; }`, `bc::Lab { double L, a, b; }`, `float bc::toGamma(float linear)`, `float bc::toLinearLight(float gamma)` (gamma 2.4, clamped to ≥ 0), `bc::Lab bc::linearToLab(Rgb)`, `bc::Rgb bc::labToLinear(const Lab&)`, `double bc::deltaE2000(const Lab&, const Lab&)`.
  - `bc::Lut3D { int size = 0; std::vector<Rgb> data; /* size³, red fastest */ Rgb at(int r, int g, int b) const; Rgb apply(Rgb gammaIn) const; static Lut3D identity(int size); }`, `bc::Lut3D bc::readCube(const std::filesystem::path&)`, `void bc::writeCube(const std::filesystem::path&, const Lut3D&, std::string_view title)`, `class bc::CubeError : std::runtime_error` (message includes the line number).

`.cube` rules: `TITLE` and `#` comments ignored; `LUT_3D_SIZE N` with 2 ≤ N ≤ 65; optional `DOMAIN_MIN 0 0 0` / `DOMAIN_MAX 1 1 1` (any other domain is refused); exactly N³ data lines of three finite floats; `LUT_1D_SIZE` is refused ("1D LUTs are not supported"); files over 16 MiB are refused.

- [ ] **Step 1: Write the failing tests**

`tests/color_math_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "core/color_math.h"

TEST_CASE("deltaE2000 matches the Sharma reference pair") {
  bc::Lab a{50.0, 2.6772, -79.7751}, b{50.0, 0.0, -82.7485};
  CHECK(bc::deltaE2000(a, b) == doctest::Approx(2.0425).epsilon(0.0005));
  bc::Lab c{50.0, 2.5, 0.0}, d{73.0, 25.0, -18.0};
  CHECK(bc::deltaE2000(c, d) == doctest::Approx(27.1492).epsilon(0.0005));
  CHECK(bc::deltaE2000(a, a) == doctest::Approx(0.0));
}

TEST_CASE("Lab round trip and white point") {
  auto white = bc::linearToLab({1, 1, 1});
  CHECK(white.L == doctest::Approx(100.0).epsilon(0.001));
  CHECK(std::abs(white.a) < 0.01);
  bc::Rgb x{0.2f, 0.5f, 0.1f};
  auto back = bc::labToLinear(bc::linearToLab(x));
  CHECK(back.r == doctest::Approx(0.2).epsilon(0.001));
  CHECK(back.g == doctest::Approx(0.5).epsilon(0.001));
  CHECK(back.b == doctest::Approx(0.1).epsilon(0.001));
  CHECK(bc::toLinearLight(bc::toGamma(0.3f)) == doctest::Approx(0.3f).epsilon(0.0001));
}
```

`tests/lut_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <fstream>

#include "core/lut.h"
#include "support/temp_dir.h"

namespace {
void write(const std::filesystem::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }
}  // namespace

TEST_CASE("Lut3D: identity applies as identity, trilinear in between") {
  auto id = bc::Lut3D::identity(33);
  auto o = id.apply({0.25f, 0.5f, 0.75f});
  CHECK(o.r == doctest::Approx(0.25f).epsilon(0.0001));
  CHECK(o.g == doctest::Approx(0.5f).epsilon(0.0001));
  CHECK(o.b == doctest::Approx(0.75f).epsilon(0.0001));
  auto clamped = id.apply({-1.0f, 2.0f, 0.5f});
  CHECK(clamped.r == doctest::Approx(0.0f));
  CHECK(clamped.g == doctest::Approx(1.0f));
}

TEST_CASE("cube: write then read round-trips") {
  bc::test::TempDir dir;
  auto lut = bc::Lut3D::identity(5);
  lut.data[7] = {0.1f, 0.2f, 0.3f};
  bc::writeCube(dir.path / "a.cube", lut, "test");
  auto back = bc::readCube(dir.path / "a.cube");
  CHECK(back.size == 5);
  CHECK(back.data[7].g == doctest::Approx(0.2f).epsilon(0.00001));
}

TEST_CASE("cube: malformed files are refused with a line number") {
  bc::test::TempDir dir;
  auto p = dir.path / "bad.cube";
  write(p, "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n");  // 3 of 8 lines
  CHECK_THROWS_AS(bc::readCube(p), bc::CubeError);
  write(p, "LUT_3D_SIZE 2\n0 0 0\n1 0 x\n");
  try {
    bc::readCube(p);
    FAIL("should throw");
  } catch (const bc::CubeError& e) {
    CHECK(std::string(e.what()).find("line 3") != std::string::npos);
  }
  write(p, "LUT_3D_SIZE 200\n");
  CHECK_THROWS_AS(bc::readCube(p), bc::CubeError);
  write(p, "LUT_1D_SIZE 1024\n");
  CHECK_THROWS_AS(bc::readCube(p), bc::CubeError);
  write(p, "DOMAIN_MAX 2 2 2\nLUT_3D_SIZE 2\n");
  CHECK_THROWS_AS(bc::readCube(p), bc::CubeError);
  write(p, "LUT_3D_SIZE 2\n0 0 nan\n");
  CHECK_THROWS_AS(bc::readCube(p), bc::CubeError);
  CHECK_THROWS_AS(bc::readCube(dir.path / "missing.cube"), bc::CubeError);
}
```

Add both to `bc_tests`, and `src/core/color_math.cpp src/core/lut.cpp` to `bc_core`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "deltaE*,Lab*,Lut3D*,cube*"`
Expected: build FAILS — headers not found.

- [ ] **Step 3: Implement colour maths**

`src/core/color_math.h`:
```cpp
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
```

`src/core/color_math.cpp`:
```cpp
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
```

- [ ] **Step 4: Implement the LUT type and `.cube`**

`src/core/lut.h`:
```cpp
#pragma once
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "core/color_math.h"

namespace bc {
// size³ entries, red index fastest (the .cube order). Works on gamma-encoded values.
struct Lut3D {
  int size = 0;
  std::vector<Rgb> data;
  Rgb at(int r, int g, int b) const { return data[static_cast<size_t>((b * size + g) * size + r)]; }
  Rgb apply(Rgb in) const;  // trilinear, input clamped to 0..1
  static Lut3D identity(int size);
};

class CubeError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

Lut3D readCube(const std::filesystem::path& path);
void writeCube(const std::filesystem::path& path, const Lut3D& lut, std::string_view title);
}  // namespace bc
```

`src/core/lut.cpp`:
```cpp
#include "core/lut.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace bc {
Lut3D Lut3D::identity(int size) {
  Lut3D l;
  l.size = size;
  l.data.resize(static_cast<size_t>(size) * size * size);
  const float s = 1.0f / static_cast<float>(size - 1);
  for (int b = 0; b < size; ++b)
    for (int g = 0; g < size; ++g)
      for (int r = 0; r < size; ++r) l.data[static_cast<size_t>((b * size + g) * size + r)] = {r * s, g * s, b * s};
  return l;
}

Rgb Lut3D::apply(Rgb in) const {
  auto coord = [&](float v, int& i0, float& t) {
    const float x = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(size - 1);
    i0 = std::min(static_cast<int>(x), size - 2);
    t = x - static_cast<float>(i0);
  };
  int r0, g0, b0;
  float tr, tg, tb;
  coord(in.r, r0, tr);
  coord(in.g, g0, tg);
  coord(in.b, b0, tb);
  auto lerp = [](Rgb a, Rgb b, float t) { return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; };
  auto c00 = lerp(at(r0, g0, b0), at(r0 + 1, g0, b0), tr);
  auto c10 = lerp(at(r0, g0 + 1, b0), at(r0 + 1, g0 + 1, b0), tr);
  auto c01 = lerp(at(r0, g0, b0 + 1), at(r0 + 1, g0, b0 + 1), tr);
  auto c11 = lerp(at(r0, g0 + 1, b0 + 1), at(r0 + 1, g0 + 1, b0 + 1), tr);
  return lerp(lerp(c00, c10, tg), lerp(c01, c11, tg), tb);
}

Lut3D readCube(const std::filesystem::path& path) {
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(path, ec);
  if (ec) throw CubeError("cannot open " + path.string());
  if (bytes > 16u * 1024 * 1024) throw CubeError("the LUT file is larger than 16 MiB");
  std::ifstream f(path);
  if (!f) throw CubeError("cannot open " + path.string());
  Lut3D lut;
  std::string line;
  int lineNo = 0;
  while (std::getline(f, line)) {
    ++lineNo;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') continue;
    std::istringstream in(line.substr(first));
    std::string word;
    in >> word;
    auto fail = [&](const std::string& why) { throw CubeError("line " + std::to_string(lineNo) + ": " + why); };
    if (word == "TITLE") continue;
    if (word == "LUT_1D_SIZE") fail("1D LUTs are not supported");
    if (word == "LUT_3D_SIZE") {
      if (!(in >> lut.size) || lut.size < 2 || lut.size > 65) fail("LUT_3D_SIZE must be 2 to 65");
      lut.data.reserve(static_cast<size_t>(lut.size) * lut.size * lut.size);
      continue;
    }
    if (word == "DOMAIN_MIN" || word == "DOMAIN_MAX") {
      float a, b, c;
      const float want = word == "DOMAIN_MIN" ? 0.0f : 1.0f;
      if (!(in >> a >> b >> c) || a != want || b != want || c != want) fail("only a 0..1 domain is supported");
      continue;
    }
    if (lut.size == 0) fail("data before LUT_3D_SIZE");
    std::istringstream nums(line.substr(first));
    float r, g, b;
    if (!(nums >> r >> g >> b) || !std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) fail("expected three numbers");
    if (lut.data.size() == lut.data.capacity()) fail("more entries than LUT_3D_SIZE³");
    lut.data.push_back({r, g, b});
  }
  if (lut.size == 0) throw CubeError("no LUT_3D_SIZE line");
  const size_t want = static_cast<size_t>(lut.size) * lut.size * lut.size;
  if (lut.data.size() != want) {
    throw CubeError("line " + std::to_string(lineNo) + ": expected " + std::to_string(want) + " entries, found " +
                    std::to_string(lut.data.size()));
  }
  return lut;
}

void writeCube(const std::filesystem::path& path, const Lut3D& lut, std::string_view title) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::trunc);
  if (!f) throw CubeError("cannot write " + path.string());
  f << "TITLE \"" << title << "\"\nLUT_3D_SIZE " << lut.size << "\n" << std::fixed << std::setprecision(6);
  for (auto& c : lut.data) f << c.r << ' ' << c.g << ' ' << c.b << '\n';
  if (!f) throw CubeError("cannot write " + path.string());
}
}  // namespace bc
```

Note on `nan`: `std::istream >> float` fails on the text `nan`, which the `!(nums >> ...)` check reports — the `isfinite` check also covers values like `1e40`.

- [ ] **Step 5: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "deltaE*,Lab*,Lut3D*,cube*"`
Expected: 5 cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/core tests CMakeLists.txt
git commit -m "feat(core): Lab/ΔE2000 colour maths and .cube LUT read/write"
```

### Task 2: Chart geometry, patch sampling and the calibration solver

**Files:**
- Create: `src/core/chart.h`, `src/core/chart.cpp`, `src/core/calibration.h`, `src/core/calibration.cpp`, `tests/support/chart_image.h`
- Modify: `CMakeLists.txt` (`chart.cpp`, `calibration.cpp` in `bc_core`; tests)
- Test: `tests/chart_test.cpp`, `tests/calibration_test.cpp`

**Interfaces:**
- Consumes: `Rgb`, `Lab`, `linearToLab`, `toGamma`, `toLinearLight`, `deltaE2000`, `Lut3D` (Task 1); `CpuImage` (M1 Task 5 — `gpu/converter.h` declares it; include it).
- Produces:
  - `bc::ChartQuad { std::array<std::array<double, 2>, 4> c; }` — the outer corners of the 24-patch grid in normalised picture coordinates (0..1), in chart order: top-left (dark skin), top-right, bottom-right, bottom-left. `bool bc::validQuad(const ChartQuad&)`.
  - `using bc::ChartColors = std::array<Rgb, 24>;` (linear light, row by row, 6 per row).
  - `const ChartColors& bc::colorCheckerReference()`, `const std::array<std::array<uint8_t, 3>, 24>& bc::colorCheckerSrgb()`.
  - `bc::Homography { std::array<double, 9> m; std::array<double, 2> map(double u, double v) const; Homography inverse() const; }`, `bc::Homography bc::chartToPicture(const ChartQuad&)` (throws `std::invalid_argument` for a degenerate quad).
  - `bc::ChartColors bc::sampleChartRaw(const std::vector<CpuImage>& frames, const ChartQuad&)` — averages the centre 40 % of each patch over all frames, in the order the corners give; `bool bc::chartUpright(const ChartColors&)` — the neutral row is at the bottom and ramps from white to black.
  - `bc::ChartColors bc::sampleChart(const std::vector<CpuImage>& frames, const ChartQuad&)` — `sampleChartRaw`, turned round when the chart is upside down; throws `std::invalid_argument("the corners are not on the chart")` when neither way is upright.
  - `bc::Calibration { Lut3D lut; double deltaEBefore = 0, deltaEAfter = 0; }`, `bc::Calibration bc::solveCalibration(const ChartColors& measured, const ChartColors& target, int lutSize = 33)`, `double bc::meanDeltaE(const ChartColors&, const ChartColors&)`, `std::string_view bc::calibrationRating(double meanDeltaE)` (`"good"` ≤ 2, `"fair"` ≤ 4, else `"poor"`).
  - Test helper `bc::test::renderChart(uint32_t w, uint32_t h, const ChartQuad&, const std::array<std::array<uint8_t,3>,24>& srgb, std::array<float, 3> cast = {1, 1, 1})` → `CpuImage` (BGRA).

Solver (spec §5.3):
1. **Refuse** when the mean luminance of the six neutrals (patches 19–24, indices 18–23) is below 0.01 ("the chart is too dark — add light on it") or the white patch (index 18) is above 0.95 ("the white patch is clipped — lower the exposure or the light on the chart").
2. **Tone curves**, per channel, in the gamma domain: points (gamma(measured neutral), gamma(target neutral)) for the six neutrals plus (0, 0) and (1, 1); sorted by x; equal x averaged; y forced non-decreasing; linear between points; each curve clamped to |curve(x) − x| ≤ 0.25.
3. **Matrix**: apply the curves to all 24 measured patches, back to linear; fit a 3×3 matrix to the 24 targets by least squares with a ridge pull towards identity (λ = 0.24); if its determinant is below 0.05, use identity.
4. **Bake** a lutSize³ LUT: grid value (gamma) → curves → linear → matrix → clamp 0..1 → gamma.
5. **ΔE**: before = `meanDeltaE(measured, target)`; after = the LUT applied (on the CPU) to the measured patches vs the target.

- [ ] **Step 1: Write the test helper and the failing tests**

`tests/support/chart_image.h`:
```cpp
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
```

`tests/chart_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <algorithm>

#include "core/chart.h"
#include "support/chart_image.h"

namespace {
const bc::ChartQuad kTilted{{{{0.30, 0.25}, {0.72, 0.22}, {0.74, 0.70}, {0.28, 0.72}}}};

void checkMatchesReference(const bc::ChartColors& got) {
  for (size_t i = 0; i < 24; ++i) {
    const auto& want = bc::colorCheckerSrgb()[i];
    CHECK(std::abs(bc::toGamma(got[i].r) * 255 - want[0]) <= 2.5);
    CHECK(std::abs(bc::toGamma(got[i].g) * 255 - want[1]) <= 2.5);
    CHECK(std::abs(bc::toGamma(got[i].b) * 255 - want[2]) <= 2.5);
  }
}
}  // namespace

TEST_CASE("chartToPicture maps the corners, inverts, and refuses a degenerate quad") {
  auto h = bc::chartToPicture(kTilted);
  auto tl = h.map(0, 0), br = h.map(1, 1);
  CHECK(tl[0] == doctest::Approx(0.30));
  CHECK(br[1] == doctest::Approx(0.70));
  auto back = h.inverse().map(0.74, 0.70);
  CHECK(back[0] == doctest::Approx(1.0).epsilon(1e-6));
  CHECK(back[1] == doctest::Approx(1.0).epsilon(1e-6));
  CHECK_THROWS_AS(bc::chartToPicture(bc::ChartQuad{{{{0.1, 0.1}, {0.1, 0.1}, {0.1, 0.1}, {0.1, 0.1}}}}), std::invalid_argument);
  CHECK(bc::validQuad(kTilted));
  CHECK_FALSE(bc::validQuad(bc::ChartQuad{{{{0.3, 0.3}, {0.31, 0.3}, {0.31, 0.31}, {0.3, 0.31}}}}));  // too small
}

TEST_CASE("sampleChart reads every patch of a tilted chart") {
  auto img = bc::test::renderChart(640, 360, kTilted, bc::colorCheckerSrgb());
  checkMatchesReference(bc::sampleChart({img}, kTilted));
}

TEST_CASE("sampleChart turns an upside-down chart to the normal order") {
  auto reversed = bc::colorCheckerSrgb();
  std::reverse(reversed.begin(), reversed.end());  // the card held upside down
  auto img = bc::test::renderChart(640, 360, kTilted, reversed);
  checkMatchesReference(bc::sampleChart({img}, kTilted));
}

TEST_CASE("sampleChart refuses corners that are not on the chart") {
  auto img = bc::test::renderChart(640, 360, kTilted, bc::colorCheckerSrgb());
  bc::ChartQuad off{{{{0.02, 0.02}, {0.2, 0.02}, {0.2, 0.15}, {0.02, 0.15}}}};
  CHECK_THROWS_WITH_AS(bc::sampleChart({img}, off), doctest::Contains("not on the chart"), std::invalid_argument);
}
```

`tests/calibration_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <cmath>

#include "core/calibration.h"

namespace {
// A camera with channel crosstalk, more gain and a slightly different tone response.
bc::ChartColors withCast(const bc::ChartColors& ref) {
  bc::ChartColors out;
  for (size_t i = 0; i < 24; ++i) {
    const auto c = ref[i];
    const float r = 0.85f * c.r + 0.10f * c.g + 0.02f * c.b;
    const float g = 0.05f * c.r + 0.80f * c.g + 0.05f * c.b;
    const float b = 0.02f * c.r + 0.08f * c.g + 0.65f * c.b;
    out[i] = {std::pow(std::min(1.0f, r * 1.1f), 1.08f), std::pow(std::min(1.0f, g * 1.1f), 1.08f), std::pow(std::min(1.0f, b * 1.1f), 1.08f)};
  }
  return out;
}
}  // namespace

TEST_CASE("solveCalibration: a camera with a known cast is corrected below 2 dE") {
  const auto& ref = bc::colorCheckerReference();
  auto cal = bc::solveCalibration(withCast(ref), ref);
  MESSAGE("mean dE2000 before " << cal.deltaEBefore << ", after " << cal.deltaEAfter);
  CHECK(cal.deltaEBefore > 5);
  CHECK(cal.deltaEAfter < 2);
  CHECK(cal.lut.size == 33);
  CHECK(bc::calibrationRating(cal.deltaEAfter) == "good");
}

TEST_CASE("solveCalibration: a camera that already matches gets a near-identity LUT") {
  const auto& ref = bc::colorCheckerReference();
  auto cal = bc::solveCalibration(ref, ref);
  CHECK(cal.deltaEAfter < 0.5);
  auto mid = cal.lut.apply({0.5f, 0.5f, 0.5f});
  CHECK(mid.r == doctest::Approx(0.5).epsilon(0.02));
  CHECK(mid.b == doctest::Approx(0.5).epsilon(0.02));
}

TEST_CASE("solveCalibration: too dark or clipped charts are refused; an extreme cast stays bounded") {
  const auto& ref = bc::colorCheckerReference();
  bc::ChartColors dark;
  for (size_t i = 0; i < 24; ++i) dark[i] = {ref[i].r * 0.005f, ref[i].g * 0.005f, ref[i].b * 0.005f};
  CHECK_THROWS_WITH_AS(bc::solveCalibration(dark, ref), doctest::Contains("too dark"), std::invalid_argument);
  auto clipped = ref;
  clipped[18] = {1, 1, 1};
  CHECK_THROWS_WITH_AS(bc::solveCalibration(clipped, ref), doctest::Contains("clipped"), std::invalid_argument);
  bc::ChartColors blue;
  for (size_t i = 0; i < 24; ++i) blue[i] = {ref[i].r * 0.3f, ref[i].g * 0.5f, std::min(0.9f, ref[i].b * 3.0f)};
  auto cal = bc::solveCalibration(blue, ref);
  for (auto& e : cal.lut.data) {
    CHECK(std::isfinite(e.r));
    CHECK(e.g >= 0.0f);
    CHECK(e.b <= 1.0f);
  }
}

TEST_CASE("calibrationRating words") {
  CHECK(bc::calibrationRating(1.9) == "good");
  CHECK(bc::calibrationRating(3.5) == "fair");
  CHECK(bc::calibrationRating(6.0) == "poor");
}
```

Add both test files to `bc_tests`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "chartToPicture*,sampleChart*,solveCalibration*,calibrationRating*"`
Expected: build FAILS — `core/chart.h` not found.

- [ ] **Step 3: Chart geometry and sampling**

`src/core/chart.h`:
```cpp
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "core/color_math.h"
#include "gpu/converter.h"  // CpuImage

namespace bc {
// Outer corners of the 24-patch grid, normalised to the picture (0..1), in chart order:
// top-left (dark skin), top-right, bottom-right, bottom-left.
struct ChartQuad {
  std::array<std::array<double, 2>, 4> c;
};
using ChartColors = std::array<Rgb, 24>;  // linear light, row by row, 6 per row

// ColorChecker Classic 24, published sRGB (D65) values (spec §5.3).
const std::array<std::array<uint8_t, 3>, 24>& colorCheckerSrgb();
const ChartColors& colorCheckerReference();  // the same, in linear light

// Chart space (u across the 6 columns, v down the 4 rows, both 0..1) to the picture.
struct Homography {
  std::array<double, 9> m;  // a b c / d e f / g h 1
  std::array<double, 2> map(double u, double v) const;
  Homography inverse() const;
};
Homography chartToPicture(const ChartQuad& q);  // throws std::invalid_argument for a degenerate quad
bool validQuad(const ChartQuad& q);              // convex, inside the picture, at least 0.2 % of it

ChartColors sampleChartRaw(const std::vector<CpuImage>& frames, const ChartQuad& q);
bool chartUpright(const ChartColors& c);
ChartColors sampleChart(const std::vector<CpuImage>& frames, const ChartQuad& q);
}  // namespace bc
```

`src/core/chart.cpp`:
```cpp
#include "core/chart.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace bc {
namespace {
double luma(const Rgb& c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
double chroma(const Rgb& c) {
  const double hi = std::max({c.r, c.g, c.b}), lo = std::min({c.r, c.g, c.b});
  return (hi - lo) / (hi + 1e-6);
}
double rowChroma(const ChartColors& p, int row) {
  double s = 0;
  for (int i = 0; i < 6; ++i) s += chroma(p[static_cast<size_t>(row * 6 + i)]);
  return s;
}
}  // namespace

const std::array<std::array<uint8_t, 3>, 24>& colorCheckerSrgb() {
  static const std::array<std::array<uint8_t, 3>, 24> v{{
      {115, 82, 68},   {194, 150, 130}, {98, 122, 157},  {87, 108, 67},   {133, 128, 177}, {103, 189, 170},
      {214, 126, 44},  {80, 91, 166},   {193, 90, 99},   {94, 60, 108},   {157, 188, 64},  {224, 163, 46},
      {56, 61, 150},   {70, 148, 73},   {175, 54, 60},   {231, 199, 31},  {187, 86, 149},  {8, 133, 161},
      {243, 243, 242}, {200, 200, 200}, {160, 160, 160}, {122, 122, 121}, {85, 85, 85},    {52, 52, 52},
  }};
  return v;
}

const ChartColors& colorCheckerReference() {
  static const ChartColors lin = [] {
    ChartColors out;
    for (size_t i = 0; i < 24; ++i) {
      const auto& s = colorCheckerSrgb()[i];
      out[i] = {toLinearLight(s[0] / 255.0f), toLinearLight(s[1] / 255.0f), toLinearLight(s[2] / 255.0f)};
    }
    return out;
  }();
  return lin;
}

std::array<double, 2> Homography::map(double u, double v) const {
  const double w = m[6] * u + m[7] * v + m[8];
  return {(m[0] * u + m[1] * v + m[2]) / w, (m[3] * u + m[4] * v + m[5]) / w};
}

Homography Homography::inverse() const {
  const auto& a = m;
  const double det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
  if (std::abs(det) < 1e-12) throw std::invalid_argument("the chart corners are degenerate");
  Homography r;
  r.m = {(a[4] * a[8] - a[5] * a[7]) / det, (a[2] * a[7] - a[1] * a[8]) / det, (a[1] * a[5] - a[2] * a[4]) / det,
         (a[5] * a[6] - a[3] * a[8]) / det, (a[0] * a[8] - a[2] * a[6]) / det, (a[2] * a[3] - a[0] * a[5]) / det,
         (a[3] * a[7] - a[4] * a[6]) / det, (a[1] * a[6] - a[0] * a[7]) / det, (a[0] * a[4] - a[1] * a[3]) / det};
  return r;
}

// Unit square → quad (Heckbert): (0,0)→c0, (1,0)→c1, (1,1)→c2, (0,1)→c3.
Homography chartToPicture(const ChartQuad& q) {
  const auto& [p0, p1, p2, p3] = q.c;
  const double dx1 = p1[0] - p2[0], dx2 = p3[0] - p2[0], dx3 = p0[0] - p1[0] + p2[0] - p3[0];
  const double dy1 = p1[1] - p2[1], dy2 = p3[1] - p2[1], dy3 = p0[1] - p1[1] + p2[1] - p3[1];
  const double den = dx1 * dy2 - dx2 * dy1;
  if (std::abs(den) < 1e-12) throw std::invalid_argument("the chart corners are degenerate");
  const double g = (dx3 * dy2 - dx2 * dy3) / den;
  const double h = (dx1 * dy3 - dx3 * dy1) / den;
  Homography r;
  r.m = {p1[0] - p0[0] + g * p1[0], p3[0] - p0[0] + h * p3[0], p0[0],
         p1[1] - p0[1] + g * p1[1], p3[1] - p0[1] + h * p3[1], p0[1],
         g, h, 1.0};
  return r;
}

bool validQuad(const ChartQuad& q) {
  double area = 0;
  int sign = 0;
  for (int i = 0; i < 4; ++i) {
    const auto& a = q.c[static_cast<size_t>(i)];
    const auto& b = q.c[static_cast<size_t>((i + 1) % 4)];
    const auto& c = q.c[static_cast<size_t>((i + 2) % 4)];
    if (a[0] < -0.05 || a[0] > 1.05 || a[1] < -0.05 || a[1] > 1.05) return false;
    area += a[0] * b[1] - b[0] * a[1];
    const double cross = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0]);
    const int s = cross > 0 ? 1 : cross < 0 ? -1 : 0;
    if (s == 0 || (sign != 0 && s != sign)) return false;
    sign = s;
  }
  return std::abs(area) / 2 >= 0.002;
}

ChartColors sampleChartRaw(const std::vector<CpuImage>& frames, const ChartQuad& q) {
  if (frames.empty()) throw std::invalid_argument("no frames to sample");
  const auto h = chartToPicture(q);
  ChartColors out{};
  for (int p = 0; p < 24; ++p) {
    const double cu = (p % 6 + 0.5) / 6.0, cv = (p / 6 + 0.5) / 4.0;
    double r = 0, g = 0, b = 0;
    int n = 0;
    for (const auto& f : frames) {
      for (int sy = -3; sy <= 3; ++sy) {
        for (int sx = -3; sx <= 3; ++sx) {
          const auto xy = h.map(cu + sx * (0.2 / 6.0) / 3.0, cv + sy * (0.2 / 4.0) / 3.0);
          const int x = std::clamp(static_cast<int>(xy[0] * f.width), 0, static_cast<int>(f.width) - 1);
          const int y = std::clamp(static_cast<int>(xy[1] * f.height), 0, static_cast<int>(f.height) - 1);
          const uint8_t* px = &f.bgra[(static_cast<size_t>(y) * f.width + x) * 4];
          r += toLinearLight(px[2] / 255.0f);
          g += toLinearLight(px[1] / 255.0f);
          b += toLinearLight(px[0] / 255.0f);
          ++n;
        }
      }
    }
    out[static_cast<size_t>(p)] = {static_cast<float>(r / n), static_cast<float>(g / n), static_cast<float>(b / n)};
  }
  return out;
}

bool chartUpright(const ChartColors& c) {
  return rowChroma(c, 3) < rowChroma(c, 0) && rowChroma(c, 3) < rowChroma(c, 1) && luma(c[18]) >= 1.5 * luma(c[23]);
}

ChartColors sampleChart(const std::vector<CpuImage>& frames, const ChartQuad& q) {
  if (!validQuad(q)) throw std::invalid_argument("place the four corners on the chart");
  auto out = sampleChartRaw(frames, q);
  if (chartUpright(out)) return out;
  std::reverse(out.begin(), out.end());  // held upside down
  if (chartUpright(out)) return out;
  throw std::invalid_argument("the corners are not on the chart");
}
}  // namespace bc
```

- [ ] **Step 4: The solver**

`src/core/calibration.h`:
```cpp
#pragma once
#include <string_view>

#include "core/chart.h"
#include "core/lut.h"

namespace bc {
struct Calibration {
  Lut3D lut;
  double deltaEBefore = 0;
  double deltaEAfter = 0;
};

// measured → target (spec §5.3): tone curves from the neutrals, a 3x3 matrix by least squares,
// baked into a lutSize³ LUT. Throws std::invalid_argument when the chart is too dark or clipped.
Calibration solveCalibration(const ChartColors& measured, const ChartColors& target, int lutSize = 33);
double meanDeltaE(const ChartColors& a, const ChartColors& b);
std::string_view calibrationRating(double meanDeltaE);
}  // namespace bc
```

`src/core/calibration.cpp`:
```cpp
#include "core/calibration.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bc {
namespace {
using Mat3 = std::array<std::array<double, 3>, 3>;

double luma(const Rgb& c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }
float ch(const Rgb& c, int i) { return i == 0 ? c.r : i == 1 ? c.g : c.b; }

// One tone curve: sorted, non-decreasing points in the gamma domain.
struct Curve {
  std::vector<std::pair<double, double>> pts;
  double operator()(double x) const {
    if (x <= pts.front().first) return pts.front().second;
    for (size_t i = 1; i < pts.size(); ++i) {
      if (x <= pts[i].first) {
        const auto [x0, y0] = pts[i - 1];
        const auto [x1, y1] = pts[i];
        return x1 - x0 < 1e-9 ? y1 : y0 + (y1 - y0) * (x - x0) / (x1 - x0);
      }
    }
    return pts.back().second;
  }
};

Curve neutralCurve(const ChartColors& measured, const ChartColors& target, int c) {
  std::vector<std::pair<double, double>> p{{0.0, 0.0}, {1.0, 1.0}};
  for (size_t i = 18; i < 24; ++i) p.push_back({toGamma(ch(measured[i], c)), toGamma(ch(target[i], c))});
  std::sort(p.begin(), p.end());
  Curve out;
  for (auto& [x, y] : p) {
    if (!out.pts.empty() && std::abs(out.pts.back().first - x) < 1e-6) {
      out.pts.back().second = (out.pts.back().second + y) / 2;
      continue;
    }
    out.pts.push_back({x, y});
  }
  for (size_t i = 1; i < out.pts.size(); ++i) out.pts[i].second = std::max(out.pts[i].second, out.pts[i - 1].second);
  for (auto& [x, y] : out.pts) y = std::clamp(y, x - 0.25, x + 0.25);
  return out;
}

Rgb curved(const std::array<Curve, 3>& curves, Rgb lin) {
  return {toLinearLight(static_cast<float>(curves[0](toGamma(lin.r)))), toLinearLight(static_cast<float>(curves[1](toGamma(lin.g)))),
          toLinearLight(static_cast<float>(curves[2](toGamma(lin.b))))};
}

double det3(const Mat3& a) {
  return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
         a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
}

// Solves A x = b for 3x3 A (Cramer).
std::array<double, 3> solve3(const Mat3& a, const std::array<double, 3>& b) {
  const double d = det3(a);
  std::array<double, 3> x{};
  for (int k = 0; k < 3; ++k) {
    Mat3 m = a;
    for (int r = 0; r < 3; ++r) m[static_cast<size_t>(r)][static_cast<size_t>(k)] = b[static_cast<size_t>(r)];
    x[static_cast<size_t>(k)] = det3(m) / d;
  }
  return x;
}

// Least squares x_i·M ≈ t_i, pulled towards identity: (XᵀX + λI) M_k = Xᵀt_k + λ e_k.
Mat3 fitMatrix(const std::vector<Rgb>& x, const ChartColors& t) {
  constexpr double kLambda = 0.24;
  Mat3 xtx{};
  for (auto& v : x)
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c) xtx[static_cast<size_t>(r)][static_cast<size_t>(c)] += ch(v, r) * ch(v, c);
  for (int i = 0; i < 3; ++i) xtx[static_cast<size_t>(i)][static_cast<size_t>(i)] += kLambda;
  Mat3 m{};
  for (int k = 0; k < 3; ++k) {
    std::array<double, 3> xty{};
    for (size_t i = 0; i < x.size(); ++i)
      for (int r = 0; r < 3; ++r) xty[static_cast<size_t>(r)] += ch(x[i], r) * ch(t[i], k);
    xty[static_cast<size_t>(k)] += kLambda;
    m[static_cast<size_t>(k)] = solve3(xtx, xty);  // row k: output channel k from (r, g, b)
  }
  if (det3(m) < 0.05) m = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
  return m;
}

Rgb apply(const Mat3& m, Rgb v) {
  auto row = [&](int k) {
    const auto& r = m[static_cast<size_t>(k)];
    return static_cast<float>(std::clamp(r[0] * v.r + r[1] * v.g + r[2] * v.b, 0.0, 1.0));
  };
  return {row(0), row(1), row(2)};
}
}  // namespace

double meanDeltaE(const ChartColors& a, const ChartColors& b) {
  double s = 0;
  for (size_t i = 0; i < 24; ++i) s += deltaE2000(linearToLab(a[i]), linearToLab(b[i]));
  return s / 24;
}

std::string_view calibrationRating(double d) { return d <= 2 ? "good" : d <= 4 ? "fair" : "poor"; }

Calibration solveCalibration(const ChartColors& measured, const ChartColors& target, int lutSize) {
  double neutral = 0;
  for (size_t i = 18; i < 24; ++i) neutral += luma(measured[i]);
  if (neutral / 6 < 0.01) throw std::invalid_argument("the chart is too dark — add light on it");
  if (luma(measured[18]) > 0.95) throw std::invalid_argument("the white patch is clipped — lower the exposure or the light on the chart");

  const std::array<Curve, 3> curves{neutralCurve(measured, target, 0), neutralCurve(measured, target, 1), neutralCurve(measured, target, 2)};
  std::vector<Rgb> x;
  for (auto& m : measured) x.push_back(curved(curves, m));
  const Mat3 mat = fitMatrix(x, target);

  Calibration out;
  out.lut.size = lutSize;
  out.lut.data.resize(static_cast<size_t>(lutSize) * lutSize * lutSize);
  const float n1 = static_cast<float>(lutSize - 1);
  for (int b = 0; b < lutSize; ++b)
    for (int g = 0; g < lutSize; ++g)
      for (int r = 0; r < lutSize; ++r) {
        const Rgb lin{toLinearLight(r / n1), toLinearLight(g / n1), toLinearLight(b / n1)};
        const Rgb v = apply(mat, curved(curves, lin));
        out.lut.data[static_cast<size_t>((b * lutSize + g) * lutSize + r)] = {toGamma(v.r), toGamma(v.g), toGamma(v.b)};
      }

  ChartColors corrected;
  for (size_t i = 0; i < 24; ++i) {
    const auto gOut = out.lut.apply({toGamma(measured[i].r), toGamma(measured[i].g), toGamma(measured[i].b)});
    corrected[i] = {toLinearLight(gOut.r), toLinearLight(gOut.g), toLinearLight(gOut.b)};
  }
  out.deltaEBefore = meanDeltaE(measured, target);
  out.deltaEAfter = meanDeltaE(corrected, target);
  return out;
}
}  // namespace bc
```
Check against Task 1 before writing: `Lut3D::apply` takes and returns **gamma-encoded** values and the data layout is red-fastest (`index = (b·n + g)·n + r`); if Task 1's helpers differ, adapt here and ledger it.

- [ ] **Step 5: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "chartToPicture*,sampleChart*,solveCalibration*,calibrationRating*"`
Expected: all pass. The first solver case prints the before/after ΔE — paste it into the report. If "after" is not below 2, do not loosen the bound: report the numbers and the per-patch worst three.

- [ ] **Step 6: Commit**

```bash
git add src/core tests CMakeLists.txt
git commit -m "feat(core): chart geometry, patch sampling and the chart calibration solver"
```

### Task 3: The colour stage parameters and the GPU colour grader

**Files:**
- Create: `src/gpu/color_grader.h`, `src/gpu/color_grader.cpp`, `src/gpu/shaders/color_grade.hlsl`
- Modify: `src/core/stage_params.h/.cpp` (`hidden` fields; the `color` schema, listed first), `tests/stage_params_test.cpp` and `tests/command_router_test.cpp` (schema count 2 → 3), `CMakeLists.txt`
- Test: `tests/color_grader_test.cpp`, `tests/stage_params_test.cpp` (new case)

**Interfaces:**
- Consumes: `StageField`/`StageSchema`/`normalizeStageParams` (M2 Task 1); `Lut3D` (Task 1); `GpuDevice`, `TexturePool`, `Converter::kLinearFormat` (M1).
- Produces:
  - `StageField::hidden` (`bool`, default `false`; emitted as `"hidden": true` by `schemaToJson`).
  - Stage `color` (first in `stageSchemas()`; label "Colour", description "Calibrate this camera once against a colour chart (printed card or tablet); the correction is then locked.", icon `color`, `onAdd {"enabled": true}`): `enabled` bool false (the on/off switch); **hidden** fields — the trims `exposure` −3..3 = 0, `temperature` −100..100 = 0, `tint` −100..100 = 0, `lift` −0.2..0.2 = 0, `gamma` 0.5..2 = 1, `gain` 0.5..2 = 1, `saturation` 0..2 = 1 (kept at neutral; the grader still supports them), `lut` path "", `previousLut` path "", `chart` enum `card`|`tablet` = `card`, `deltaE` 0..100 = 0, `deltaEBefore` 0..100 = 0, `calibratedAt` 0..4102444800 (unix seconds) = 0, `reference` bool false (Task 6 writes these).
  - `bc::GpuLut { Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv; int size; }`, `bc::ColorGrade { float exposure = 0, temperature = 0, tint = 0, lift = 0, gamma = 1, gain = 1, saturation = 1; std::shared_ptr<const GpuLut> lut; }`, `bc::ColorGrade bc::gradeFrom(const nlohmann::json& colorParams)` (no LUT), `bc::ColorGrader(GpuDevice&, TexturePool&)`: `std::shared_ptr<const GpuLut> upload(const Lut3D&)`, `TextureRef apply(const TextureRef& frame, const ColorGrade&)`, `static bool isIdentity(const ColorGrade&)`.

Grade order (spec §5.3, "trims on top"): LUT (gamma domain, trilinear 3D texture) → exposure (× 2^EV, linear) → white balance (temperature T: R × (1 + T/400), B × (1 − T/400); tint t: G × (1 − t/400)) → lift/gain then gamma in the gamma domain (`g = gain·(g + lift·(1 − g)); g = g^(1/gamma)`) → saturation around BT.709 luma (linear). Alpha passes through. An identity grade with no LUT returns the input texture itself.

- [ ] **Step 1: Write the failing tests**

Append to `tests/stage_params_test.cpp`:
```cpp
TEST_CASE("colour stage: defaults, hidden LUT fields, order") {
  auto c = bc::normalizeStageParams("color", nlohmann::json::object());
  CHECK(c["enabled"] == false);
  CHECK(c["gamma"] == 1);
  CHECK(c["lut"] == "");
  CHECK(bc::stageSchemas().front().kind == "color");
  auto j = bc::schemaToJson(*bc::findStageSchema("color"));
  bool lutHidden = false;
  for (auto& f : j["fields"]) if (f["key"] == "lut") lutHidden = f.value("hidden", false);
  CHECK(lutHidden);
  CHECK(j["icon"] == "color");
  CHECK(j["onAdd"]["enabled"] == true);
  for (auto& f : j["fields"]) if (f["key"] != "enabled") CHECK(f.value("hidden", false));  // the UI shows only the switch
  CHECK_THROWS_AS(bc::normalizeStageParams("color", {{"exposure", 4}}), std::invalid_argument);
}
```
and change the two existing count checks from 2 to 3 (`stageSchemas().size() == 3` in `stage_params_test.cpp`, `["result"].size() == 3` in the `setStage` router test).

`tests/color_grader_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include "core/lut.h"
#include "gpu/color_grader.h"
#include "gpu/converter.h"
#include "gpu/device.h"

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::ColorGrader grader{*gpu, pool};
  std::array<int, 3> rgb(const bc::TextureRef& t) {
    auto img = conv.readback(*conv.toBgra8(*t, t->width, t->height));
    return {img.bgra[2], img.bgra[1], img.bgra[0]};
  }
};
}  // namespace

TEST_CASE("ColorGrader: an identity grade returns the frame itself") {
  Rig r;
  auto f = r.conv.solid(8, 8, {0.2f, 0.2f, 0.2f, 1});
  CHECK(r.grader.apply(f, bc::ColorGrade{}) == f);
  CHECK(bc::ColorGrader::isIdentity(bc::gradeFrom(bc::normalizeStageParams("color", nlohmann::json::object()))));
}

TEST_CASE("ColorGrader: exposure +1 doubles linear light") {
  Rig r;
  bc::ColorGrade g;
  g.exposure = 1;
  auto out = r.rgb(r.grader.apply(r.conv.solid(8, 8, {0.2f, 0.2f, 0.2f, 1}), g));
  for (int v : out) CHECK(std::abs(v - 174) <= 2);  // 0.4^(1/2.4)
}

TEST_CASE("ColorGrader: identity LUT changes nothing; a constant LUT paints everything") {
  Rig r;
  auto f = r.conv.solid(8, 8, {0.05f, 0.3f, 0.7f, 1});
  const auto before = r.rgb(f);
  bc::ColorGrade id;
  id.lut = r.grader.upload(bc::Lut3D::identity(33));
  auto after = r.rgb(r.grader.apply(f, id));
  for (int i = 0; i < 3; ++i) CHECK(std::abs(after[i] - before[i]) <= 1);
  auto red = bc::Lut3D::identity(2);
  for (auto& e : red.data) e = {1, 0, 0};
  bc::ColorGrade paint;
  paint.lut = r.grader.upload(red);
  auto p = r.rgb(r.grader.apply(f, paint));
  CHECK(p[0] >= 253);
  CHECK(p[1] <= 2);
  CHECK(p[2] <= 2);
}

TEST_CASE("ColorGrader: saturation 0 gives grey; GPU LUT matches the CPU LUT") {
  Rig r;
  bc::ColorGrade grey;
  grey.saturation = 0;
  auto g = r.rgb(r.grader.apply(r.conv.solid(8, 8, {0.5f, 0.1f, 0.1f, 1}), grey));
  CHECK(std::abs(g[0] - g[1]) <= 1);
  CHECK(std::abs(g[1] - g[2]) <= 1);
  auto curve = bc::Lut3D::identity(33);
  for (auto& e : curve.data) e = {std::pow(e.r, 0.8f), std::pow(e.g, 1.2f), e.b};
  bc::ColorGrade cg;
  cg.lut = r.grader.upload(curve);
  bc::Rgb lin{0.1f, 0.4f, 0.6f};
  auto gpu = r.rgb(r.grader.apply(r.conv.solid(8, 8, {lin.r, lin.g, lin.b, 1}), cg));
  auto cpu = curve.apply({bc::toGamma(lin.r), bc::toGamma(lin.g), bc::toGamma(lin.b)});
  CHECK(std::abs(gpu[0] - static_cast<int>(cpu.r * 255 + 0.5f)) <= 2);
  CHECK(std::abs(gpu[1] - static_cast<int>(cpu.g * 255 + 0.5f)) <= 2);
  CHECK(std::abs(gpu[2] - static_cast<int>(cpu.b * 255 + 0.5f)) <= 2);
}
```

Add it to `bc_tests`; add `bc_shader(color_grade cs_5_0)` before `add_library(bc_gpu ...)` and `src/gpu/color_grader.cpp` to `bc_gpu`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "ColorGrader*,colour stage*"`
Expected: build FAILS.

- [ ] **Step 3: Add the colour schema**

`src/core/stage_params.h`: add `bool hidden = false;` to `StageField`.

`src/core/stage_params.cpp`:
- in `schemaToJson`, after building `j`: `if (f.hidden) j["hidden"] = true;`
- in `build()`, create the colour schema and return `{color, matte, background}`:
```cpp
  StageSchema color{"color", "Colour",
                    "Calibrate this camera once against a colour chart (printed card or tablet); the correction is then locked.",
                    "color", {}, json{{"enabled", true}}, {}};
  auto hide = [](StageField f) {
    f.hidden = true;
    return f;
  };
  color.fields = {
      simple("enabled", "bool", false, "On"),
      hide(number("exposure", -3, 3, 0.05, 0, "Exposure (EV)")),
      hide(number("temperature", -100, 100, 1, 0, "Temperature")),
      hide(number("tint", -100, 100, 1, 0, "Tint")),
      hide(number("lift", -0.2, 0.2, 0.005, 0, "Lift")),
      hide(number("gamma", 0.5, 2, 0.01, 1, "Gamma")),
      hide(number("gain", 0.5, 2, 0.01, 1, "Gain")),
      hide(number("saturation", 0, 2, 0.01, 1, "Saturation")),
      hide(simple("lut", "path", "", "LUT")),
      hide(simple("previousLut", "path", "", "Previous LUT")),
      hide(enumeration("chart", {"card", "tablet"}, {"Printed card", "Tablet"}, "card", "Chart")),
      hide(number("deltaE", 0, 100, 0.01, 0, "Colour error after")),
      hide(number("deltaEBefore", 0, 100, 0.01, 0, "Colour error before")),
      hide(number("calibratedAt", 0, 4102444800.0, 1, 0, "Calibrated at")),
      hide(simple("reference", "bool", false, "Tablet reference")),
  };
```

- [ ] **Step 4: Write the shader and the grader**

`src/gpu/shaders/color_grade.hlsl`:
```hlsl
Texture2D<float4> gFrame : register(t0);
Texture3D<float4> gLut : register(t1);
RWTexture2D<float4> gOut : register(u0);
SamplerState gLinear : register(s0);
cbuffer Params : register(b0) {
  float4 gA;  // exposure multiplier, WB r, g, b
  float4 gB;  // lift, gamma, gain, saturation
  float4 gC;  // useLut, lutSize
  uint2 gOutSize;
  uint2 gSrcSize;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= gOutSize.x || id.y >= gOutSize.y) return;
  float4 c = gFrame[id.xy];
  float3 lin = c.rgb;
  if (gC.x > 0.5) {
    float n = gC.y;
    float3 g = pow(saturate(lin), 1.0 / 2.4);
    g = gLut.SampleLevel(gLinear, (g * (n - 1.0) + 0.5) / n, 0).rgb;
    lin = pow(saturate(g), 2.4);
  }
  lin *= gA.x * gA.yzw;
  float3 g2 = pow(saturate(lin), 1.0 / 2.4);
  g2 = gB.z * (g2 + gB.x * (1.0 - g2));
  g2 = pow(saturate(g2), 1.0 / gB.y);
  lin = pow(saturate(g2), 2.4);
  float luma = dot(lin, float3(0.2126, 0.7152, 0.0722));
  lin = max(luma + (lin - luma) * gB.w, 0.0);
  gOut[id.xy] = float4(lin, c.a);
}
```

`src/gpu/color_grader.h`:
```cpp
#pragma once
#include <memory>

#include <nlohmann/json.hpp>

#include "core/lut.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc {
struct GpuLut {
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  int size = 0;
};

struct ColorGrade {
  float exposure = 0, temperature = 0, tint = 0, lift = 0, gamma = 1, gain = 1, saturation = 1;
  std::shared_ptr<const GpuLut> lut;
};
ColorGrade gradeFrom(const nlohmann::json& colorParams);

// The colour stage (spec §5.3): matched LUT, then trims.
class ColorGrader {
 public:
  ColorGrader(GpuDevice& gpu, TexturePool& pool);
  std::shared_ptr<const GpuLut> upload(const Lut3D& lut);
  TextureRef apply(const TextureRef& frame, const ColorGrade& g);
  static bool isIdentity(const ColorGrade& g);

 private:
  struct alignas(16) Params {
    float a[4];
    float b[4];
    float c[4];
    uint32_t outW, outH, srcW, srcH;
  };
  GpuDevice& gpu_;
  TexturePool& pool_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  std::shared_ptr<const GpuLut> identity_;  // bound when no LUT is set (a slot must hold a valid view)
};
}  // namespace bc
```

`src/gpu/color_grader.cpp`:
```cpp
#include "gpu/color_grader.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

#include "color_grade.h"
#include "gpu/converter.h"

namespace bc {
ColorGrade gradeFrom(const nlohmann::json& p) {
  ColorGrade g;
  g.exposure = p.value("exposure", 0.0f);
  g.temperature = p.value("temperature", 0.0f);
  g.tint = p.value("tint", 0.0f);
  g.lift = p.value("lift", 0.0f);
  g.gamma = p.value("gamma", 1.0f);
  g.gain = p.value("gain", 1.0f);
  g.saturation = p.value("saturation", 1.0f);
  return g;
}

bool ColorGrader::isIdentity(const ColorGrade& g) {
  return !g.lut && g.exposure == 0 && g.temperature == 0 && g.tint == 0 && g.lift == 0 && g.gamma == 1 &&
         g.gain == 1 && g.saturation == 1;
}

ColorGrader::ColorGrader(GpuDevice& gpu, TexturePool& pool) : gpu_(gpu), pool_(pool) {
  auto* d = gpu_.device();
  if (FAILED(d->CreateComputeShader(g_color_grade, sizeof(g_color_grade), nullptr, &cs_))) throw std::runtime_error("CreateComputeShader failed");
  D3D11_BUFFER_DESC bd{sizeof(Params), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  if (FAILED(d->CreateBuffer(&bd, nullptr, &cb_))) throw std::runtime_error("CreateBuffer failed");
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(d->CreateSamplerState(&sd, &sampler_))) throw std::runtime_error("CreateSamplerState failed");
  identity_ = upload(Lut3D::identity(2));
}

std::shared_ptr<const GpuLut> ColorGrader::upload(const Lut3D& lut) {
  std::vector<float> rgba(lut.data.size() * 4);
  for (size_t i = 0; i < lut.data.size(); ++i) {
    rgba[i * 4] = lut.data[i].r;
    rgba[i * 4 + 1] = lut.data[i].g;
    rgba[i * 4 + 2] = lut.data[i].b;
    rgba[i * 4 + 3] = 1;
  }
  const UINT n = static_cast<UINT>(lut.size);
  D3D11_TEXTURE3D_DESC d{n, n, n, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_SUBRESOURCE_DATA init{rgba.data(), n * 16, n * n * 16};
  Microsoft::WRL::ComPtr<ID3D11Texture3D> tex;
  if (FAILED(gpu_.device()->CreateTexture3D(&d, &init, &tex))) throw std::runtime_error("CreateTexture3D failed");
  auto out = std::make_shared<GpuLut>();
  out->size = lut.size;
  if (FAILED(gpu_.device()->CreateShaderResourceView(tex.Get(), nullptr, &out->srv))) throw std::runtime_error("LUT SRV failed");
  return out;
}

TextureRef ColorGrader::apply(const TextureRef& frame, const ColorGrade& g) {
  if (isIdentity(g)) return frame;
  auto out = pool_.acquire(frame->width, frame->height, Converter::kLinearFormat,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.a[0] = std::pow(2.0f, g.exposure);
  p.a[1] = 1 + g.temperature / 400.0f;
  p.a[2] = 1 - g.tint / 400.0f;
  p.a[3] = 1 - g.temperature / 400.0f;
  p.b[0] = g.lift;
  p.b[1] = g.gamma;
  p.b[2] = g.gain;
  p.b[3] = g.saturation;
  const auto& lut = g.lut ? g.lut : identity_;
  p.c[0] = g.lut ? 1.0f : 0.0f;
  p.c[1] = static_cast<float>(lut->size);
  p.outW = p.srcW = frame->width;
  p.outH = p.srcH = frame->height;
  auto lock = gpu_.lock();
  auto* ctx = gpu_.context();
  D3D11_MAPPED_SUBRESOURCE m{};
  ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
  std::memcpy(m.pData, &p, sizeof(p));
  ctx->Unmap(cb_.Get(), 0);
  ID3D11ShaderResourceView* srvs[] = {frame->srv.Get(), lut->srv.Get()};
  ID3D11UnorderedAccessView* uav = out->uav.Get();
  ctx->CSSetShader(cs_.Get(), nullptr, 0);
  ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ctx->CSSetSamplers(0, 1, sampler_.GetAddressOf());
  ctx->CSSetShaderResources(0, 2, srvs);
  ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx->Dispatch((p.outW + 7) / 8, (p.outH + 7) / 8, 1);
  ID3D11ShaderResourceView* nulls[2] = {};
  ID3D11UnorderedAccessView* nullUav = nullptr;
  ctx->CSSetShaderResources(0, 2, nulls);
  ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
  return out;
}
}  // namespace bc
```

- [ ] **Step 5: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "ColorGrader*,colour stage*,normalizeStageParams*,stage schemas*,CommandRouter*"`
Expected: all pass (also once with `BC_FORCE_WARP=1`).

- [ ] **Step 6: Commit**

```bash
git add src tests CMakeLists.txt
git commit -m "feat(gpu): colour stage schema (add-in) and the LUT colour grader"
```

### Task 4: Colour stage in the camera, `GpuTools`, and the engine's frame capture

**Files:**
- Create: `src/engine/gpu_tools.h`
- Modify: `src/engine/camera.h/.cpp`, `src/engine/engine.h/.cpp`, `src/engine/engine_main.cpp`, `tests/support/fake_input.h` (gradient option), `tests/engine_test.cpp`, `tests/engine_matte_test.cpp`, `tests/e2e_m1_test.cpp`, `tests/e2e_m2_test.cpp` (constructor change)
- Test: `tests/engine_color_test.cpp`

**Interfaces:**
- Consumes: `ColorGrader`, `ColorGrade`, `gradeFrom`, `GpuLut` (Task 3); `readCube` (Task 1); `StageGuard` (M2); M2's `Camera`/`Engine`.
- Produces:
  - `src/engine/gpu_tools.h`: `struct bc::GpuTools { Converter& conv; Compositor& comp; ColorGrader& grader; };`
  - **Changed constructors:** `bc::Engine(GpuTools, StateStore&, AdapterFactory&)`, `bc::Camera(CameraConfig, AdapterFactory&, GpuTools, StateStore&, SyncPolicy)`.
  - `TextureRef Camera::tickRaw() const` (this tick's frame **before** any stage; the slate when there is no picture), `StageGuard& Camera::colorGuard()`, `FeedPhase Camera::tickPhase() const`.
  - `std::map<std::string, std::vector<CpuImage>> Engine::capture(const std::vector<std::string>& cameraIds, int frames = 30, int everyTicks = 2, uint32_t w = 256, uint32_t h = 144, std::chrono::milliseconds timeout = 3000ms)` — throws `std::invalid_argument` naming the camera when an id is unknown or a camera's phase is not `live` during the capture, `std::runtime_error` on timeout.
  - State: `/cameras/<id>/stages/color/{state,error}`; a LUT that cannot be read → `{code: "color-lut", message}` and the camera runs uncorrected (never black).
  - Fake input option `"gradient": true` — pixel value = colour × (0.3 + 0.7 × x / width) (so regions are not flat).

Colour stage in `beginTick` (spec §3.2): after conversion, when `color.enabled` and the guard allows, `tickFrame_ = grader.apply(raw, grade)` (budget 4 ms; ponytail: fixed budget, share by camera count if many cameras grade). The slate is never graded. A repeated frame reuses the graded texture unless the parameters changed.

- [ ] **Step 1: Test helper change and the failing tests**

In `tests/support/fake_input.h`, give `FakeInput` a `bool gradient = false;` public member and use it when filling pixels:
```cpp
          for (uint32_t x = 0; x < w; ++x) {
            const float k = gradient ? 0.3f + 0.7f * static_cast<float>(x) / static_cast<float>(w) : 1.0f;
            for (uint32_t y = 0; y < h; ++y) {
              uint8_t* p = &px[(static_cast<size_t>(y) * w + x) * 4];
              p[0] = static_cast<uint8_t>(color_[0] * k);
              p[1] = static_cast<uint8_t>(color_[1] * k);
              p[2] = static_cast<uint8_t>(color_[2] * k);
              p[3] = color_[3];
            }
          }
```
(replacing the solid fill loop), and in `FakeFactory::makeInput` set `in->gradient = p.value("gradient", false);` before returning.

`tests/engine_color_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <thread>

#include "core/lut.h"
#include "core/stage_params.h"
#include "core/state_store.h"
#include "engine/engine.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "support/fake_input.h"
#include "support/temp_dir.h"

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};
  bc::ColorGrader grader{*gpu, pool};
  bc::StateStore state;
  bc::test::FakeFactory factory{conv};
  bc::Engine engine{bc::GpuTools{conv, comp, grader}, state, factory};
  ~Rig() { engine.stop(); }
  json at(const std::string& p) {
    auto s = state.snapshot();
    auto ptr = json::json_pointer(p);
    return s.contains(ptr) ? s.at(ptr) : json();
  }
};

bc::CameraConfig cam(const std::string& id, json color, json in = json::object()) {
  bc::CameraConfig c;
  c.id = id;
  c.name = id;
  c.input.kind = "fake";
  c.input.params = in;
  c.input.params["tag"] = id;
  c.outputs = {{"o-" + id, "fake", false, true, json::object()}};
  c.stages = {{"color", bc::normalizeStageParams("color", color)}};
  return c;
}
}  // namespace

TEST_CASE("Colour stage: exposure brightens the output") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"exposure", 1}}, {{"bgra", {131, 131, 131, 255}}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  auto px = r.factory.output("o-a")->seen().back().firstPixel;
  CHECK(std::abs(px[1] - 174) <= 3);  // 131 ≈ 0.2 linear → ×2 → 174
  CHECK(r.at("/cameras/a/stages/color/state") == "active");
}

TEST_CASE("Colour stage: a missing LUT file reports color-lut and keeps the trims") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"exposure", 1}, {"lut", "C:/nope/missing.cube"}}, {{"bgra", {131, 131, 131, 255}}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1200ms);
  CHECK(r.at("/cameras/a/stages/color/error/code") == "color-lut");
  auto px = r.factory.output("o-a")->seen().back().firstPixel;
  CHECK(std::abs(px[1] - 174) <= 3);
}

TEST_CASE("Colour stage: a LUT file is applied") {
  Rig r;
  bc::test::TempDir dir;
  auto lut = bc::Lut3D::identity(2);
  for (auto& e : lut.data) e = {1, 0, 0};
  bc::writeCube(dir.path / "red.cube", lut, "red");
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"lut", (dir.path / "red.cube").string()}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  auto px = r.factory.output("o-a")->seen().back().firstPixel;
  CHECK(px[2] >= 253);
  CHECK(px[1] <= 2);
}

TEST_CASE("Engine::capture returns frames before stages, and refuses a camera with no picture") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("a", {{"enabled", true}, {"exposure", 2}}, {{"bgra", {20, 40, 200, 255}}}));
  show.cameras.push_back(cam("b", json::object()));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  auto got = r.engine.capture({"a"}, 10, 2, 256, 144);
  REQUIRE(got["a"].size() == 10);
  CHECK(got["a"][0].width == 256);
  CHECK(std::abs(got["a"][0].bgra[2] - 200) <= 3);  // raw, not the +2 EV grade
  r.factory.input("b")->paused = true;
  std::this_thread::sleep_for(6000ms);  // past the 5 s hold → slate
  try {
    r.engine.capture({"a", "b"});
    FAIL("capture should refuse camera b");
  } catch (const std::invalid_argument& e) {
    CHECK(std::string(e.what()).find("b") != std::string::npos);
  }
  CHECK_THROWS_AS(r.engine.capture({"zzz"}), std::invalid_argument);
}
```

Update the four existing rigs for `GpuTools`: add `#include "gpu/color_grader.h"` and `#include "engine/gpu_tools.h"`, a `bc::ColorGrader grader{*gpu, pool};` member (or local) after `comp`, and construct the engine with `bc::GpuTools{conv, comp, grader}` in place of `conv, comp`. Same in `engine_main.cpp` (`bc::ColorGrader grader(*gpu, pool);`).

Add `tests/engine_color_test.cpp` to `bc_tests`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Colour stage*,Engine::capture*"`
Expected: build FAILS (`engine/gpu_tools.h` missing).

- [ ] **Step 3: `GpuTools`**

`src/engine/gpu_tools.h`:
```cpp
#pragma once
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/converter.h"

namespace bc {
// The GPU helpers every camera stage uses, passed as one bundle.
struct GpuTools {
  Converter& conv;
  Compositor& comp;
  ColorGrader& grader;
};
}  // namespace bc
```

- [ ] **Step 4: The colour stage in the camera**

`src/engine/camera.h`:
- constructor `Camera(CameraConfig cfg, AdapterFactory& factory, GpuTools tools, StateStore& state, SyncPolicy policy);` (include `engine/gpu_tools.h`); replace the `Converter& conv_; Compositor& comp_;` members with `GpuTools tools_;` and use `tools_.conv` / `tools_.comp` wherever `conv_` / `comp_` appeared (mechanical rename across `camera.cpp`).
- public: `TextureRef tickRaw() const { return tickRaw_; }`, `StageGuard& colorGuard() { return colorGuard_; }`, `FeedPhase tickPhase() const { return tickPhase_; }`.
- private: `nlohmann::json colorParams_; ColorGrade grade_; std::string lutPath_; std::optional<ErrorInfo> lutError_; StageGuard colorGuard_; TextureRef tickRaw_; TextureRef lastGraded_; bool gradeDirty_ = true;`

`src/engine/camera.cpp`:
- at the top of `loadStageParams()`:
```cpp
  colorParams_ = normalizeStageParams("color", cfg_.stages.value("color", nlohmann::json::object()));
  auto lutKeep = grade_.lut;
  grade_ = gradeFrom(colorParams_);
  const std::string lut = colorParams_.value("lut", std::string());
  if (lut.empty()) {
    lutPath_.clear();
    lutError_.reset();
    lutKeep.reset();
  } else if (lut != lutPath_) {
    lutPath_ = lut;
    lutError_.reset();
    lutKeep.reset();
    try {
      lutKeep = tools_.grader.upload(readCube(std::filesystem::u8path(lut)));
    } catch (const std::exception& e) {
      lutError_ = ErrorInfo{"color-lut", std::string("the colour LUT cannot be read; the camera runs without the correction: ") + e.what()};
    }
  }
  grade_.lut = lutKeep;
  gradeDirty_ = true;
```
(includes: `core/lut.h`, `gpu/color_grader.h`).
- also reset `colorGuard_` where M2 resets the other guards on a stages change.
- in `beginTick`, after the `if (pick.frame) {...} else {...}` block, replace `tickFrame_ = lastConverted_ ? lastConverted_ : slate; return tickFrame_;` with:
```cpp
  tickRaw_ = lastConverted_ ? lastConverted_ : slate;
  tickFrame_ = tickRaw_;
  const bool colorOn = colorParams_.value("enabled", false);
  if (lastConverted_ && colorOn && colorGuard_.shouldRun(now)) {
    if (!pick.fresh && !gradeDirty_ && lastGraded_) {
      tickFrame_ = lastGraded_;
    } else {
      const auto t0 = Clock::now();
      try {
        lastGraded_ = tools_.grader.apply(tickRaw_, grade_);
        gradeDirty_ = false;
        tickFrame_ = lastGraded_;
        colorGuard_.succeeded(Clock::now() - t0, std::chrono::milliseconds(4), now);
      } catch (const std::exception& e) {
        colorGuard_.failed(e.what(), now);
      }
    }
  }
  return tickFrame_;
```
- in `publishStatus()`, add:
```cpp
  const bool colorOn = colorParams_.value("enabled", false);
  state_.set(path("/stages/color/state"), colorGuard_.stateName(colorOn));
  auto colorErr = colorGuard_.error() ? colorGuard_.error() : lutError_;
  state_.set(path("/stages/color/error"), errorToJson(colorOn ? colorErr : std::nullopt));
```

- [ ] **Step 5: Capture in the engine**

`src/engine/engine.h`: constructor `Engine(GpuTools tools, StateStore& state, AdapterFactory& factory);` with member `GpuTools tools_;` replacing `conv_`/`comp_` (rename uses: `tools_.conv`, `tools_.comp`). Add:
```cpp
  std::map<std::string, std::vector<CpuImage>> capture(const std::vector<std::string>& cameraIds, int frames = 30,
                                                       int everyTicks = 2, uint32_t w = 256, uint32_t h = 144,
                                                       std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));
 private:
  struct CaptureJob {
    std::vector<std::string> ids;
    int frames, every;
    uint32_t w, h;
    std::map<std::string, std::vector<CpuImage>> got;
    std::string error;
    bool done = false;
  };
  std::shared_ptr<CaptureJob> capture_;  // guarded by mutex_
  std::condition_variable captureCv_;
```
(include `<condition_variable>`).

`src/engine/engine.cpp`:
```cpp
std::map<std::string, std::vector<CpuImage>> Engine::capture(const std::vector<std::string>& ids, int frames, int every,
                                                             uint32_t w, uint32_t h, std::chrono::milliseconds timeout) {
  auto job = std::make_shared<CaptureJob>();
  job->ids = ids;
  job->frames = frames;
  job->every = every;
  job->w = w;
  job->h = h;
  std::unique_lock l(mutex_);
  for (auto& id : ids) {
    if (std::none_of(cameras_.begin(), cameras_.end(), [&](auto& c) { return c->config().id == id; })) {
      throw std::invalid_argument("no camera " + id);
    }
  }
  if (capture_) throw std::runtime_error("another capture is running");
  capture_ = job;
  const bool finished = captureCv_.wait_for(l, timeout, [&] { return job->done; });
  capture_.reset();
  if (!finished) throw std::runtime_error("capture timed out");
  if (!job->error.empty()) throw std::invalid_argument(job->error);
  return std::move(job->got);
}
```
and in `run()`, right after the half-1 loop (still under the lock):
```cpp
      if (capture_ && !capture_->done && index % static_cast<uint64_t>(capture_->every) == 0) {
        auto& job = *capture_;
        for (auto& cam : cameras_) {
          const auto& id = cam->config().id;
          if (std::find(job.ids.begin(), job.ids.end(), id) == job.ids.end()) continue;
          if (cam->tickPhase() != FeedPhase::Live) {
            job.error = "camera \"" + cam->config().name + "\" (" + id + ") has no picture";
            break;
          }
          auto& v = job.got[id];
          if (static_cast<int>(v.size()) < job.frames) {
            v.push_back(tools_.conv.readback(*tools_.conv.toBgra8(*cam->tickRaw(), job.w, job.h)));
          }
        }
        bool all = job.error.empty();
        for (auto& id : job.ids) all = all && static_cast<int>(job.got[id].size()) >= job.frames;
        if (all || !job.error.empty()) {
          job.done = true;
          captureCv_.notify_all();
        }
      }
```
Note `capture()` waits on `captureCv_` with `mutex_` — the same lock the tick holds while ticking, which `wait_for` releases while waiting. Never call `capture()` from the tick thread.

- [ ] **Step 6: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Colour stage*,Engine*,Matte*"`
Expected: the 4 new cases and every M1/M2 engine and matte case pass. Then the whole suite.

- [ ] **Step 7: Commit**

```bash
git add src tests CMakeLists.txt
git commit -m "feat(engine): colour stage before matte, GpuTools bundle and frame capture for matching"
```

### Task 5: Chart detection (OpenCV `mcc`, optional)

**Files:**
- Create: `src/engine/chart_detector.h`, `src/engine/chart_detector.cpp`
- Modify: `vcpkg.json` (`opencv4` with the `contrib` feature), `CMakeLists.txt` (`BC_WITH_CHART_DETECT`), `tests/support/requires.h/.cpp` (`Req::ChartDetect`)
- Test: `tests/chart_detector_test.cpp`

**Interfaces:**
- Consumes: `ChartQuad`, `chartToPicture`, `sampleChartRaw`, `chartUpright`, `validQuad` (Task 2); `CpuImage`.
- Produces: `bool bc::chartDetectionAvailable()`; `std::optional<ChartQuad> bc::findChart(const CpuImage& frame)` — the grid corners in chart order, checked upright; `Req::ChartDetect` ("chart detection is not in this build (OpenCV mcc missing)").

Rules: detection runs on the CPU off the tick thread (the service calls it); one `CCheckerDetector::process(image, MCC24, 1)` per call. `getColorCharts()` gives four points per patch in patch order; the centres of patches 1, 6, 24 and 19 (dark skin, bluish green, black, white) sit at chart coordinates (1/12, 1/8), (11/12, 1/8), (11/12, 7/8), (1/12, 7/8), so a homography through them extends to the grid corners at (−0.1, −1/6) … (1.1, 7/6) in that inner frame. If that quad is not upright, the turned-round quad (mcc numbering from the other end) is tried. Without `mcc` the functions compile and `findChart` returns `std::nullopt`.

- [ ] **Step 1: Dependency and CMake**

`vcpkg.json` — add to `dependencies`:
```json
{ "name": "opencv4", "default-features": false, "features": ["contrib"] }
```
`CMakeLists.txt`, next to the other `BC_WITH_*` options:
```cmake
find_package(OpenCV CONFIG QUIET COMPONENTS core imgproc mcc)
if(OpenCV_FOUND)
  set(BC_WITH_CHART_DETECT ON)
else()
  set(BC_WITH_CHART_DETECT OFF)
  message(STATUS "Chart detection OFF: OpenCV with the mcc module not found (manual corners still work)")
endif()
message(STATUS "BC_WITH_CHART_DETECT=${BC_WITH_CHART_DETECT}")
target_compile_definitions(bc_engine PUBLIC BC_WITH_CHART_DETECT=$<BOOL:${BC_WITH_CHART_DETECT}>)
if(BC_WITH_CHART_DETECT)
  target_link_libraries(bc_engine PUBLIC opencv_core opencv_imgproc opencv_mcc)
endif()
```
and add `src/engine/chart_detector.cpp` to `bc_engine`. The configure line must print `BC_WITH_CHART_DETECT=...`; report it.

- [ ] **Step 2: Write the failing tests**

`tests/chart_detector_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <algorithm>

#include "core/chart.h"
#include "engine/chart_detector.h"
#include "support/chart_image.h"
#include "support/requires.h"

TEST_CASE("findChart finds a tilted chart, upright and upside down" BC_REQUIRES(bc::test::Req::ChartDetect)) {
  const bc::ChartQuad q{{{{0.30, 0.25}, {0.72, 0.22}, {0.74, 0.70}, {0.28, 0.72}}}};
  for (bool flipped : {false, true}) {
    auto colors = bc::colorCheckerSrgb();
    if (flipped) std::reverse(colors.begin(), colors.end());
    auto img = bc::test::renderChart(960, 540, q, colors);
    auto found = bc::findChart(img);
    REQUIRE(found);
    auto got = bc::sampleChart({img}, *found);
    for (size_t i = 0; i < 24; ++i) CHECK(std::abs(bc::toGamma(got[i].g) * 255 - bc::colorCheckerSrgb()[i][1]) <= 4);
  }
}

TEST_CASE("findChart returns nothing for a picture without a chart") {
  bc::CpuImage grey{960, 540, std::vector<uint8_t>(960 * 540 * 4, 90)};
  CHECK_FALSE(bc::findChart(grey));
}
```
`requires`: `ChartDetect` → `reason = "chart detection is not in this build (OpenCV mcc missing)"; return !bc::chartDetectionAvailable();` (include `engine/chart_detector.h`).

- [ ] **Step 3: Implement**

`src/engine/chart_detector.h`:
```cpp
#pragma once
#include <optional>

#include "core/chart.h"
#include "gpu/converter.h"

namespace bc {
bool chartDetectionAvailable();
// Finds a ColorChecker 24 in the frame; corners come back in chart order (spec §5.3).
std::optional<ChartQuad> findChart(const CpuImage& frame);
}  // namespace bc
```

`src/engine/chart_detector.cpp`:
```cpp
#include "engine/chart_detector.h"

#include <array>

#if BC_WITH_CHART_DETECT
#include <opencv2/imgproc.hpp>
#include <opencv2/mcc.hpp>
#endif

namespace bc {
bool chartDetectionAvailable() { return BC_WITH_CHART_DETECT != 0; }

std::optional<ChartQuad> findChart(const CpuImage& frame) {
#if BC_WITH_CHART_DETECT
  cv::Mat bgra(static_cast<int>(frame.height), static_cast<int>(frame.width), CV_8UC4, const_cast<uint8_t*>(frame.bgra.data()));
  cv::Mat bgr;
  cv::cvtColor(bgra, bgr, cv::COLOR_BGRA2BGR);
  auto detector = cv::mcc::CCheckerDetector::create();
  if (!detector->process(bgr, cv::mcc::MCC24, 1)) return std::nullopt;
  auto checker = detector->getBestColorChecker();
  if (!checker) return std::nullopt;
  const auto pts = checker->getColorCharts();  // 4 points per patch, patch order
  if (pts.size() != 96) return std::nullopt;
  auto centre = [&](int k) {
    double x = 0, y = 0;
    for (int i = 0; i < 4; ++i) {
      x += pts[static_cast<size_t>(k * 4 + i)].x;
      y += pts[static_cast<size_t>(k * 4 + i)].y;
    }
    return std::array<double, 2>{x / 4 / frame.width, y / 4 / frame.height};
  };
  // Corner patch centres → the outer corners of the 6x4 grid.
  const auto inner = chartToPicture(ChartQuad{{centre(0), centre(5), centre(23), centre(18)}});
  ChartQuad q{{inner.map(-0.1, -1.0 / 6), inner.map(1.1, -1.0 / 6), inner.map(1.1, 7.0 / 6), inner.map(-0.1, 7.0 / 6)}};
  for (int turn = 0; turn < 2; ++turn) {
    if (validQuad(q) && chartUpright(sampleChartRaw({frame}, q))) return q;
    q.c = {q.c[2], q.c[3], q.c[0], q.c[1]};  // mcc numbered the patches from the other end
  }
  return std::nullopt;
#else
  (void)frame;
  return std::nullopt;
#endif
}
}  // namespace bc
```

ponytail: the MCC detector runs per call (~50–150 ms at 960×540); the UI polls it once a second while calibrating, never per tick.

- [ ] **Step 4: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "findChart*"` — Expected: both pass with `BC_WITH_CHART_DETECT=ON`; with it OFF the first is skipped with the reason and the second passes.

- [ ] **Step 5: Commit**

```bash
git add vcpkg.json CMakeLists.txt src/engine/chart_detector.h src/engine/chart_detector.cpp tests
git commit -m "feat(engine): optional ColorChecker detection with OpenCV mcc"
```

### Task 6: Calibration service and the commands (find, calibrate, undo, clear)

**Files:**
- Create: `src/engine/color_calibration_service.h`, `src/engine/color_calibration_service.cpp`
- Modify: `src/engine/command_router.h/.cpp` (commands + `Effects` additions), `src/engine/engine_main.cpp` (wire the service), `tests/command_router_test.cpp` (`FakeFx` additions + cases), `CMakeLists.txt`
- Test: `tests/color_calibration_test.cpp`, `tests/command_router_test.cpp`

**Interfaces:**
- Consumes: `Engine::capture` (Task 4), `sampleChart`, `ChartQuad`, `validQuad`, `colorCheckerReference` (Task 2), `solveCalibration`, `calibrationRating` (Task 2), `findChart`, `chartDetectionAvailable` (Task 5), `writeCube`, `Lut3D` (Task 1), `normalizeStageParams` (M2/Task 3).
- Produces:
  - `bc::ColorCalibrationService(Engine&, std::filesystem::path lutDir)`:
    - `nlohmann::json find(const std::string& cameraId)` → `{"available": bool, "found": bool, "corners": [[x,y]×4]}` (`corners` only when found).
    - `struct Result { std::string lutPath; double deltaEBefore, deltaE; std::string rating; bool reference; }`, `Result calibrate(const std::string& cameraId, const std::string& chart /* "card" | "tablet" */, const ChartQuad& q)` — throws `std::invalid_argument` with an operator-readable message.
    - `void forget(const std::string& cameraId)` — if this camera is the tablet reference, the reference is dropped.
  - `CommandRouter::Effects` additions: `virtual nlohmann::json findChart(const std::string& cameraId) = 0;`, `virtual nlohmann::json calibrateColor(const std::string& cameraId, const std::string& chart, const ChartQuad& q) = 0;` (returns `{lutPath, deltaEBefore, deltaE, rating, reference}`), `virtual void forgetColor(const std::string& cameraId) = 0;`.
  - Commands:
    | cmd | args | effect on `color` params | result |
    |---|---|---|---|
    | `findChart` | `{cameraId}` | — | `{available, found, corners?}` |
    | `calibrateColor` | `{cameraId, chart: "card"\|"tablet", corners: [[x,y]×4]}` | `previousLut = lut`, `lut = <new file>`, `enabled = true`, `chart`, `deltaE`, `deltaEBefore`, `calibratedAt = <unix seconds>`, `reference` | `{deltaEBefore, deltaE, rating, reference}` |
    | `undoColor` | `{cameraId}` | swap `lut` ↔ `previousLut`; `invalid` "nothing to undo" when `previousLut` is empty | `{}` |
    | `clearColor` | `{cameraId}` | colour params back to defaults except `enabled` (kept) and `previousLut = <old lut>`; the service forgets a tablet reference held by this camera | `{}` |
  - The `color` schema (Task 3) gains the hidden field `reference` (bool, default false).

Rules:
- The router validates before any capture: camera exists, `chart` is `card` or `tablet`, `corners` is four `[x, y]` number pairs forming a `validQuad`. It does **not** hold the show lock while capturing and solving (~1 s); it re-takes the lock to store the result and answers `not-found` if the camera was removed meanwhile. `removeCamera` also calls `fx_.forgetColor(id)`.
- Capture: `engine.capture({id}, 15, 2, 960, 540)`; `find` uses one frame.
- Card: target = `colorCheckerReference()`. Tablet: if `<lutDir>\tablet-reference.json` exists, target = its colours; else the measured colours become the reference (file `{"cameraId": id, "colors": [[r,g,b]×24]}`), this camera gets an identity LUT, `deltaE = deltaEBefore = 0`, `reference = true`.
- LUT files: `<lutDir>\<cameraId>-<YYYYMMDD-HHMMSS>.cube` (add `-2`, `-3` … if the name exists).

- [ ] **Step 1: Write the failing tests**

`tests/color_calibration_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <thread>

#include "core/calibration.h"
#include "core/lut.h"
#include "core/stage_params.h"
#include "core/state_store.h"
#include "engine/color_calibration_service.h"
#include "engine/engine.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "support/fake_input.h"
#include "support/temp_dir.h"

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
const bc::ChartQuad kQuad{{{{0.25, 0.22}, {0.75, 0.22}, {0.75, 0.78}, {0.25, 0.78}}}};

struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};
  bc::ColorGrader grader{*gpu, pool};
  bc::StateStore state;
  bc::test::FakeFactory factory{conv};
  bc::Engine engine{bc::GpuTools{conv, comp, grader}, state, factory};
  bc::test::TempDir dir;
  bc::ColorCalibrationService svc{engine, dir.path / "luts"};
  ~Rig() { engine.stop(); }
};

bc::CameraConfig chartCam(const std::string& id, std::vector<float> cast) {
  bc::CameraConfig c;
  c.id = id;
  c.name = id;
  c.input.kind = "fake";
  c.input.params = {{"tag", id}, {"width", 960}, {"height", 540}, {"chart", true}, {"cast", cast}};
  c.outputs = {{"o-" + id, "fake", false, true, json::object()}};
  return c;
}

bc::Show showOf(std::vector<bc::CameraConfig> cams) {
  bc::Show s;
  s.settings.width = 960;
  s.settings.height = 540;
  s.cameras = std::move(cams);
  return s;
}
}  // namespace

TEST_CASE("Calibration: a printed card corrects a cast camera") {
  Rig r;
  r.engine.apply(showOf({chartCam("a", {1.25f, 1.0f, 0.7f})}));
  r.engine.start();
  std::this_thread::sleep_for(400ms);
  auto res = r.svc.calibrate("a", "card", kQuad);
  MESSAGE("card: before " << res.deltaEBefore << ", after " << res.deltaE);
  CHECK(res.deltaEBefore > 5);
  CHECK(res.deltaE < 2);
  CHECK(res.rating == "good");
  CHECK_FALSE(res.reference);
  CHECK(bc::readCube(std::filesystem::u8path(res.lutPath)).size == 33);
}

TEST_CASE("Calibration: tablet mode matches the second camera to the first") {
  Rig r;
  r.engine.apply(showOf({chartCam("a", {1.2f, 1.0f, 0.8f}), chartCam("b", {0.8f, 1.0f, 1.25f})}));
  r.engine.start();
  std::this_thread::sleep_for(400ms);
  auto first = r.svc.calibrate("a", "tablet", kQuad);
  CHECK(first.reference);
  CHECK(first.deltaE == 0);
  auto second = r.svc.calibrate("b", "tablet", kQuad);
  CHECK_FALSE(second.reference);
  MESSAGE("tablet: before " << second.deltaEBefore << ", after " << second.deltaE);
  CHECK(second.deltaE < 2);
  r.svc.forget("a");  // the reference camera is cleared
  auto again = r.svc.calibrate("b", "tablet", kQuad);
  CHECK(again.reference);  // b is the new reference
}

TEST_CASE("Calibration: wrong corners and a camera without picture are refused") {
  Rig r;
  r.engine.apply(showOf({chartCam("a", {1, 1, 1})}));
  r.engine.start();
  std::this_thread::sleep_for(400ms);
  bc::ChartQuad off{{{{0.01, 0.01}, {0.15, 0.01}, {0.15, 0.1}, {0.01, 0.1}}}};
  CHECK_THROWS_WITH_AS(r.svc.calibrate("a", "card", off), doctest::Contains("not on the chart"), std::invalid_argument);
  r.factory.input("a")->paused = true;
  std::this_thread::sleep_for(6000ms);
  CHECK_THROWS_WITH_AS(r.svc.calibrate("a", "card", kQuad), doctest::Contains("no picture"), std::invalid_argument);
}
```

`tests/support/fake_input.h` — add a chart option: `FakeInput` gets `std::vector<uint8_t> image;` (public); when non-empty and its size matches `w*h*4`, the frame is uploaded from `image` instead of the solid/gradient fill. In `FakeFactory::makeInput`, when `p.value("chart", false)`:
```cpp
    auto cast = p.value("cast", std::vector<float>{1, 1, 1});
    const bc::ChartQuad q{{{{0.25, 0.22}, {0.75, 0.22}, {0.75, 0.78}, {0.25, 0.78}}}};
    in->image = bc::test::renderChart(in->width, in->height, q, bc::colorCheckerSrgb(), {cast[0], cast[1], cast[2]}).bgra;
```
(include `support/chart_image.h`).

Append to `tests/command_router_test.cpp` — first add to `FakeFx`:
```cpp
  std::string lastChart;
  std::vector<std::string> forgotten;
  json findChart(const std::string&) override { return {{"available", false}, {"found", false}}; }
  json calibrateColor(const std::string&, const std::string& chart, const bc::ChartQuad&) override {
    lastChart = chart;
    return {{"lutPath", "C:/luts/x.cube"}, {"deltaEBefore", 7.4}, {"deltaE", 1.1}, {"rating", "good"}, {"reference", false}};
  }
  void forgetColor(const std::string& id) override { forgotten.push_back(id); }
```
then:
```cpp
TEST_CASE("CommandRouter: calibrateColor stores the result; undo, clear and removal") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto cam = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "Cam"}, {"input", kSrt}}}})["result"]["id"];
  const json corners = json::array({json::array({0.25, 0.22}), json::array({0.75, 0.22}), json::array({0.75, 0.78}), json::array({0.25, 0.78})});
  auto res = call(r, {{"cmd", "calibrateColor"}, {"args", {{"cameraId", cam}, {"chart", "card"}, {"corners", corners}}}});
  REQUIRE(res["ok"] == true);
  CHECK(res["result"]["rating"] == "good");
  auto c = r.show().cameras[0].stages["color"];
  CHECK(c["enabled"] == true);
  CHECK(c["lut"] == "C:/luts/x.cube");
  CHECK(c["deltaE"] == 1.1);
  CHECK(c["calibratedAt"].get<double>() > 1.7e9);
  CHECK(call(r, {{"cmd", "calibrateColor"}, {"args", {{"cameraId", cam}, {"chart", "poster"}, {"corners", corners}}}})["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "calibrateColor"}, {"args", {{"cameraId", cam}, {"chart", "card"}, {"corners", json::array()}}}})["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "undoColor"}, {"args", {{"cameraId", cam}}}})["error"]["code"] == "invalid");  // previousLut was empty
  REQUIRE(call(r, {{"cmd", "clearColor"}, {"args", {{"cameraId", cam}}}})["ok"] == true);
  CHECK(r.show().cameras[0].stages["color"]["lut"] == "");
  CHECK(r.show().cameras[0].stages["color"]["previousLut"] == "C:/luts/x.cube");
  REQUIRE(call(r, {{"cmd", "undoColor"}, {"args", {{"cameraId", cam}}}})["ok"] == true);
  CHECK(r.show().cameras[0].stages["color"]["lut"] == "C:/luts/x.cube");
  call(r, {{"cmd", "removeCamera"}, {"args", {{"id", cam}}}});
  CHECK(fx.forgotten.size() == 2);  // clearColor and removeCamera
}
```

Add `tests/color_calibration_test.cpp` and the service source to the build.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Calibration*,CommandRouter*"` — Expected: build FAILS.

- [ ] **Step 3: The service**

`src/engine/color_calibration_service.h`:
```cpp
#pragma once
#include <filesystem>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "core/chart.h"
#include "engine/engine.h"

namespace bc {
// One-time chart calibration (spec §5.3): capture, sample, solve, write the LUT.
class ColorCalibrationService {
 public:
  struct Result {
    std::string lutPath;
    double deltaEBefore = 0;
    double deltaE = 0;
    std::string rating;
    bool reference = false;
  };
  ColorCalibrationService(Engine& engine, std::filesystem::path lutDir);
  nlohmann::json find(const std::string& cameraId);
  Result calibrate(const std::string& cameraId, const std::string& chart, const ChartQuad& q);
  void forget(const std::string& cameraId);

 private:
  std::filesystem::path newLutPath(const std::string& cameraId) const;
  Engine& engine_;
  std::filesystem::path lutDir_;
  std::mutex m_;  // one calibration at a time; guards the tablet reference file
};
}  // namespace bc
```

`src/engine/color_calibration_service.cpp`:
```cpp
#include "engine/color_calibration_service.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <stdexcept>

#include "core/calibration.h"
#include "core/lut.h"
#include "engine/chart_detector.h"

namespace bc {
namespace {
using nlohmann::json;

json colorsToJson(const ChartColors& c) {
  json out = json::array();
  for (auto& v : c) out.push_back({v.r, v.g, v.b});
  return out;
}
ChartColors colorsFromJson(const json& j) {
  if (!j.is_array() || j.size() != 24) throw std::runtime_error("bad tablet reference file");
  ChartColors out;
  for (size_t i = 0; i < 24; ++i) out[i] = {j[i][0].get<float>(), j[i][1].get<float>(), j[i][2].get<float>()};
  return out;
}
}  // namespace

ColorCalibrationService::ColorCalibrationService(Engine& engine, std::filesystem::path lutDir)
    : engine_(engine), lutDir_(std::move(lutDir)) {}

std::filesystem::path ColorCalibrationService::newLutPath(const std::string& cameraId) const {
  const auto t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
  auto p = lutDir_ / (cameraId + "-" + stamp + ".cube");
  for (int n = 2; std::filesystem::exists(p); ++n) p = lutDir_ / (cameraId + "-" + stamp + "-" + std::to_string(n) + ".cube");
  return p;
}

json ColorCalibrationService::find(const std::string& cameraId) {
  if (!chartDetectionAvailable()) return {{"available", false}, {"found", false}};
  auto frames = engine_.capture({cameraId}, 1, 1, 960, 540);
  auto q = findChart(frames.at(cameraId).front());
  if (!q) return {{"available", true}, {"found", false}};
  json corners = json::array();
  for (auto& c : q->c) corners.push_back({c[0], c[1]});
  return {{"available", true}, {"found", true}, {"corners", corners}};
}

ColorCalibrationService::Result ColorCalibrationService::calibrate(const std::string& cameraId, const std::string& chart,
                                                                   const ChartQuad& q) {
  std::lock_guard l(m_);
  if (!validQuad(q)) throw std::invalid_argument("place the four corners on the chart");
  auto frames = engine_.capture({cameraId}, 15, 2, 960, 540);
  const ChartColors measured = sampleChart(frames.at(cameraId), q);
  std::filesystem::create_directories(lutDir_);
  Result r;
  ChartColors target = colorCheckerReference();
  if (chart == "tablet") {
    const auto refFile = lutDir_ / "tablet-reference.json";
    if (!std::filesystem::exists(refFile)) {
      std::ofstream(refFile) << json{{"cameraId", cameraId}, {"colors", colorsToJson(measured)}}.dump(2);
      const auto path = newLutPath(cameraId);
      writeCube(path, Lut3D::identity(33), "BreadCamera tablet reference");
      return {path.u8string(), 0, 0, "good", true};
    }
    target = colorsFromJson(json::parse(std::ifstream(refFile))["colors"]);
  }
  const auto cal = solveCalibration(measured, target);
  const auto path = newLutPath(cameraId);
  writeCube(path, cal.lut, "BreadCamera chart calibration");
  r.lutPath = path.u8string();
  r.deltaEBefore = cal.deltaEBefore;
  r.deltaE = cal.deltaEAfter;
  r.rating = std::string(calibrationRating(cal.deltaEAfter));
  return r;
}

void ColorCalibrationService::forget(const std::string& cameraId) {
  std::lock_guard l(m_);
  const auto refFile = lutDir_ / "tablet-reference.json";
  std::error_code ec;
  if (!std::filesystem::exists(refFile, ec)) return;
  try {
    if (json::parse(std::ifstream(refFile)).value("cameraId", std::string()) == cameraId) std::filesystem::remove(refFile, ec);
  } catch (const json::exception&) {
    std::filesystem::remove(refFile, ec);  // unreadable: drop it rather than match to garbage
  }
}
}  // namespace bc
```
Note `Engine::capture` throws `std::invalid_argument("camera \"…\" (id) has no picture")` (Task 4), which reaches the operator unchanged.

- [ ] **Step 4: The commands**

`src/engine/command_router.h`: add the three `Effects` methods above (include `core/chart.h`).

`src/engine/command_router.cpp` — in the anonymous namespace:
```cpp
ChartQuad quadFrom(const json& args) {
  const auto& c = args.contains("corners") ? args["corners"] : json();
  if (!c.is_array() || c.size() != 4) throw CmdError("invalid", "corners must be four [x, y] points");
  ChartQuad q;
  for (size_t i = 0; i < 4; ++i) {
    if (!c[i].is_array() || c[i].size() != 2 || !c[i][0].is_number() || !c[i][1].is_number()) {
      throw CmdError("invalid", "corners must be four [x, y] points");
    }
    q.c[i] = {c[i][0].get<double>(), c[i][1].get<double>()};
  }
  if (!validQuad(q)) throw CmdError("invalid", "place the four corners on the chart");
  return q;
}
```
In `dispatch`, before the `std::lock_guard l(m_);` line:
```cpp
  if (cmd == "findChart" || cmd == "calibrateColor") {
    const auto camId = str(args, "cameraId");
    {
      std::lock_guard l(m_);
      if (!show_.findCamera(camId)) throw CmdError("not-found", "no camera " + camId);
    }
    if (cmd == "findChart") {
      try {
        return fx_.findChart(camId);
      } catch (const std::exception& e) {
        throw CmdError("invalid", e.what());
      }
    }
    const auto chart = args.value("chart", std::string("card"));
    if (chart != "card" && chart != "tablet") throw CmdError("invalid", "chart must be card or tablet");
    const auto q = quadFrom(args);
    json res;
    try {
      res = fx_.calibrateColor(camId, chart, q);  // ~1 s, without the show lock
    } catch (const std::invalid_argument& e) {
      throw CmdError("invalid", e.what());
    }
    std::lock_guard l(m_);
    Show next = show_;
    auto* c = next.findCamera(camId);
    if (!c) throw CmdError("not-found", "camera " + camId + " was removed during the calibration");
    auto p = normalizeStageParams("color", c->stages.value("color", json::object()));
    p["previousLut"] = p["lut"];
    p["lut"] = res["lutPath"];
    p["enabled"] = true;
    p["chart"] = chart;
    p["deltaE"] = std::min(100.0, res["deltaE"].get<double>());
    p["deltaEBefore"] = std::min(100.0, res["deltaEBefore"].get<double>());
    p["calibratedAt"] = static_cast<double>(std::time(nullptr));
    p["reference"] = res.value("reference", false);
    c->stages["color"] = normalizeStageParams("color", p);
    show_ = std::move(next);
    fx_.showChanged(show_);
    return {{"deltaEBefore", res["deltaEBefore"]}, {"deltaE", res["deltaE"]}, {"rating", res["rating"]}, {"reference", res.value("reference", false)}};
  }
```
and inside the locked chain:
```cpp
  } else if (cmd == "undoColor") {
    auto& c = findCam(str(args, "cameraId"));
    auto p = normalizeStageParams("color", c.stages.value("color", json::object()));
    if (p["previousLut"] == "") throw CmdError("invalid", "nothing to undo");
    std::swap(p["lut"], p["previousLut"]);
    c.stages["color"] = normalizeStageParams("color", p);
  } else if (cmd == "clearColor") {
    auto& c = findCam(str(args, "cameraId"));
    auto old = normalizeStageParams("color", c.stages.value("color", json::object()));
    auto p = normalizeStageParams("color", json::object());
    p["enabled"] = old["enabled"];
    p["previousLut"] = old["lut"];
    c.stages["color"] = normalizeStageParams("color", p);
    fx_.forgetColor(c.id);
```
and in the existing `removeCamera` branch add `fx_.forgetColor(camId);`. (`#include <ctime>`, `core/chart.h`, `core/stage_params.h`.)

`src/engine/engine_main.cpp`: after the engine is created, `bc::ColorCalibrationService calib(engine, showDir / "luts");`; in the `Effects` implementation passed to `CommandRouter`, implement `findChart` → `calib.find(id)`, `calibrateColor` → the `Result` as JSON (`lutPath`, `deltaEBefore`, `deltaE`, `rating`, `reference`), `forgetColor` → `calib.forget(id)`.

- [ ] **Step 5: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "Calibration*,CommandRouter*,Colour stage*"` — Expected: all pass; paste the before/after lines. Then the whole suite.

- [ ] **Step 6: Commit**

```bash
git add src/engine tests CMakeLists.txt
git commit -m "feat(engine): chart calibration service with tablet reference, and its commands"
```

### Task 7: UI — Calibrate in the Colour effect, solo view with chart corners, chart image for tablets

Mockup: tabs "Multiview" (Colour group "✓ Good match · 19:40") and "Calibrate colour" in `docs/design/mockups/index.html`. English only.

**Files:**
- Create: `ui/src/calibration/chartGeometry.ts`, `ui/src/calibration/chartImage.ts`, `ui/src/calibration/useCalibration.ts`, `ui/src/calibration/CalibrationView.tsx`, `ui/src/calibration/ColourPanel.tsx`
- Modify: `ui/src/api/protocol.ts` (`StageField.hidden`), `ui/src/inspector/StageForm.tsx` (skip hidden fields), `ui/src/inspector/Effects.tsx` (Colour group body), `ui/src/App.tsx` (calibration state, solo view, Camera menu item), `ui/src/app.css`
- Test: `ui/src/calibration/chartGeometry.test.ts`, `ui/src/Calibration.test.tsx`

**Interfaces:**
- Consumes: commands `findChart`, `calibrateColor`, `undoColor`, `clearColor` (Task 6); the `color` params (`lut`, `previousLut`, `chart`, `deltaE`, `deltaEBefore`, `calibratedAt`, `reference`); M2's `Effects`, `StageForm`, `ConfirmDialog`; M1's `Workspace`, `Multiview`, `PreviewCanvas`, `ErrorBlock`, `Icon`.
- Produces: `useCalibration(client)` → `{ state, start(cameraId), setChart(c), moveCorner(i, [x, y]), capture(), cancel() }` with `state = null | { cameraId, chart, corners, found, manual, busy, error, detection }`; `CalibrationView({client, cam, n, state, onCorner})`; `ColourPanel({client, cam, n, params, calibrating, onCalibrate, calib})`; `chartPoint(corners, u, v)`; `downloadChartImage()`.

Behaviour:
- **Normal:** the Colour group shows one line — "✓ Good match · 19:40" (Fair/Poor in warning/danger colour; "Tablet reference · 19:40" for the reference camera; "Not calibrated" before the first run), the ΔE numbers and the chart type in its tooltip — and the buttons **Calibrate** (or **Calibrate again**), **Undo** (when `previousLut` is set) and **Clear…** (when `lut` is set; confirm "Clear calibration?" / "CAM n goes back to its uncorrected colours now." / **Clear calibration**).
- **Calibrating** (one camera at a time): the Multiview area shows that camera alone at the large preview size with the chart overlay — the quad outline, four draggable corner handles and a small square where each patch is read — under a bar "CAM n · name · Calibrating colour · Other cameras keep running". The Colour group shows: an info block "Hold the chart in front of CAM n under show lighting", Chart (Printed card | Tablet), a status line ("✓ Chart found, corners placed", "Looking for the chart…", or "Drag the four corners onto the chart" when detection is not available), **Capture** (primary) and **Cancel**, the hint "Drag a corner only if it is wrong. Tablet: show the chart image at full brightness." and the link **Save chart image for a tablet…**.
- Detection polls `findChart` once a second until the chart is found or the operator drags a corner. Default corners (before anything is found): the centre 50 % × 56 % of the picture.
- Capture sends `calibrateColor`; on success calibrating ends and the group shows the new result; on failure the error stays as a block in the group (e.g. "the corners are not on the chart").
- Keyboard: Escape cancels calibrating.

- [ ] **Step 1: Write the failing tests**

`ui/src/calibration/chartGeometry.test.ts`:
```ts
import { chartPoint } from './chartGeometry';

const q: [number, number][] = [
  [0.3, 0.25],
  [0.72, 0.22],
  [0.74, 0.7],
  [0.28, 0.72],
];

test('chart space maps to the four corners', () => {
  expect(chartPoint(q, 0, 0)).toEqual([0.3, 0.25]);
  const [x, y] = chartPoint(q, 1, 1);
  expect(x).toBeCloseTo(0.74, 6);
  expect(y).toBeCloseTo(0.7, 6);
});
```

`ui/src/Calibration.test.tsx`:
```tsx
import { fireEvent, render, screen, waitFor, within } from '@testing-library/react';

import { EngineClient } from './api/client';
import { Workspace } from './App';
import { FakeWs } from './test/fakeWs';

const colorSchema = {
  kind: 'color',
  label: 'Colour',
  description: 'Calibrate once against a colour chart.',
  icon: 'color',
  requires: [],
  onAdd: { enabled: true },
  fields: [
    { key: 'enabled', type: 'bool', default: false, label: 'On' },
    { key: 'lut', type: 'path', default: '', label: 'LUT', hidden: true },
  ],
};
const at = new Date(2026, 9, 8, 19, 40).getTime() / 1000;
function show(color: Record<string, unknown>) {
  return {
    schemaVersion: 1,
    settings: { rate: '60/1', resolution: '1920x1080', holdMs: 5000, slatePath: '' },
    cameras: [{ id: 'a', name: 'MC chính – Cam 2', input: { kind: 'srt', params: {} }, outputs: [], stages: { color } }],
  };
}
const state = { cameras: { a: { name: 'MC chính – Cam 2', phase: 'live', input: { state: 'live', error: null, health: 'receiving' }, stages: { color: { state: 'active', error: null } } } } };

async function setup(color: Record<string, unknown>) {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({ type: 'snapshot', state, show: show(color) });
  render(<Workspace client={c} />);
  const req = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'getStageSchemas');
  FakeWs.last.receive({ id: req.id, ok: true, result: [colorSchema] });
  const insp = screen.getByRole('complementary', { name: 'Inspector' });
  await waitFor(() => within(insp).getByRole('group', { name: 'Colour' }));
  return within(insp).getByRole('group', { name: 'Colour' });
}

test('a calibrated camera shows the result in words, with the numbers in the tooltip', async () => {
  const group = await setup({ enabled: true, lut: 'C:/l.cube', previousLut: '', chart: 'card', deltaE: 1.1, deltaEBefore: 7.4, calibratedAt: at, reference: false });
  const line = within(group).getByText(/Good match · 19:40/);
  expect(line.closest('[title]')).toHaveAttribute('title', expect.stringContaining('7.4'));
  expect(within(group).getByRole('button', { name: 'Calibrate again' })).toBeInTheDocument();
  expect(within(group).queryByRole('button', { name: 'Undo' })).toBeNull();
});

test('Calibrate shows the camera alone, looks for the chart and captures with the corners', async () => {
  const group = await setup({ enabled: true, lut: '', previousLut: '', chart: 'card', deltaE: 0, deltaEBefore: 0, calibratedAt: 0, reference: false });
  expect(within(group).getByText('Not calibrated')).toBeInTheDocument();
  fireEvent.click(within(group).getByRole('button', { name: 'Calibrate' }));
  expect(screen.getByText('Calibrating colour')).toBeInTheDocument();
  const find = FakeWs.last.lastRequest();
  expect(find.cmd).toBe('findChart');
  FakeWs.last.receive({ id: find.id, ok: true, result: { available: true, found: true, corners: [[0.3, 0.25], [0.72, 0.22], [0.74, 0.7], [0.28, 0.72]] } });
  await waitFor(() => expect(within(group).getByText(/Chart found/)).toBeInTheDocument());
  fireEvent.click(within(group).getByRole('radio', { name: 'Tablet' }));
  fireEvent.click(within(group).getByRole('button', { name: 'Capture' }));
  const cal = FakeWs.last.lastRequest();
  expect(cal.cmd).toBe('calibrateColor');
  expect(cal.args).toEqual({ cameraId: 'a', chart: 'tablet', corners: [[0.3, 0.25], [0.72, 0.22], [0.74, 0.7], [0.28, 0.72]] });
  FakeWs.last.receive({ id: cal.id, ok: true, result: { deltaEBefore: 6, deltaE: 1.2, rating: 'good', reference: false } });
  await waitFor(() => expect(screen.queryByText('Calibrating colour')).toBeNull());
});

test('a failed capture keeps calibrating and shows why', async () => {
  const group = await setup({ enabled: true, lut: '', previousLut: '', chart: 'card', deltaE: 0, deltaEBefore: 0, calibratedAt: 0, reference: false });
  fireEvent.click(within(group).getByRole('button', { name: 'Calibrate' }));
  fireEvent.click(within(group).getByRole('button', { name: 'Capture' }));
  const cal = FakeWs.last.sent.map((s) => JSON.parse(s)).filter((r) => r.cmd === 'calibrateColor').pop();
  FakeWs.last.receive({ id: cal.id, ok: false, error: { code: 'invalid', message: 'the corners are not on the chart' } });
  await waitFor(() => expect(within(group).getByRole('alert')).toHaveTextContent('the corners are not on the chart'));
  expect(screen.getByText('Calibrating colour')).toBeInTheDocument();
});
```

Run `PATH="/c/Program Files/nodejs:$PATH" npm test` in `ui/` — Expected: FAIL.

- [ ] **Step 2: Geometry, the chart image, the calibration hook**

`ui/src/api/protocol.ts`: add `hidden?: boolean;` to `StageField`. `ui/src/inspector/StageForm.tsx`: change `schema.fields.slice(1)` to `schema.fields.slice(1).filter((f) => !f.hidden)`.

`ui/src/calibration/chartGeometry.ts`:
```ts
export type Pt = [number, number];

// Chart space (u across 6 columns, v down 4 rows) → picture, through the four grid corners
// (top-left, top-right, bottom-right, bottom-left). Same maths as src/core/chart.cpp.
export function chartPoint(q: Pt[], u: number, v: number): Pt {
  const [p0, p1, p2, p3] = q;
  const dx1 = p1[0] - p2[0], dx2 = p3[0] - p2[0], dx3 = p0[0] - p1[0] + p2[0] - p3[0];
  const dy1 = p1[1] - p2[1], dy2 = p3[1] - p2[1], dy3 = p0[1] - p1[1] + p2[1] - p3[1];
  const den = dx1 * dy2 - dx2 * dy1 || 1e-12;
  const g = (dx3 * dy2 - dx2 * dy3) / den;
  const h = (dx1 * dy3 - dx3 * dy1) / den;
  const a = p1[0] - p0[0] + g * p1[0], b = p3[0] - p0[0] + h * p3[0];
  const d = p1[1] - p0[1] + g * p1[1], e = p3[1] - p0[1] + h * p3[1];
  const w = g * u + h * v + 1;
  return [(a * u + b * v + p0[0]) / w, (d * u + e * v + p0[1]) / w];
}

export const DEFAULT_CORNERS: Pt[] = [
  [0.25, 0.22],
  [0.75, 0.22],
  [0.75, 0.78],
  [0.25, 0.78],
];
```

`ui/src/calibration/chartImage.ts`:
```ts
// The ColorChecker 24 layout in sRGB, for showing on a tablet (spec §5.3). Same values as src/core/chart.cpp.
const SRGB = [
  '#735244', '#c29682', '#627a9d', '#576c43', '#8580b1', '#67bdaa',
  '#d67e2c', '#505ba6', '#c15a63', '#5e3c6c', '#9dbc40', '#e0a32e',
  '#383d96', '#469449', '#af363c', '#e7c71f', '#bb5695', '#0885a1',
  '#f3f3f2', '#c8c8c8', '#a0a0a0', '#7a7a79', '#555555', '#343434',
];

// Saves breadcamera-chart.png through the WebView2 download prompt.
export function downloadChartImage() {
  const c = document.createElement('canvas');
  c.width = 2048;
  c.height = 1536;
  const ctx = c.getContext('2d');
  if (!ctx) return;
  ctx.fillStyle = '#141414';
  ctx.fillRect(0, 0, c.width, c.height);
  const cell = 300, gap = 30, x0 = (c.width - 6 * cell) / 2, y0 = (c.height - 4 * cell) / 2;
  SRGB.forEach((hex, i) => {
    ctx.fillStyle = hex;
    ctx.fillRect(x0 + (i % 6) * cell + gap / 2, y0 + Math.floor(i / 6) * cell + gap / 2, cell - gap, cell - gap);
  });
  c.toBlob((blob) => {
    if (!blob) return;
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'breadcamera-chart.png';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  });
}
```

`ui/src/calibration/useCalibration.ts`:
```ts
import { useCallback, useEffect, useRef, useState } from 'react';

import type { EngineClient } from '../api/client';
import { DEFAULT_CORNERS, type Pt } from './chartGeometry';

export interface CalibrationState {
  cameraId: string;
  chart: 'card' | 'tablet';
  corners: Pt[];
  found: boolean;
  manual: boolean;
  detection: boolean | null; // null = not asked yet
  busy: boolean;
  error: string;
}

// One camera at a time (spec §6.4): find the chart, let the operator fix the corners, capture.
export function useCalibration(client: EngineClient) {
  const [state, setState] = useState<CalibrationState | null>(null);
  const live = useRef<CalibrationState | null>(null);
  live.current = state;

  useEffect(() => {
    if (!state || state.found || state.manual || state.busy || state.detection === false) return;
    let stop = false;
    const look = () =>
      client
        .request<{ available: boolean; found: boolean; corners?: Pt[] }>('findChart', { cameraId: state.cameraId })
        .then((r) => {
          if (stop || !live.current || live.current.manual) return;
          setState({ ...live.current, detection: r.available, found: r.found, corners: r.found && r.corners ? r.corners : live.current.corners });
        })
        .catch(() => {});
    look();
    const t = setInterval(look, 1000);
    return () => {
      stop = true;
      clearInterval(t);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [client, state?.cameraId, state?.found, state?.manual, state?.busy, state?.detection]);

  const start = useCallback(
    (cameraId: string) => setState({ cameraId, chart: 'card', corners: DEFAULT_CORNERS, found: false, manual: false, detection: null, busy: false, error: '' }),
    [],
  );
  const cancel = useCallback(() => setState(null), []);
  const setChart = useCallback((chart: 'card' | 'tablet') => setState((s) => (s ? { ...s, chart } : s)), []);
  const moveCorner = useCallback((i: number, p: Pt) => setState((s) => (s ? { ...s, manual: true, corners: s.corners.map((c, k) => (k === i ? p : c)) } : s)), []);
  const capture = useCallback(async () => {
    const s = live.current;
    if (!s) return;
    setState({ ...s, busy: true, error: '' });
    try {
      await client.request('calibrateColor', { cameraId: s.cameraId, chart: s.chart, corners: s.corners });
      setState(null);
    } catch (e) {
      setState({ ...s, busy: false, error: e instanceof Error ? e.message : String(e) });
    }
  }, [client]);

  return { state, start, cancel, setChart, moveCorner, capture };
}
export type Calibration = ReturnType<typeof useCalibration>;
```

- [ ] **Step 3: The solo view and the Colour panel**

`ui/src/calibration/CalibrationView.tsx`:
```tsx
import { useEffect, useRef, type PointerEvent } from 'react';

import type { EngineClient } from '../api/client';
import type { CameraConfig } from '../api/protocol';
import { Icon } from '../components/Icon';
import { PreviewCanvas } from '../preview/PreviewCanvas';
import { chartPoint, type Pt } from './chartGeometry';
import type { CalibrationState } from './useCalibration';

// The calibrating camera alone, with the chart corners on top (spec §6.4).
export function CalibrationView({ client, cam, n, state, onCorner }: { client: EngineClient; cam: CameraConfig; n: number; state: CalibrationState; onCorner: (i: number, p: Pt) => void }) {
  const box = useRef<HTMLDivElement>(null);
  const drag = useRef<number | null>(null);
  useEffect(() => client.setPreviewSubscription([cam.id], 'large'), [client, cam.id]);

  function at(e: PointerEvent): Pt {
    const r = box.current!.getBoundingClientRect();
    return [Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)), Math.min(1, Math.max(0, (e.clientY - r.top) / r.height))];
  }
  const q = state.corners;
  const squares: Pt[] = [];
  for (let p = 0; p < 24; p++) squares.push(chartPoint(q, ((p % 6) + 0.5) / 6, (Math.floor(p / 6) + 0.5) / 4));

  return (
    <section className="center" aria-label="Calibrating">
      <div className="solo-bar">
        <Icon name="chart" size={14} />
        <b>
          CAM {n} · {cam.name}
        </b>
        <span>Calibrating colour</span>
        <span className="right">Other cameras keep running</span>
      </div>
      <div className="cal-view">
        <div
          className="pic cal-pic"
          ref={box}
          onPointerMove={(e) => drag.current !== null && onCorner(drag.current, at(e))}
          onPointerUp={() => (drag.current = null)}
          onPointerLeave={() => (drag.current = null)}
        >
          <PreviewCanvas client={client} cameraId={cam.id} size="large" />
          <svg className="chart-overlay" viewBox="0 0 1 1" preserveAspectRatio="none" aria-label="Chart corners">
            <polygon points={q.map((p) => p.join(',')).join(' ')} />
            {squares.map(([x, y], i) => (
              <rect key={i} x={x - 0.006} y={y - 0.009} width={0.012} height={0.018} className="sample" />
            ))}
          </svg>
          {q.map(([x, y], i) => (
            <button
              key={i}
              className="corner"
              aria-label={`Chart corner ${i + 1}`}
              style={{ left: `${x * 100}%`, top: `${y * 100}%` }}
              onPointerDown={(e) => {
                (e.target as Element).setPointerCapture?.(e.pointerId);
                drag.current = i;
              }}
            />
          ))}
        </div>
      </div>
    </section>
  );
}
```

`ui/src/calibration/ColourPanel.tsx`:
```tsx
import { useState } from 'react';

import type { EngineClient } from '../api/client';
import type { CameraConfig } from '../api/protocol';
import { ErrorBlock } from '../components/ErrorBlock';
import { Icon } from '../components/Icon';
import { ConfirmDialog } from '../dialogs/ConfirmDialog';
import { clock } from '../state/marks';
import { downloadChartImage } from './chartImage';
import type { Calibration } from './useCalibration';

const WORDS = { good: ['Good match', 'ok-line'], fair: ['Fair match', 'warn-line'], poor: ['Poor match — check the lighting and glare', 'bad-line'] } as const;
const rating = (d: number) => (d <= 2 ? 'good' : d <= 4 ? 'fair' : 'poor');

// The Colour effect's body: the result in words, or the calibrate controls (spec §5.3, §6.4).
export function ColourPanel({ client, cam, n, calib }: { client: EngineClient; cam: CameraConfig; n: number; calib: Calibration }) {
  const [clearing, setClearing] = useState(false);
  const [error, setError] = useState('');
  const p = (cam.stages?.color ?? {}) as Record<string, unknown>;
  const s = calib.state?.cameraId === cam.id ? calib.state : null;

  if (s) {
    const status = s.found ? '✓ Chart found, corners placed' : s.detection === false || s.manual ? `Drag the four corners onto the chart` : 'Looking for the chart…';
    return (
      <>
        <ErrorBlock severity="info" title="Calibrating." message={`Hold the chart in front of CAM ${n} under show lighting.`} />
        {s.error && <ErrorBlock title="Capture failed." message={s.error} />}
        <div className="prop">
          <label>Chart</label>
          <span className="seg" role="radiogroup" aria-label="Chart">
            {(['card', 'tablet'] as const).map((c) => (
              <button key={c} role="radio" aria-checked={s.chart === c} className={s.chart === c ? 'on' : ''} onClick={() => calib.setChart(c)}>
                {c === 'card' ? 'Printed card' : 'Tablet'}
              </button>
            ))}
          </span>
        </div>
        <div className="prop">
          <label>Status</label>
          <span className={s.found ? 'ok-line' : 'ro'}>{status}</span>
        </div>
        <div className="row-btns">
          <button className="btn primary" disabled={s.busy} onClick={calib.capture}>
            <Icon name="check" size={14} />
            {s.busy ? 'Capturing…' : 'Capture'}
          </button>
          <button className="btn" onClick={calib.cancel}>
            Cancel
          </button>
        </div>
        <p className="note">Drag a corner only if it is wrong. Tablet: show the chart image at full brightness.</p>
        <button className="link" onClick={downloadChartImage}>
          Save chart image for a tablet…
        </button>
      </>
    );
  }

  const lut = String(p.lut ?? '');
  const at = Number(p.calibratedAt ?? 0) * 1000;
  const d = Number(p.deltaE ?? 0);
  const [word, cls] = WORDS[rating(d)];
  const tip = `Average colour error ${Number(p.deltaEBefore ?? 0).toFixed(1)} before, ${d.toFixed(1)} after (ΔE2000) · ${p.chart === 'tablet' ? 'tablet' : 'printed card'}`;
  async function run(cmd: string) {
    try {
      await client.request(cmd, { cameraId: cam.id });
      setError('');
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }
  return (
    <>
      {error && <ErrorBlock message={error} />}
      <div className="prop">
        <label>Calibration</label>
        {!lut ? (
          <span className="ro">Not calibrated</span>
        ) : p.reference ? (
          <span className="ok-line" title="The other tablet cameras are matched to this one">
            Tablet reference · {clock(at)}
          </span>
        ) : (
          <span className={cls} title={tip}>
            {rating(d) === 'good' && <Icon name="check" size={12} />}
            {word} · {clock(at)}
          </span>
        )}
      </div>
      <div className="row-btns">
        <button className="btn" disabled={calib.state !== null} onClick={() => calib.start(cam.id)}>
          <Icon name="chart" size={14} />
          {lut ? 'Calibrate again' : 'Calibrate'}
        </button>
        {String(p.previousLut ?? '') && (
          <button className="btn" onClick={() => run('undoColor')}>
            Undo
          </button>
        )}
        {lut && (
          <button className="btn ghost-danger" onClick={() => setClearing(true)}>
            Clear…
          </button>
        )}
      </div>
      {clearing && (
        <ConfirmDialog
          title="Clear calibration?"
          body={`CAM ${n} goes back to its uncorrected colours now.`}
          confirm="Clear calibration"
          onCancel={() => setClearing(false)}
          onConfirm={async () => {
            await run('clearColor');
            setClearing(false);
          }}
        />
      )}
    </>
  );
}
```

- [ ] **Step 4: Wire it in**

- `ui/src/inspector/Effects.tsx`: add a prop `calib: Calibration` (import the type from `../calibration/useCalibration`) and, inside each group's `group-b`, right before `<StageForm …/>`: `{s.kind === 'color' && <ColourPanel client={client} cam={cam} n={n} calib={calib} />}` (import `ColourPanel`).
- `ui/src/App.tsx` (`Workspace`):
  - `const calib = useCalibration(client);` (import); pass `calib={calib}` to `<Effects …/>`;
  - render the centre as `{calib.state && calibCam ? <CalibrationView client={client} cam={calibCam} n={calibIndex + 1} state={calib.state} onCorner={calib.moveCorner} /> : <Multiview … />}`, where `calibIndex = cameras.findIndex((c) => c.id === calib.state?.cameraId)` and `calibCam = cameras[calibIndex]` (if the camera disappears, call `calib.cancel()` in an effect);
  - while calibrating, keep the Inspector on that camera (`selected` follows `calib.state.cameraId`);
  - Escape cancels: in the existing keydown handler, `if (e.key === 'Escape' && calib.state) calib.cancel();`;
  - `menus.camera` gains `{ label: 'Calibrate colour…', shortcut: 'Ctrl+K', disabled: !cam || !cam.stages || !('color' in cam.stages), onSelect: () => cam && calib.start(cam.id) }` and Ctrl+K in the keydown handler does the same.
- `ui/src/app.css` — append:
```css
/* calibration (M3) */
.solo-bar { height: 30px; display: flex; align-items: center; gap: 8px; padding: 0 10px; background: var(--bm-brand-wash); color: var(--bm-text-muted); border-bottom: 1px solid var(--bm-border-subtle); }
.solo-bar svg { color: var(--bm-brand); }
.solo-bar b { color: var(--bm-text); font-weight: 600; }
.solo-bar .right { margin-left: auto; color: var(--bm-text-dim); }
.cal-view { flex: 1; display: grid; place-items: center; padding: 10px; min-height: 0; }
.cal-pic { position: relative; height: 100%; max-width: 100%; touch-action: none; }
.chart-overlay { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }
.chart-overlay polygon { fill: none; stroke: var(--bm-brand); stroke-width: 0.002; vector-effect: non-scaling-stroke; }
.chart-overlay .sample { fill: none; stroke: var(--bm-text); stroke-width: 1; vector-effect: non-scaling-stroke; }
.corner { position: absolute; width: 16px; height: 16px; margin: -8px 0 0 -8px; border-radius: 50%; background: var(--bm-bg); border: 2px solid var(--bm-brand); cursor: grab; }
.corner:active { cursor: grabbing; }
.ok-line, .warn-line, .bad-line { display: inline-flex; align-items: center; gap: 5px; }
.ok-line { color: var(--bm-success); }
.warn-line { color: var(--bm-warning); }
.bad-line { color: var(--bm-danger); }
.row-btns { display: flex; gap: 6px; flex-wrap: wrap; }
.link { color: var(--bm-text-muted); text-decoration: underline; text-underline-offset: 3px; text-decoration-color: var(--bm-border-strong); justify-self: start; }
```

- [ ] **Step 5: Run the UI tests and build**

In `ui/`: `npm test` (all pass) and `npm run build`. Compare once with the mockup's "Calibrate colour" tab (screenshot in the report).

- [ ] **Step 6: Commit**

```bash
git add ui/src
git commit -m "feat(ui): calibrate colour in the Colour effect, with a solo view and chart corners"
```

### Task 8: M3 acceptance — a cast camera calibrated end to end on the GPU; the human check; follow-ups

**Files:**
- Create: `tests/e2e_m3_test.cpp`, `docs/superpowers/manual-checks/m3-calibration.md`
- Modify: `tests/support/fake_output.h` (keep the last image), `docs/superpowers/follow-ups.md`, `CMakeLists.txt`

**Interfaces:**
- Consumes: `ColorCalibrationService` (Task 6), `Engine` with `GpuTools` (Task 4), the fake chart input (Task 6), `sampleChart`, `meanDeltaE`, `colorCheckerReference` (Task 2).
- Produces: spec §9 M3 evidence ("calibration works with a printed card and with a tablet" — stood in by rendered charts with known casts through the real GPU path; a person checks real cameras, Step 4).

- [ ] **Step 1: Keep the last output image**

`tests/support/fake_output.h`: in `deliver`, also keep `frame.bgra` (when set) in a member under the existing mutex, and add `std::shared_ptr<const bc::CpuImage> lastImage() const`.

- [ ] **Step 2: Write the acceptance test**

`tests/e2e_m3_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <thread>

#include "core/calibration.h"
#include "core/stage_params.h"
#include "core/state_store.h"
#include "engine/color_calibration_service.h"
#include "engine/engine.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "support/fake_input.h"
#include "support/requires.h"
#include "support/temp_dir.h"

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
const bc::ChartQuad kQuad{{{{0.25, 0.22}, {0.75, 0.22}, {0.75, 0.78}, {0.25, 0.78}}}};

bc::CameraConfig chartCam(const std::string& id, std::vector<float> cast) {
  bc::CameraConfig c;
  c.id = id;
  c.name = id;
  c.input.kind = "fake";
  c.input.params = {{"tag", id}, {"width", 960}, {"height", 540}, {"chart", true}, {"cast", cast}};
  c.outputs = {{"o-" + id, "fake", false, true, json::object()}};
  return c;
}
}  // namespace

TEST_CASE("M3 acceptance: printed card and tablet calibration through the GPU" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::Compositor comp(*gpu, pool);
  bc::ColorGrader grader(*gpu, pool);
  bc::StateStore state;
  bc::test::FakeFactory factory(conv);
  bc::Engine engine(bc::GpuTools{conv, comp, grader}, state, factory);
  bc::test::TempDir dir;
  bc::ColorCalibrationService svc(engine, dir.path / "luts");

  bc::Show show;
  show.settings.width = 960;
  show.settings.height = 540;
  show.cameras = {chartCam("card", {1.25f, 1.0f, 0.7f}), chartCam("t1", {1.2f, 1.0f, 0.8f}), chartCam("t2", {0.8f, 1.0f, 1.25f})};
  engine.apply(show);
  engine.start();
  std::this_thread::sleep_for(500ms);

  auto card = svc.calibrate("card", "card", kQuad);
  auto t1 = svc.calibrate("t1", "tablet", kQuad);
  auto t2 = svc.calibrate("t2", "tablet", kQuad);
  REQUIRE(t1.reference);
  auto setLut = [&](size_t i, const std::string& lut) {
    show.cameras[i].stages = {{"color", bc::normalizeStageParams("color", {{"enabled", true}, {"lut", lut}})}};
  };
  setLut(0, card.lutPath);
  setLut(1, t1.lutPath);
  setLut(2, t2.lutPath);
  engine.apply(show);
  std::this_thread::sleep_for(800ms);

  auto seen = [&](const std::string& id) { return bc::sampleChart({*factory.output("o-" + id)->lastImage()}, kQuad); };
  const double cardAfter = bc::meanDeltaE(seen("card"), bc::colorCheckerReference());
  const double tabletAfter = bc::meanDeltaE(seen("t1"), seen("t2"));
  MESSAGE("card: solver " << card.deltaEBefore << " -> " << card.deltaE << ", through the GPU " << cardAfter);
  MESSAGE("tablet: solver " << t2.deltaEBefore << " -> " << t2.deltaE << ", camera-to-camera through the GPU " << tabletAfter);
  CHECK(cardAfter < 2.5);
  CHECK(tabletAfter < 2.5);
  engine.stop();
}
```
(The GPU bound is 2.5, not 2: the 33³ LUT is sampled trilinearly on the GPU and the output is 8-bit.)

Add it to `bc_tests`.

- [ ] **Step 3: Run it**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "M3 acceptance*"` — Expected: PASS; paste both MESSAGE lines. If a bound fails, report the numbers; do not relax it.

- [ ] **Step 4: The human check and follow-ups**

`docs/superpowers/manual-checks/m3-calibration.md`:
```markdown
# M3 manual check — colour calibration on real cameras

A person does this; it is not self-certified (spec §9 M3).

1. Two physical cameras on the same scene, show lighting on.
2. Printed ColorChecker Classic 24: on CAM 1, Colour → Calibrate, hold the card up, check the corners, Capture. Note the words shown (Good/Fair/Poor) and the tooltip numbers. Repeat on CAM 2.
3. Judge by eye on a calibrated monitor: skin tones and the grey row look the same on both cameras.
4. Tablet: Save chart image for a tablet…, show it full screen at full brightness. Clear both calibrations; calibrate CAM 1 with "Tablet" (it becomes the reference), then CAM 2. Judge again.
5. Upside-down card, tilted card, card too small in the picture: note what happens.
6. Record: date, cameras, lighting, results, and anything that looked wrong.
```

Append to `docs/superpowers/follow-ups.md`:
```markdown
## M3
- Real-camera calibration is a **human check** (`docs/superpowers/manual-checks/m3-calibration.md`). Automated evidence uses rendered charts with known casts (`tests/e2e_m3_test.cpp`).
- Chart detection depends on OpenCV's `mcc` module (`BC_WITH_CHART_DETECT`); without it the operator places the corners (`src/engine/chart_detector.cpp`).
- The tablet chart's colours are only as good as the tablet; tablet mode matches cameras to each other, not to absolute colour (`src/engine/color_calibration_service.cpp`).
- Trims stay in the colour schema as hidden fields at neutral values; manual trims are out of scope (spec §10) (`src/core/stage_params.cpp`).
- OpenCV (Apache-2.0) must be listed in the M5 licence notices (`installer/THIRD_PARTY_NOTICES.md`).
```
Add any `ponytail:` comment or deferred minor from this run.

- [ ] **Step 5: Full verification**

1. `scripts/test.ps1` (all) — passed/skipped and reasons, with the configure line `BC_WITH_*=` values (incl. `BC_WITH_CHART_DETECT`).
2. `ui/`: `npm test`, `npm run build`.
3. `BC_FORCE_WARP=1` run — record.

- [ ] **Step 6: Commit**

```bash
git add tests docs/superpowers CMakeLists.txt
git commit -m "test: M3 acceptance (card and tablet calibration through the GPU), manual check and follow-ups"
```
