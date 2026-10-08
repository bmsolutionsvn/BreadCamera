# BreadCamera M3 — One-shot colour match Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The operator picks a reference camera and targets, optionally draws a region on each, clicks Match; BreadCamera captures ~1 s, solves white balance/exposure + Lab mean/spread + per-channel tone curves, bakes a 33³ LUT per target, and locks it; trims (exposure, temperature/tint, lift/gamma/gain, saturation), `.cube` import, undo and reset sit on top.

**Architecture:** Pure CPU code in `bc_core` for colour maths, the `.cube` format and the solver (unit-tested with a synthetic ΔE2000 check). A `ColorGrader` on the GPU applies the LUT (3D texture) and trims as the **first** stage (before matte, spec §3.2). The engine gains a frame-capture API; a `ColorMatchService` captures, solves off the tick thread, writes LUT files next to the show and returns paths that the router stores in the `color` stage params.

**Tech Stack:** as M1/M2. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-10-08-breadcamera-design.md` (§5.3, §3.2, §8.1 "mean ΔE2000 below 2").

**Branch:** `bcam-m3`, cut from `bcam-m2`.

## Global Constraints

- All M1 and M2 Global Constraints apply.
- Stage order is **Color → Matte → Background**. The colour stage is a guarded stage like the others (bypass on failure/overrun, retry after 5 s).
- The match is **one-shot and locked**: nothing changes until the operator matches again (spec §5.3). No continuous adaptation.
- LUTs are **33³**, written as standard `.cube` files (`LUT_3D_SIZE 33`, red fastest) in `<show dir>\luts\`. The LUT works on gamma-encoded values (the pipeline's gamma 2.4), so imported `.cube` files designed for Rec.709 display-referred footage behave as expected.
- Tone curves are clamped (|curve(x) − x| ≤ 0.15) and smoothed so a match can never produce banding or extreme shifts (spec §5.3).
- Capture: ~1 s per camera (30 samples, every other tick) at 256×144, from the frame **before** any stage. A camera without picture (phase not `live`) fails the match with a clear message; no LUT is changed.
- The solver's acceptance test: synthetic known transform → match → **mean ΔE2000 < 2** (spec §8.1).

## Review Focus

1. **A camera with no picture or a black/flat region** (lens cap, slate, region of one colour) — Match refuses with a readable error and changes nothing — owned by Task 2 (flat stats) and Task 5 (no-signal camera).
2. **A malformed or huge `.cube` file** (wrong count, NaN, size 200, DOMAIN not 0..1) — import is refused with the line number, the current LUT stays — owned by Task 1 and Task 5.
3. **Undo after two matches / reset** — undo returns to exactly the previous LUT; reset clears LUT and trims — owned by Task 5.
4. **The LUT file disappears or is edited on disk** — the colour stage shows `color-lut` and keeps running with trims only, never black — owned by Task 4.
5. **Extreme casts** (target 4× too bright, or strongly blue) — gains are clamped, the result is bounded and monotone, no NaN — owned by Task 2.

## File Structure

```
src/core/color_math.h/.cpp     linear↔Lab, ΔE2000
src/core/lut.h/.cpp            Lut3D type, .cube read/write, CPU apply
src/core/color_match.h/.cpp    region sampling, solver, LUT bake
src/gpu/color_grader.h/.cpp    3D LUT upload + trims shader
src/gpu/shaders/color_grade.hlsl
src/engine/color_match_service.h/.cpp
ui/src/screens/ColorMatch.tsx, ui/src/components/RegionPicker.tsx
```

## Execution notes for the controller

- 7 tasks → batched reviews (1–3, 4–6, 7).

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

### Task 2: The colour-match solver

**Files:**
- Create: `src/core/color_match.h`, `src/core/color_match.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/color_match_test.cpp`

**Interfaces:**
- Consumes: `Rgb`, `Lab`, `linearToLab`, `labToLinear`, `toGamma`, `toLinearLight`, `deltaE2000`, `Lut3D` (Task 1); `CpuImage` (M1 Task 5 — `gpu/converter.h` is header-only for that struct; include it).
- Produces:
  - `bc::Region { float x = 0, y = 0, w = 1, h = 1; }` (normalised 0..1), `bool bc::validRegion(const Region&)`.
  - `std::vector<Rgb> bc::sampleRegion(const std::vector<CpuImage>& frames, const Region&, int step = 2)` — BGRA gamma-encoded bytes → **linear** samples, every `step`-th pixel in x and y.
  - `bc::MatchResult { Lut3D lut; std::array<float,3> gains; double residualDeltaE; }`, `bc::MatchResult bc::solveColorMatch(const std::vector<Rgb>& reference, const std::vector<Rgb>& target, int lutSize = 33)` — throws `std::invalid_argument` ("not enough picture", "the region is flat") when a side has < 64 samples or a luminance spread < 0.002.

Algorithm (spec §5.3):
1. **Gains** = per-channel mean(ref) / mean(target), each clamped to 0.25..4.
2. **Lab transfer** on gained target: per channel, `(v − meanT) × clamp(stdR / stdT, 0.5, 2) + meanR` (scale 1 when `stdT < 1e-4`).
3. **Tone curves** per RGB channel in the gamma domain: 256-bin histograms of reference and of the target after steps 1–2; curve = inverse-CDF match; smoothed with a 9-bin box; forced monotone; clamped to |curve(x) − x| ≤ 0.15.
4. **Bake** a `lutSize`³ LUT: grid input (gamma) → linear → gains → Lab transfer → linear (clamped ≥ 0, ≤ 1) → gamma → curves.
5. **Residual** = ΔE2000 between mean Lab of the reference and mean Lab of the corrected target.

- [ ] **Step 1: Write the failing tests**

`tests/color_match_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <random>

#include "core/color_match.h"

namespace {
std::vector<bc::Rgb> randomColours(int n, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> d(0.02f, 0.8f);
  std::vector<bc::Rgb> out(n);
  for (auto& c : out) c = {d(rng), d(rng), d(rng)};
  return out;
}

bc::Rgb cast(bc::Rgb c) {  // a known camera difference: WB shift + a tone tweak
  return {std::pow(c.r * 1.15f, 1.1f), std::pow(c.g, 1.1f), std::pow(c.b * 0.8f, 1.1f)};
}

bc::Rgb applyLinear(const bc::Lut3D& lut, bc::Rgb lin) {
  auto g = lut.apply({bc::toGamma(lin.r), bc::toGamma(lin.g), bc::toGamma(lin.b)});
  return {bc::toLinearLight(g.r), bc::toLinearLight(g.g), bc::toLinearLight(g.b)};
}
}  // namespace

TEST_CASE("solveColorMatch: a known cast is undone to mean ΔE2000 < 2") {
  auto ref = randomColours(4096, 1);
  std::vector<bc::Rgb> tgt;
  for (auto& c : ref) tgt.push_back(cast(c));
  auto m = bc::solveColorMatch(ref, tgt);
  REQUIRE(m.lut.size == 33);
  double sum = 0, before = 0;
  for (size_t i = 0; i < ref.size(); ++i) {
    sum += bc::deltaE2000(bc::linearToLab(ref[i]), bc::linearToLab(applyLinear(m.lut, tgt[i])));
    before += bc::deltaE2000(bc::linearToLab(ref[i]), bc::linearToLab(tgt[i]));
  }
  MESSAGE("mean ΔE2000 before " << before / ref.size() << ", after " << sum / ref.size());
  CHECK(sum / ref.size() < 2.0);
  CHECK(m.residualDeltaE < 2.0);
}

TEST_CASE("solveColorMatch: matching a camera to itself is (nearly) identity") {
  auto ref = randomColours(2048, 7);
  auto m = bc::solveColorMatch(ref, ref);
  auto id = bc::Lut3D::identity(33);
  double worst = 0;
  for (size_t i = 0; i < m.lut.data.size(); ++i) {
    worst = std::max<double>(worst, std::abs(m.lut.data[i].r - id.data[i].r));
  }
  CHECK(worst < 0.03);
}

TEST_CASE("solveColorMatch: refuses flat or tiny regions") {
  std::vector<bc::Rgb> flat(1000, {0.2f, 0.2f, 0.2f});
  auto ok = randomColours(1000, 3);
  CHECK_THROWS_AS(bc::solveColorMatch(ok, flat), std::invalid_argument);
  CHECK_THROWS_AS(bc::solveColorMatch(ok, std::vector<bc::Rgb>(10, {0.5f, 0.5f, 0.5f})), std::invalid_argument);
}

TEST_CASE("solveColorMatch: extreme casts stay bounded, monotone and finite") {
  auto ref = randomColours(2048, 11);
  std::vector<bc::Rgb> bright;
  for (auto& c : ref) bright.push_back({std::min(c.r * 4.0f, 1.0f), std::min(c.g * 4.0f, 1.0f), std::min(c.b * 6.0f, 1.0f)});
  auto m = bc::solveColorMatch(ref, bright);
  for (auto& c : m.lut.data) {
    CHECK(std::isfinite(c.r));
    CHECK(c.r >= 0.0f);
    CHECK(c.r <= 1.0f);
  }
  for (int i = 1; i < m.lut.size; ++i) CHECK(m.lut.at(i, 0, 0).r >= m.lut.at(i - 1, 0, 0).r - 1e-4f);
  for (auto g : m.gains) CHECK(g >= 0.25f);
}

TEST_CASE("sampleRegion: picks the region and linearises") {
  bc::CpuImage img{4, 2, std::vector<uint8_t>(4 * 2 * 4, 0)};
  for (int i = 0; i < 4 * 2; ++i) {
    const bool right = (i % 4) >= 2;
    img.bgra[i * 4 + 2] = right ? 255 : 0;  // red on the right half
  }
  auto s = bc::sampleRegion({img}, {0.5f, 0, 0.5f, 1}, 1);
  REQUIRE(s.size() == 4);
  for (auto& c : s) CHECK(c.r == doctest::Approx(1.0f));
  CHECK_FALSE(bc::validRegion({0.9f, 0, 0.5f, 1}));
  CHECK(bc::validRegion({0, 0, 1, 1}));
}
```

Add it to `bc_tests`, `src/core/color_match.cpp` to `bc_core`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "solveColorMatch*,sampleRegion*"`
Expected: build FAILS.

- [ ] **Step 3: Implement the solver**

`src/core/color_match.h`:
```cpp
#pragma once
#include <array>
#include <vector>

#include "core/color_math.h"
#include "core/lut.h"
#include "gpu/converter.h"  // CpuImage only

namespace bc {
struct Region {
  float x = 0, y = 0, w = 1, h = 1;  // normalised
};
bool validRegion(const Region& r);
std::vector<Rgb> sampleRegion(const std::vector<CpuImage>& frames, const Region& r, int step = 2);

struct MatchResult {
  Lut3D lut;
  std::array<float, 3> gains{1, 1, 1};
  double residualDeltaE = 0;
};
// spec §5.3: gains → Lab mean/spread → clamped tone curves → baked LUT.
MatchResult solveColorMatch(const std::vector<Rgb>& reference, const std::vector<Rgb>& target, int lutSize = 33);
}  // namespace bc
```

`src/core/color_match.cpp`:
```cpp
#include "core/color_match.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace bc {
namespace {
struct Stats {
  double mean[3] = {0, 0, 0};
  double sd[3] = {0, 0, 0};
};

Stats labStats(const std::vector<Lab>& v) {
  Stats s;
  for (auto& c : v) {
    s.mean[0] += c.L;
    s.mean[1] += c.a;
    s.mean[2] += c.b;
  }
  for (double& m : s.mean) m /= static_cast<double>(v.size());
  for (auto& c : v) {
    s.sd[0] += (c.L - s.mean[0]) * (c.L - s.mean[0]);
    s.sd[1] += (c.a - s.mean[1]) * (c.a - s.mean[1]);
    s.sd[2] += (c.b - s.mean[2]) * (c.b - s.mean[2]);
  }
  for (double& d : s.sd) d = std::sqrt(d / static_cast<double>(v.size()));
  return s;
}

float ch(const Rgb& c, int i) { return i == 0 ? c.r : i == 1 ? c.g : c.b; }

void checkUsable(const std::vector<Rgb>& v, const char* who) {
  if (v.size() < 64) throw std::invalid_argument(std::string("not enough picture in the ") + who + " region");
  double lo = 1e9, hi = -1e9;
  for (auto& c : v) {
    const double y = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    lo = std::min(lo, y);
    hi = std::max(hi, y);
  }
  if (hi - lo < 0.002) throw std::invalid_argument(std::string("the ") + who + " region is flat (one colour); pick a region with detail");
}

// Inverse-CDF tone curve, gamma domain, 256 points.
std::array<float, 256> toneCurve(const std::vector<float>& ref, const std::vector<float>& tgt) {
  auto cdf = [](const std::vector<float>& v) {
    std::array<double, 256> h{};
    for (float x : v) h[static_cast<size_t>(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f)] += 1;
    std::partial_sum(h.begin(), h.end(), h.begin());
    for (double& x : h) x /= h.back();
    return h;
  };
  const auto cr = cdf(ref), ct = cdf(tgt);
  std::array<float, 256> curve{};
  size_t j = 0;
  for (size_t i = 0; i < 256; ++i) {
    while (j < 255 && cr[j] < ct[i]) ++j;
    curve[i] = static_cast<float>(j) / 255.0f;
  }
  std::array<float, 256> smooth{};
  for (int i = 0; i < 256; ++i) {
    float s = 0;
    int n = 0;
    for (int k = std::max(0, i - 4); k <= std::min(255, i + 4); ++k, ++n) s += curve[static_cast<size_t>(k)];
    const float x = static_cast<float>(i) / 255.0f;
    smooth[static_cast<size_t>(i)] = std::clamp(s / static_cast<float>(n), x - 0.15f, x + 0.15f);
  }
  for (size_t i = 1; i < 256; ++i) smooth[i] = std::max(smooth[i], smooth[i - 1]);
  for (auto& v : smooth) v = std::clamp(v, 0.0f, 1.0f);
  return smooth;
}

float curveAt(const std::array<float, 256>& c, float x) {
  const float p = std::clamp(x, 0.0f, 1.0f) * 255.0f;
  const size_t i = std::min<size_t>(static_cast<size_t>(p), 254);
  const float t = p - static_cast<float>(i);
  return c[i] + (c[i + 1] - c[i]) * t;
}
}  // namespace

bool validRegion(const Region& r) {
  return r.x >= 0 && r.y >= 0 && r.w > 0.01f && r.h > 0.01f && r.x + r.w <= 1.0001f && r.y + r.h <= 1.0001f;
}

std::vector<Rgb> sampleRegion(const std::vector<CpuImage>& frames, const Region& r, int step) {
  std::vector<Rgb> out;
  for (auto& f : frames) {
    const int x0 = static_cast<int>(r.x * f.width), y0 = static_cast<int>(r.y * f.height);
    const int x1 = std::min<int>(static_cast<int>((r.x + r.w) * f.width), f.width);
    const int y1 = std::min<int>(static_cast<int>((r.y + r.h) * f.height), f.height);
    for (int y = y0; y < y1; y += step)
      for (int x = x0; x < x1; x += step) {
        const uint8_t* p = &f.bgra[(static_cast<size_t>(y) * f.width + x) * 4];
        out.push_back({toLinearLight(p[2] / 255.0f), toLinearLight(p[1] / 255.0f), toLinearLight(p[0] / 255.0f)});
      }
  }
  return out;
}

MatchResult solveColorMatch(const std::vector<Rgb>& reference, const std::vector<Rgb>& target, int lutSize) {
  checkUsable(reference, "reference");
  checkUsable(target, "target");
  MatchResult res;
  // 1. gains
  for (int c = 0; c < 3; ++c) {
    double mr = 0, mt = 0;
    for (auto& v : reference) mr += ch(v, c);
    for (auto& v : target) mt += ch(v, c);
    res.gains[static_cast<size_t>(c)] = mt > 1e-6 ? std::clamp(static_cast<float>(mr / mt), 0.25f, 4.0f) : 1.0f;
  }
  auto gain = [&](Rgb v) { return Rgb{v.r * res.gains[0], v.g * res.gains[1], v.b * res.gains[2]}; };
  // 2. Lab mean/spread
  std::vector<Lab> labR, labT;
  for (auto& v : reference) labR.push_back(linearToLab(v));
  for (auto& v : target) labT.push_back(linearToLab(gain(v)));
  const Stats sr = labStats(labR), st = labStats(labT);
  double scale[3];
  for (int i = 0; i < 3; ++i) scale[i] = st.sd[i] < 1e-4 ? 1.0 : std::clamp(sr.sd[i] / st.sd[i], 0.5, 2.0);
  auto transfer = [&](Rgb v) {
    Lab l = linearToLab(gain(v));
    l.L = (l.L - st.mean[0]) * scale[0] + sr.mean[0];
    l.a = (l.a - st.mean[1]) * scale[1] + sr.mean[1];
    l.b = (l.b - st.mean[2]) * scale[2] + sr.mean[2];
    Rgb o = labToLinear(l);
    return Rgb{std::clamp(o.r, 0.0f, 1.0f), std::clamp(o.g, 0.0f, 1.0f), std::clamp(o.b, 0.0f, 1.0f)};
  };
  // 3. tone curves in the gamma domain
  std::array<std::array<float, 256>, 3> curves{};
  for (int c = 0; c < 3; ++c) {
    std::vector<float> gr, gt;
    for (auto& v : reference) gr.push_back(toGamma(ch(v, c)));
    for (auto& v : target) gt.push_back(toGamma(ch(transfer(v), c)));
    curves[static_cast<size_t>(c)] = toneCurve(gr, gt);
  }
  auto full = [&](Rgb lin) {
    Rgb t = transfer(lin);
    return Rgb{curveAt(curves[0], toGamma(t.r)), curveAt(curves[1], toGamma(t.g)), curveAt(curves[2], toGamma(t.b))};
  };
  // 4. bake
  res.lut = Lut3D::identity(lutSize);
  for (auto& e : res.lut.data) e = full({toLinearLight(e.r), toLinearLight(e.g), toLinearLight(e.b)});
  // 5. residual on the means
  Rgb mr{}, mc{};
  for (auto& v : reference) {
    mr.r += v.r / reference.size();
    mr.g += v.g / reference.size();
    mr.b += v.b / reference.size();
  }
  for (auto& v : target) {
    auto g = res.lut.apply({toGamma(v.r), toGamma(v.g), toGamma(v.b)});
    mc.r += toLinearLight(g.r) / target.size();
    mc.g += toLinearLight(g.g) / target.size();
    mc.b += toLinearLight(g.b) / target.size();
  }
  res.residualDeltaE = deltaE2000(linearToLab(mr), linearToLab(mc));
  return res;
}
}  // namespace bc
```

- [ ] **Step 4: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "solveColorMatch*,sampleRegion*"`
Expected: 5 cases pass; the log prints "mean ΔE2000 before X, after Y" with Y < 2.

If the ΔE check fails, do **not** relax the bound (it is the spec's acceptance number). Investigate in this order and ledger each finding: (a) step 2 over-correcting — try the test with the Lab scale clamped to 1 and report both numbers; (b) the 9-bin smoothing — try 5; (c) histogram bin quantisation at the dark end. Report the numbers to the planner if none brings it under 2.

- [ ] **Step 5: Commit**

```bash
git add src/core tests CMakeLists.txt
git commit -m "feat(core): colour-match solver (gains, Lab transfer, tone curves) baked to a 33³ LUT"
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
  - Stage `color` (first in `stageSchemas()`): `enabled` bool false; `exposure` −3..3 step 0.05 = 0 (EV); `temperature` −100..100 step 1 = 0; `tint` −100..100 step 1 = 0; `lift` −0.2..0.2 step 0.005 = 0; `gamma` 0.5..2 step 0.01 = 1; `gain` 0.5..2 step 0.01 = 1; `saturation` 0..2 step 0.01 = 1; `lut` path hidden = ""; `previousLut` path hidden = "". Labels vi/en: stage "Chỉnh màu"/"Colour"; "Bật"/"Enabled"; "Phơi sáng (EV)"/"Exposure (EV)"; "Nhiệt độ màu"/"Temperature"; "Tint (xanh lá – tím)"/"Tint (green – magenta)"; "Lift"; "Gamma"; "Gain"; "Độ bão hoà"/"Saturation"; "LUT"; "LUT trước"/"Previous LUT".
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
  StageSchema color{"color", "Chỉnh màu", "Colour", {}};
  auto hiddenPath = [](std::string key, std::string vi, std::string en) {
    auto f = simple(std::move(key), "path", "", std::move(vi), std::move(en));
    f.hidden = true;
    return f;
  };
  color.fields = {
      simple("enabled", "bool", false, "Bật", "Enabled"),
      number("exposure", -3, 3, 0.05, 0, "Phơi sáng (EV)", "Exposure (EV)"),
      number("temperature", -100, 100, 1, 0, "Nhiệt độ màu", "Temperature"),
      number("tint", -100, 100, 1, 0, "Tint (xanh lá – tím)", "Tint (green – magenta)"),
      number("lift", -0.2, 0.2, 0.005, 0, "Lift", "Lift"),
      number("gamma", 0.5, 2, 0.01, 1, "Gamma", "Gamma"),
      number("gain", 0.5, 2, 0.01, 1, "Gain", "Gain"),
      number("saturation", 0, 2, 0.01, 1, "Độ bão hoà", "Saturation"),
      hiddenPath("lut", "LUT", "LUT"),
      hiddenPath("previousLut", "LUT trước", "Previous LUT"),
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
git commit -m "feat(gpu): colour stage schema and LUT + trims colour grader"
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
  - State: `/cameras/<id>/stages/color/{state,error}`; a LUT that cannot be read → `{code: "color-lut", message}` while trims keep running.
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
      lutError_ = ErrorInfo{"color-lut", std::string("the colour LUT cannot be read; using the trims only: ") + e.what()};
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

### Task 5: Colour-match service and the commands (match, import, undo, reset)

**Files:**
- Create: `src/engine/color_match_service.h`, `src/engine/color_match_service.cpp`
- Modify: `src/engine/command_router.h/.cpp` (`Effects` + 4 commands), `src/engine/engine_main.cpp`, `tests/command_router_test.cpp` (`FakeFx` gains the two new effects), `CMakeLists.txt`
- Test: `tests/color_match_service_test.cpp`, `tests/command_router_test.cpp` (new cases)

**Interfaces:**
- Consumes: `Engine::capture` (Task 4), `sampleRegion`, `solveColorMatch`, `Region`, `validRegion` (Task 2), `writeCube`, `readCube` (Task 1), `normalizeStageParams` (M2/Task 3).
- Produces:
  - `bc::ColorMatchService(Engine&, std::filesystem::path lutDir)`: `struct Outcome { std::string cameraId; std::string lutPath; double residualDeltaE; }`, `std::vector<Outcome> match(const std::string& referenceId, const std::vector<std::string>& targetIds, const std::map<std::string, Region>& regions)`, `std::string importCube(const std::string& cameraId, const std::filesystem::path& source)`. Both throw `std::invalid_argument` with an operator-readable message.
  - `CommandRouter::Effects` additions: `virtual nlohmann::json colorMatch(const std::string& referenceId, const std::vector<std::string>& targetIds, const nlohmann::json& regions) = 0;` (returns `[{cameraId, lutPath, deltaE}]`), `virtual std::string importCube(const std::string& cameraId, const std::string& path) = 0;`.
  - Commands:
    | cmd | args | effect | result |
    |---|---|---|---|
    | `colorMatch` | `{referenceId, targetIds:[...], regions?: {id: {x,y,w,h}}}` | per target: `previousLut = lut`, `lut = <new file>`, `enabled = true` | `[{cameraId, deltaE}]` |
    | `importLut` | `{cameraId, path}` | `previousLut = lut`, `lut = <copied file>`, `enabled = true` | `{}` |
    | `undoLut` | `{cameraId}` | swap `lut` ↔ `previousLut`; `invalid` "nothing to undo" when `previousLut` is empty | `{}` |
    | `resetColor` | `{cameraId}` | colour params back to defaults, keeping `previousLut = <old lut>` so reset can be undone | `{}` |

Validation (router, before any capture): `targetIds` 1..16 strings; every id exists; the reference is not a target; each region (if given) passes `validRegion`. The router does **not** hold the show lock while `colorMatch` captures and solves (~1.5 s); it re-takes the lock to store the results and drops results for cameras removed meanwhile.

LUT files: `<lutDir>\<cameraId>-<YYYYMMDD-HHMMSS>-<n>.cube`, where `lutDir = <show dir>\luts`. Imports are validated with `readCube` and re-written (normalised) to the same folder.

- [ ] **Step 1: Write the failing tests**

In `tests/command_router_test.cpp`, extend `FakeFx`:
```cpp
  int matches = 0;
  std::string importError;
  json colorMatch(const std::string&, const std::vector<std::string>& targets, const json&) override {
    ++matches;
    json out = json::array();
    for (auto& t : targets) out.push_back({{"cameraId", t}, {"lutPath", "C:/luts/" + t + "-" + std::to_string(matches) + ".cube"}, {"deltaE", 1.2}});
    return out;
  }
  std::string importCube(const std::string& cam, const std::string&) override {
    if (!importError.empty()) throw std::invalid_argument(importError);
    return "C:/luts/" + cam + "-imported.cube";
  }
```
and append:
```cpp
TEST_CASE("CommandRouter: colorMatch stores LUTs, undo swaps, reset clears") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto a = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "A"}, {"input", kSrt}}}})["result"]["id"];
  auto b = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "B"}, {"input", kSrt}}}})["result"]["id"];
  auto res = call(r, {{"cmd", "colorMatch"}, {"args", {{"referenceId", a}, {"targetIds", {b}}}}});
  REQUIRE(res["ok"] == true);
  CHECK(res["result"][0]["cameraId"] == b);
  CHECK(res["result"][0]["deltaE"] == 1.2);
  auto color = [&] { return r.show().findCamera(b.get<std::string>())->stages["color"]; };
  CHECK(color()["enabled"] == true);
  CHECK(color()["lut"] == "C:/luts/" + b.get<std::string>() + "-1.cube");
  call(r, {{"cmd", "colorMatch"}, {"args", {{"referenceId", a}, {"targetIds", {b}}}}});
  CHECK(color()["previousLut"] == "C:/luts/" + b.get<std::string>() + "-1.cube");
  REQUIRE(call(r, {{"cmd", "undoLut"}, {"args", {{"cameraId", b}}}})["ok"] == true);
  CHECK(color()["lut"] == "C:/luts/" + b.get<std::string>() + "-1.cube");
  REQUIRE(call(r, {{"cmd", "resetColor"}, {"args", {{"cameraId", b}}}})["ok"] == true);
  CHECK(color()["lut"] == "");
  CHECK(color()["enabled"] == false);
  CHECK(color()["previousLut"] == "C:/luts/" + b.get<std::string>() + "-1.cube");
}

TEST_CASE("CommandRouter: colorMatch and importLut validation") {
  FakeFx fx;
  bc::CommandRouter r({}, fx);
  auto a = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "A"}, {"input", kSrt}}}})["result"]["id"];
  CHECK(call(r, {{"cmd", "colorMatch"}, {"args", {{"referenceId", a}, {"targetIds", {a}}}}})["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "colorMatch"}, {"args", {{"referenceId", a}, {"targetIds", json::array()}}}})["error"]["code"] == "invalid");
  CHECK(call(r, {{"cmd", "colorMatch"}, {"args", {{"referenceId", a}, {"targetIds", {"nope"}}}}})["error"]["code"] == "not-found");
  auto b = call(r, {{"cmd", "addCamera"}, {"args", {{"name", "B"}, {"input", kSrt}}}})["result"]["id"];
  json badRegion = {{b.get<std::string>(), {{"x", 0.9}, {"y", 0}, {"w", 0.5}, {"h", 1}}}};
  CHECK(call(r, {{"cmd", "colorMatch"}, {"args", {{"referenceId", a}, {"targetIds", {b}}, {"regions", badRegion}}}})["error"]["code"] == "invalid");
  CHECK(fx.matches == 0);
  CHECK(call(r, {{"cmd", "undoLut"}, {"args", {{"cameraId", b}}}})["error"]["message"] == "nothing to undo");
  fx.importError = "line 3: expected three numbers";
  auto imp = call(r, {{"cmd", "importLut"}, {"args", {{"cameraId", b}, {"path", "C:/x.cube"}}}});
  CHECK(imp["error"]["code"] == "invalid");
  CHECK(imp["error"]["message"] == "line 3: expected three numbers");
  fx.importError.clear();
  REQUIRE(call(r, {{"cmd", "importLut"}, {"args", {{"cameraId", b}, {"path", "C:/x.cube"}}}})["ok"] == true);
  CHECK(r.show().findCamera(b.get<std::string>())->stages["color"]["lut"] == "C:/luts/" + b.get<std::string>() + "-imported.cube");
}
```

`tests/color_match_service_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <fstream>
#include <thread>

#include "core/color_math.h"
#include "core/stage_params.h"
#include "core/state_store.h"
#include "engine/color_match_service.h"
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
  bc::test::TempDir dir;
  bc::ColorMatchService svc{engine, dir.path / "luts"};
  ~Rig() { engine.stop(); }
};

bc::CameraConfig cam(const std::string& id, std::vector<int> bgra) {
  bc::CameraConfig c;
  c.id = id;
  c.name = id;
  c.input.kind = "fake";
  c.input.params = {{"tag", id}, {"bgra", bgra}, {"gradient", true}};
  c.outputs = {{"o-" + id, "fake", false, true, json::object()}};
  return c;
}

bc::Lab meanLab(bc::test::FakeOutput* o) {
  auto s = o->seen().back();
  return bc::linearToLab({bc::toLinearLight(s.centerPixel[2] / 255.f), bc::toLinearLight(s.centerPixel[1] / 255.f),
                          bc::toLinearLight(s.centerPixel[0] / 255.f)});
}
}  // namespace

TEST_CASE("ColorMatchService: a cast camera ends up close to the reference") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("ref", {60, 120, 200, 255}));
  show.cameras.push_back(cam("tgt", {90, 120, 170, 255}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  const double before = bc::deltaE2000(meanLab(r.factory.output("o-ref")), meanLab(r.factory.output("o-tgt")));
  auto out = r.svc.match("ref", {"tgt"}, {});
  REQUIRE(out.size() == 1);
  CHECK(std::filesystem::exists(out[0].lutPath));
  show.cameras[1].stages = {{"color", bc::normalizeStageParams("color", {{"enabled", true}, {"lut", out[0].lutPath}})}};
  r.engine.apply(show);
  std::this_thread::sleep_for(300ms);
  const double after = bc::deltaE2000(meanLab(r.factory.output("o-ref")), meanLab(r.factory.output("o-tgt")));
  MESSAGE("ΔE2000 at the centre: before " << before << ", after " << after);
  CHECK(after < 3.0);
  CHECK(after < before);
}

TEST_CASE("ColorMatchService: refuses a camera without picture and a bad .cube import") {
  Rig r;
  bc::Show show;
  show.cameras.push_back(cam("ref", {60, 120, 200, 255}));
  show.cameras.push_back(cam("tgt", {90, 120, 170, 255}));
  r.engine.apply(show);
  r.engine.start();
  r.factory.input("tgt")->paused = true;
  std::this_thread::sleep_for(5600ms);
  CHECK_THROWS_AS(r.svc.match("ref", {"tgt"}, {}), std::invalid_argument);
  CHECK_FALSE(std::filesystem::exists(r.dir.path / "luts") && !std::filesystem::is_empty(r.dir.path / "luts"));
  std::ofstream(r.dir.path / "bad.cube") << "LUT_3D_SIZE 2\n0 0 0\n";
  CHECK_THROWS_AS(r.svc.importCube("ref", r.dir.path / "bad.cube"), std::invalid_argument);
}
```

Add both new test files to `bc_tests`, `src/engine/color_match_service.cpp` to `bc_engine`.

- [ ] **Step 2: Run to verify failure**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "ColorMatchService*,CommandRouter*"`
Expected: build FAILS.

- [ ] **Step 3: Implement the service**

`src/engine/color_match_service.h`:
```cpp
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "core/color_match.h"
#include "engine/engine.h"

namespace bc {
// spec §5.3: capture ~1 s, solve off the tick thread, write one LUT per target.
class ColorMatchService {
 public:
  struct Outcome {
    std::string cameraId;
    std::string lutPath;
    double residualDeltaE = 0;
  };
  ColorMatchService(Engine& engine, std::filesystem::path lutDir) : engine_(engine), lutDir_(std::move(lutDir)) {}
  std::vector<Outcome> match(const std::string& referenceId, const std::vector<std::string>& targetIds,
                             const std::map<std::string, Region>& regions);
  std::string importCube(const std::string& cameraId, const std::filesystem::path& source);

 private:
  std::filesystem::path newLutPath(const std::string& cameraId);
  Engine& engine_;
  std::filesystem::path lutDir_;
  int counter_ = 0;
};
}  // namespace bc
```

`src/engine/color_match_service.cpp`:
```cpp
#include "engine/color_match_service.h"

#include <chrono>
#include <ctime>
#include <stdexcept>

#include "core/lut.h"

namespace bc {
std::filesystem::path ColorMatchService::newLutPath(const std::string& cameraId) {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  localtime_s(&tm, &now);
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  return lutDir_ / (cameraId + "-" + stamp + "-" + std::to_string(++counter_) + ".cube");
}

std::vector<ColorMatchService::Outcome> ColorMatchService::match(const std::string& ref, const std::vector<std::string>& targets,
                                                                 const std::map<std::string, Region>& regions) {
  std::vector<std::string> ids{ref};
  ids.insert(ids.end(), targets.begin(), targets.end());
  std::map<std::string, std::vector<CpuImage>> frames;
  try {
    frames = engine_.capture(ids);
  } catch (const std::invalid_argument&) {
    throw;
  } catch (const std::exception& e) {
    throw std::invalid_argument(std::string("could not capture the cameras: ") + e.what());
  }
  auto regionOf = [&](const std::string& id) {
    auto it = regions.find(id);
    return it == regions.end() ? Region{} : it->second;
  };
  const auto refSamples = sampleRegion(frames[ref], regionOf(ref));
  // Solve everything first, write files only when every target solved.
  std::vector<std::pair<std::string, MatchResult>> solved;
  for (auto& t : targets) {
    try {
      solved.emplace_back(t, solveColorMatch(refSamples, sampleRegion(frames[t], regionOf(t))));
    } catch (const std::invalid_argument& e) {
      throw std::invalid_argument("camera " + t + ": " + e.what());
    }
  }
  std::vector<Outcome> out;
  for (auto& [id, res] : solved) {
    const auto path = newLutPath(id);
    writeCube(path, res.lut, "BreadCamera match to " + ref);
    out.push_back({id, path.string(), res.residualDeltaE});
  }
  return out;
}

std::string ColorMatchService::importCube(const std::string& cameraId, const std::filesystem::path& source) {
  Lut3D lut;
  try {
    lut = readCube(source);
  } catch (const CubeError& e) {
    throw std::invalid_argument(e.what());
  }
  const auto path = newLutPath(cameraId);
  writeCube(path, lut, "imported " + source.filename().string());
  return path.string();
}
}  // namespace bc
```

- [ ] **Step 4: Router commands and effects**

`src/engine/command_router.h` — add the two pure virtuals to `Effects` (see Interfaces).

`src/engine/command_router.cpp` — `#include "core/color_match.h"`; in `dispatch`, **before** `std::lock_guard l(m_);` handle `colorMatch`:
```cpp
  if (cmd == "colorMatch") {
    const auto ref = str(args, "referenceId");
    if (!args.contains("targetIds") || !args["targetIds"].is_array()) throw CmdError("bad-request", "targetIds must be a list");
    std::vector<std::string> targets;
    for (auto& t : args["targetIds"]) {
      if (!t.is_string()) throw CmdError("bad-request", "targetIds must be text");
      targets.push_back(t.get<std::string>());
    }
    if (targets.empty() || targets.size() > 16) throw CmdError("invalid", "choose 1 to 16 target cameras");
    if (std::find(targets.begin(), targets.end(), ref) != targets.end()) throw CmdError("invalid", "the reference camera cannot also be a target");
    const json regions = args.value("regions", json::object());
    if (!regions.is_object()) throw CmdError("bad-request", "regions must be an object");
    {
      std::lock_guard l(m_);
      for (auto& id : std::vector<std::string>{ref}) if (!show_.findCamera(id)) throw CmdError("not-found", "no camera " + id);
      for (auto& id : targets) if (!show_.findCamera(id)) throw CmdError("not-found", "no camera " + id);
    }
    for (auto& [id, rj] : regions.items()) {
      if (!rj.is_object()) throw CmdError("bad-request", "a region must be an object");
      Region rg{rj.value("x", 0.0f), rj.value("y", 0.0f), rj.value("w", 1.0f), rj.value("h", 1.0f)};
      if (!validRegion(rg)) throw CmdError("invalid", "the region for camera " + id + " is outside the picture");
    }
    json outcomes;
    try {
      outcomes = fx_.colorMatch(ref, targets, regions);  // ~1.5 s, outside the show lock
    } catch (const std::invalid_argument& e) {
      throw CmdError("invalid", e.what());
    }
    std::lock_guard l(m_);
    Show next = show_;
    json result = json::array();
    for (auto& o : outcomes) {
      auto* c = next.findCamera(o["cameraId"].get<std::string>());
      if (!c) continue;  // removed while we were matching
      auto color = normalizeStageParams("color", c->stages.value("color", json::object()));
      color["previousLut"] = color["lut"];
      color["lut"] = o["lutPath"];
      color["enabled"] = true;
      c->stages["color"] = color;
      result.push_back({{"cameraId", o["cameraId"]}, {"deltaE", o["deltaE"]}});
    }
    show_ = std::move(next);
    fx_.showChanged(show_);
    return result;
  }
```
and inside the locked branch chain:
```cpp
  } else if (cmd == "importLut") {
    auto& c = findCam(str(args, "cameraId"));
    std::string path;
    try {
      path = fx_.importCube(c.id, str(args, "path"));
    } catch (const std::invalid_argument& e) {
      throw CmdError("invalid", e.what());
    }
    auto color = normalizeStageParams("color", c.stages.value("color", json::object()));
    color["previousLut"] = color["lut"];
    color["lut"] = path;
    color["enabled"] = true;
    c.stages["color"] = color;
  } else if (cmd == "undoLut") {
    auto& c = findCam(str(args, "cameraId"));
    auto color = normalizeStageParams("color", c.stages.value("color", json::object()));
    if (color["previousLut"] == "") throw CmdError("invalid", "nothing to undo");
    std::swap(color["lut"], color["previousLut"]);
    color["enabled"] = true;
    c.stages["color"] = color;
  } else if (cmd == "resetColor") {
    auto& c = findCam(str(args, "cameraId"));
    auto old = normalizeStageParams("color", c.stages.value("color", json::object()));
    auto color = normalizeStageParams("color", json::object());
    color["previousLut"] = old["lut"];
    c.stages["color"] = color;
```
Note `importLut` calls `fx_.importCube` under the show lock — it only reads and writes one small file, which is fine.

`src/engine/engine_main.cpp` — create the service after the engine: `bc::ColorMatchService colorMatch(engine, showDir / "luts");` and add to `Fx` (constructor gains a `bc::ColorMatchService&`):
```cpp
    json colorMatch(const std::string& ref, const std::vector<std::string>& targets, const json& regions) override {
      std::map<std::string, bc::Region> rs;
      for (auto& [id, r] : regions.items()) rs[id] = {r.value("x", 0.0f), r.value("y", 0.0f), r.value("w", 1.0f), r.value("h", 1.0f)};
      json out = json::array();
      for (auto& o : matcher.match(ref, targets, rs)) out.push_back({{"cameraId", o.cameraId}, {"lutPath", o.lutPath}, {"deltaE", o.residualDeltaE}});
      return out;
    }
    std::string importCube(const std::string& cameraId, const std::string& path) override {
      return matcher.importCube(cameraId, std::filesystem::u8path(path));
    }
```
(member `bc::ColorMatchService& matcher;`).

- [ ] **Step 5: Run the tests**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "ColorMatchService*,CommandRouter*"`
Expected: all pass; the service test logs "ΔE2000 at the centre: before X, after Y" with Y < 3.

- [ ] **Step 6: Commit**

```bash
git add src tests CMakeLists.txt
git commit -m "feat(engine): colour-match service with LUT files; colorMatch/importLut/undoLut/resetColor"
```

### Task 6: UI — colour match screen with region boxes, trims, import, undo, reset

**Files:**
- Create: `ui/src/components/RegionPicker.tsx`, `ui/src/screens/ColorMatch.tsx`
- Modify: `ui/src/components/StageForm.tsx` (skip hidden fields), `ui/src/api/protocol.ts` (`StageField.hidden`, `Region`), `ui/src/i18n.ts` (keys), `ui/src/App.tsx` (navigation)
- Test: `ui/src/components/RegionPicker.test.tsx`, `ui/src/screens/ColorMatch.test.tsx`

**Interfaces:**
- Consumes: commands `colorMatch`, `importLut`, `undoLut`, `resetColor`, `getStageSchemas`, `setStage` (Tasks 3, 5; M2); `StageForm`, `ConfirmDialog`, `ErrorBlock`, `PreviewCanvas` (M1/M2).
- Produces: `RegionPicker({value, onChange, children})` (mouse-drag rectangle, normalised `{x,y,w,h}`, `null` when cleared), `ColorMatch({client})`; the header gains two nav buttons "Multiview" and "Khớp màu".

Copy (vi / en): nav "Multiview" / "Multiview", "Khớp màu" / "Colour match"; "Tham chiếu" / "Reference"; "Khớp theo tham chiếu" / "Match to reference"; primary button "Khớp màu" / "Match", busy "Đang khớp…" / "Matching…"; "ΔE còn lại" / "Remaining ΔE"; "Tinh chỉnh" / "Trims"; "Hoàn tác" / "Undo"; "Đặt lại màu" / "Reset colour" with body "Bỏ LUT đã khớp và mọi tinh chỉnh của camera này?" / "Drop the matched LUT and all trims for this camera?"; "Đường dẫn file .cube" / ".cube file path"; "Nhập LUT" / "Import LUT"; "Xoá vùng" / "Clear region"; hint "Kéo chuột trên hình để đánh dấu nội dung chung (mặt, áo, thẻ xám)." / "Drag on the picture to mark shared content (face, shirt, grey card)."

- [ ] **Step 1: Write the failing tests**

`ui/src/components/RegionPicker.test.tsx`:
```tsx
import { fireEvent, render, screen } from '@testing-library/react';

import { RegionPicker } from './RegionPicker';

test('dragging draws a normalised region', () => {
  const onChange = vi.fn();
  render(
    <RegionPicker value={null} onChange={onChange}>
      <div style={{ width: 200, height: 100 }} />
    </RegionPicker>,
  );
  const area = screen.getByTestId('region-area');
  area.getBoundingClientRect = () => ({ left: 0, top: 0, width: 200, height: 100, right: 200, bottom: 100, x: 0, y: 0, toJSON: () => ({}) });
  fireEvent.mouseDown(area, { clientX: 20, clientY: 10 });
  fireEvent.mouseMove(area, { clientX: 120, clientY: 60 });
  fireEvent.mouseUp(area, { clientX: 120, clientY: 60 });
  expect(onChange).toHaveBeenCalledWith({ x: 0.1, y: 0.1, w: 0.5, h: 0.5 });
});

test('a click without a drag does not set a region; clear resets it', () => {
  const onChange = vi.fn();
  render(
    <RegionPicker value={{ x: 0.1, y: 0.1, w: 0.5, h: 0.5 }} onChange={onChange}>
      <div />
    </RegionPicker>,
  );
  const area = screen.getByTestId('region-area');
  area.getBoundingClientRect = () => ({ left: 0, top: 0, width: 200, height: 100, right: 200, bottom: 100, x: 0, y: 0, toJSON: () => ({}) });
  fireEvent.mouseDown(area, { clientX: 20, clientY: 10 });
  fireEvent.mouseUp(area, { clientX: 21, clientY: 10 });
  expect(onChange).not.toHaveBeenCalled();
  fireEvent.click(screen.getByRole('button', { name: 'Xoá vùng' }));
  expect(onChange).toHaveBeenCalledWith(null);
});
```

`ui/src/screens/ColorMatch.test.tsx`:
```tsx
import { fireEvent, render, screen, waitFor } from '@testing-library/react';

import { EngineClient } from '../api/client';
import { FakeWs } from '../test/fakeWs';
import { ColorMatch } from './ColorMatch';

test('sends the match with reference and targets, then shows the remaining ΔE', async () => {
  FakeWs.all = [];
  const c = new EngineClient('ws://x', (u) => new FakeWs(u) as unknown as WebSocket);
  c.connect();
  FakeWs.last.open();
  FakeWs.last.receive({
    type: 'snapshot',
    state: { cameras: { a: { name: 'A', phase: 'live' }, b: { name: 'B', phase: 'live' } } },
    show: {
      cameras: [
        { id: 'a', name: 'A', input: { kind: 'srt', params: {} }, outputs: [], stages: {} },
        { id: 'b', name: 'B', input: { kind: 'srt', params: {} }, outputs: [], stages: {} },
      ],
    },
  });
  render(<ColorMatch client={c} />);
  fireEvent.click(screen.getByLabelText('Khớp theo tham chiếu: B'));
  fireEvent.click(screen.getByRole('button', { name: 'Khớp màu' }));
  const req = FakeWs.last.sent.map((s) => JSON.parse(s)).find((r) => r.cmd === 'colorMatch');
  expect(req.args).toEqual({ referenceId: 'a', targetIds: ['b'], regions: {} });
  FakeWs.last.receive({ id: req.id, ok: true, result: [{ cameraId: 'b', deltaE: 1.4 }] });
  await waitFor(() => expect(screen.getByText(/1\.4/)).toBeInTheDocument());
});
```

Run `npm test` in `ui/` — Expected: FAIL.

- [ ] **Step 2: Types, strings, hidden fields**

`protocol.ts`: add `hidden?: boolean;` to `StageField`; add `export interface Region { x: number; y: number; w: number; h: number }`.

`i18n.ts`: add the keys `'nav.multiview'`, `'nav.color'`, `'color.reference'`, `'color.target'`, `'color.match'`, `'color.matching'`, `'color.residual'`, `'color.trims'`, `'color.undo'`, `'color.reset'`, `'color.resetBody'`, `'color.importPath'`, `'color.import'`, `'region.clear'`, `'region.hint'` to `vi` and `en` with the copy above.

`StageForm.tsx`: iterate `schema.fields.filter((f) => !f.hidden)` instead of `schema.fields`.

- [ ] **Step 3: Implement `RegionPicker`**

`ui/src/components/RegionPicker.tsx`:
```tsx
import { Box, Button } from '@mui/material';
import { useRef, useState, type ReactNode } from 'react';

import type { Region } from '../api/protocol';
import { t } from '../i18n';

const round = (v: number) => Math.round(v * 1000) / 1000;

// Drag a box over the picture; values are normalised 0..1 (spec §5.3 region boxes).
export function RegionPicker({ value, onChange, children }: { value: Region | null; onChange: (r: Region | null) => void; children: ReactNode }) {
  const area = useRef<HTMLDivElement>(null);
  const [drag, setDrag] = useState<{ x0: number; y0: number; x1: number; y1: number } | null>(null);
  const norm = (e: { clientX: number; clientY: number }) => {
    const r = area.current!.getBoundingClientRect();
    return { x: Math.min(Math.max((e.clientX - r.left) / r.width, 0), 1), y: Math.min(Math.max((e.clientY - r.top) / r.height, 0), 1) };
  };
  const shown = drag
    ? { x: Math.min(drag.x0, drag.x1), y: Math.min(drag.y0, drag.y1), w: Math.abs(drag.x1 - drag.x0), h: Math.abs(drag.y1 - drag.y0) }
    : value;
  return (
    <Box>
      <Box
        ref={area}
        data-testid="region-area"
        sx={{ position: 'relative', cursor: 'crosshair', userSelect: 'none' }}
        onMouseDown={(e) => {
          const p = norm(e);
          setDrag({ x0: p.x, y0: p.y, x1: p.x, y1: p.y });
        }}
        onMouseMove={(e) => {
          if (!drag) return;
          const p = norm(e);
          setDrag({ ...drag, x1: p.x, y1: p.y });
        }}
        onMouseUp={(e) => {
          if (!drag) return;
          const p = norm(e);
          const r = { x: Math.min(drag.x0, p.x), y: Math.min(drag.y0, p.y), w: Math.abs(p.x - drag.x0), h: Math.abs(p.y - drag.y0) };
          setDrag(null);
          if (r.w > 0.02 && r.h > 0.02) onChange({ x: round(r.x), y: round(r.y), w: round(r.w), h: round(r.h) });
        }}
      >
        {children}
        {shown && (
          <Box
            sx={{
              position: 'absolute',
              left: `${shown.x * 100}%`,
              top: `${shown.y * 100}%`,
              width: `${shown.w * 100}%`,
              height: `${shown.h * 100}%`,
              border: 2,
              borderColor: 'warning.main',
              pointerEvents: 'none',
            }}
          />
        )}
      </Box>
      <Button size="small" disabled={!value} onClick={() => onChange(null)}>
        {t('region.clear')}
      </Button>
    </Box>
  );
}
```

- [ ] **Step 4: Implement the screen**

`ui/src/screens/ColorMatch.tsx`:
```tsx
import { Box, Button, Card, CardContent, Checkbox, FormControlLabel, Radio, Stack, TextField, Typography } from '@mui/material';
import { useEffect, useMemo, useState } from 'react';

import type { EngineClient } from '../api/client';
import type { Region, StageSchemaJson } from '../api/protocol';
import { useEngine } from '../api/useEngine';
import { ConfirmDialog } from '../components/ConfirmDialog';
import { ErrorBlock } from '../components/ErrorBlock';
import { RegionPicker } from '../components/RegionPicker';
import { StageForm } from '../components/StageForm';
import { t } from '../i18n';
import { PreviewCanvas } from '../preview/PreviewCanvas';

export function ColorMatch({ client }: { client: EngineClient }) {
  const { show, state } = useEngine(client);
  const cameras = show?.cameras ?? [];
  const [reference, setReference] = useState<string>('');
  const [targets, setTargets] = useState<string[]>([]);
  const [regions, setRegions] = useState<Record<string, Region>>({});
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [results, setResults] = useState<{ cameraId: string; deltaE: number }[]>([]);
  const [schema, setSchema] = useState<StageSchemaJson | null>(null);
  const [lutPath, setLutPath] = useState<Record<string, string>>({});
  const [resetFor, setResetFor] = useState<string | null>(null);
  const ref = reference || cameras[0]?.id || '';
  const ids = useMemo(() => cameras.map((c) => c.id), [cameras]);

  useEffect(() => client.setPreviewSubscription(ids, 'small'), [client, ids]);
  useEffect(() => {
    client
      .request<StageSchemaJson[]>('getStageSchemas')
      .then((all) => setSchema(all.find((s) => s.kind === 'color') ?? null))
      .catch(() => setSchema(null));
  }, [client]);

  async function match() {
    setBusy(true);
    setError('');
    try {
      const regs: Record<string, Region> = {};
      for (const id of [ref, ...targets]) if (regions[id]) regs[id] = regions[id];
      setResults(await client.request<{ cameraId: string; deltaE: number }[]>('colorMatch', { referenceId: ref, targetIds: targets, regions: regs }));
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setBusy(false);
    }
  }

  const run = (cmd: string, args: Record<string, unknown>) => client.request(cmd, args).catch((e: Error) => setError(e.message));

  return (
    <Box sx={{ p: 2 }}>
      <Stack direction="row" justifyContent="space-between" alignItems="center" sx={{ mb: 1 }}>
        <Typography variant="h4">{t('nav.color')}</Typography>
        <Button variant="contained" disabled={busy || targets.length === 0} onClick={match}>
          {busy ? t('color.matching') : t('color.match')}
        </Button>
      </Stack>
      <Typography variant="body2" color="text.secondary" sx={{ mb: 1 }}>
        {t('region.hint')}
      </Typography>
      {error && <ErrorBlock message={error} />}
      {results.map((r) => (
        <Typography key={r.cameraId} variant="body2">
          {cameras.find((c) => c.id === r.cameraId)?.name ?? r.cameraId}: {t('color.residual')} {r.deltaE.toFixed(1)}
        </Typography>
      ))}
      <Box sx={{ display: 'grid', gap: 2, mt: 2, gridTemplateColumns: 'repeat(auto-fill, minmax(360px, 1fr))' }}>
        {cameras.map((cam) => {
          const isRef = cam.id === ref;
          const colorErr = state.cameras?.[cam.id]?.stages?.color?.error;
          return (
            <Card key={cam.id} variant="outlined">
              <RegionPicker value={regions[cam.id] ?? null} onChange={(r) => setRegions((m) => { const n = { ...m }; if (r) n[cam.id] = r; else delete n[cam.id]; return n; })}>
                <PreviewCanvas client={client} cameraId={cam.id} />
              </RegionPicker>
              <CardContent>
                <Typography variant="h6">{cam.name}</Typography>
                <FormControlLabel
                  control={<Radio checked={isRef} onChange={() => { setReference(cam.id); setTargets((ts) => ts.filter((x) => x !== cam.id)); }} />}
                  label={t('color.reference')}
                />
                <FormControlLabel
                  disabled={isRef}
                  control={
                    <Checkbox
                      checked={targets.includes(cam.id)}
                      inputProps={{ 'aria-label': `${t('color.target')}: ${cam.name}` }}
                      onChange={(e) => setTargets((ts) => (e.target.checked ? [...ts, cam.id] : ts.filter((x) => x !== cam.id)))}
                    />
                  }
                  label={t('color.target')}
                />
                {colorErr && <ErrorBlock message={colorErr.message} />}
                <Typography variant="subtitle2" sx={{ mt: 1 }}>
                  {t('color.trims')}
                </Typography>
                {schema && (
                  <StageForm schema={schema} values={cam.stages?.color ?? {}} onChange={(p) => run('setStage', { cameraId: cam.id, stage: 'color', params: p })} />
                )}
                <Stack direction="row" spacing={1} sx={{ mt: 1 }}>
                  <Button size="small" onClick={() => run('undoLut', { cameraId: cam.id })}>
                    {t('color.undo')}
                  </Button>
                  <Button size="small" color="error" onClick={() => setResetFor(cam.id)}>
                    {t('color.reset')}
                  </Button>
                </Stack>
                <Stack direction="row" spacing={1} sx={{ mt: 1 }}>
                  <TextField size="small" label={t('color.importPath')} value={lutPath[cam.id] ?? ''} onChange={(e) => setLutPath((m) => ({ ...m, [cam.id]: e.target.value }))} sx={{ flex: 1 }} />
                  <Button size="small" disabled={!lutPath[cam.id]} onClick={() => run('importLut', { cameraId: cam.id, path: lutPath[cam.id] })}>
                    {t('color.import')}
                  </Button>
                </Stack>
              </CardContent>
            </Card>
          );
        })}
      </Box>
      <ConfirmDialog
        open={resetFor !== null}
        title={t('color.reset')}
        body={t('color.resetBody')}
        confirmLabel={t('color.reset')}
        onClose={() => setResetFor(null)}
        onConfirm={() => {
          if (resetFor) run('resetColor', { cameraId: resetFor });
          setResetFor(null);
        }}
      />
    </Box>
  );
}
```
Note: trims go out per change without debounce here (sliders are coarse); if the reviewer flags request floods, reuse CameraDetail's 150 ms debounce pattern.

`ui/src/App.tsx`: replace the `selected` state with `const [view, setView] = useState<'multiview' | 'color'>('multiview');` alongside `selected`; add two header buttons before the GPU text:
```tsx
        <Button onClick={() => { setSelected(null); setView('multiview'); }}>{t('nav.multiview')}</Button>
        <Button onClick={() => { setSelected(null); setView('color'); }}>{t('nav.color')}</Button>
```
and render `view === 'color' ? <ColorMatch client={client} /> : selected ? <CameraDetail .../> : <Multiview .../>` (import `Button`, `ColorMatch`).

- [ ] **Step 5: Run the UI tests and build**

In `ui/`: `npm test` (all pass) and `npm run build`.

- [ ] **Step 6: Commit**

```bash
git add ui/src
git commit -m "feat(ui): colour match screen with region boxes, trims, LUT import, undo and reset"
```

### Task 7: M3 acceptance — two SRT cameras with a colour cast, matched end to end; follow-ups

**Files:**
- Modify: `tests/support/test_media.h/.cpp` (`cbShift`, `crShift`; constant chroma), `docs/superpowers/follow-ups.md`, `CMakeLists.txt`
- Create: `tests/e2e_m3_test.cpp`

**Interfaces:**
- Consumes: `ColorMatchService` (Task 5), `Engine` with `GpuTools` (Task 4), `writeTestTs`/`TsPusher`, `HybridFactory` pattern (M1/M2 acceptance — re-declare locally).
- Produces: spec §9 M3 evidence ("matching works on real cameras" — stood in by two real SRT/decoded streams with a known chroma cast; a person still has to try two physical cameras, see Step 4).

- [ ] **Step 1: Give the test clip a controllable cast**

`tests/support/test_media.h`: `void writeTestTs(const std::filesystem::path& path, int width, int height, int fps, int seconds, bool withAudio = true, int cbShift = 0, int crShift = 0);`

`tests/support/test_media.cpp`: same parameters; replace the chroma fill with constant planes:
```cpp
    const auto u = static_cast<uint8_t>(std::clamp(128 + cbShift, 16, 240));
    const auto v = static_cast<uint8_t>(std::clamp(128 + crShift, 16, 240));
    for (int y = 0; y < height / 2; ++y)
      for (int x = 0; x < width / 2; ++x) {
        vf->data[1][y * vf->linesize[1] + x] = u;
        vf->data[2][y * vf->linesize[2] + x] = v;
      }
```
(`#include <algorithm>`). Luma still moves, so every earlier test that needs motion still has it. Run the M1/M2 suites once to confirm nothing depended on moving chroma.

- [ ] **Step 2: Write the acceptance test**

`tests/e2e_m3_test.cpp`:
```cpp
#include <doctest/doctest.h>

#include <thread>

#include "core/color_math.h"
#include "core/stage_params.h"
#include "core/state_store.h"
#include "engine/color_match_service.h"
#include "engine/engine.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "support/fake_input.h"
#include "support/requires.h"
#include "support/temp_dir.h"
#include "support/test_media.h"

using namespace std::chrono_literals;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {
struct HybridFactory : bc::AdapterFactory {
  bc::DefaultAdapterFactory real;
  bc::test::FakeFactory fake;
  HybridFactory(bc::GpuDevice& g, bc::TexturePool& p, bc::Converter& c) : real(g, p), fake(c) {}
  std::unique_ptr<bc::Input> makeInput(const bc::InputConfig& cfg) override { return real.makeInput(cfg); }
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig& cfg, const bc::CameraConfig& cam) override {
    return fake.makeOutput(cfg, cam);
  }
};

// Mean colour of the centre pixel over the last `window` of output frames, as Lab.
bc::Lab meanCentre(bc::test::FakeOutput* o, std::chrono::milliseconds window) {
  const auto since = Clock::now() - window;
  double r = 0, g = 0, b = 0;
  int n = 0;
  for (auto& s : o->seen()) {
    if (s.at < since) continue;
    r += bc::toLinearLight(s.centerPixel[2] / 255.f);
    g += bc::toLinearLight(s.centerPixel[1] / 255.f);
    b += bc::toLinearLight(s.centerPixel[0] / 255.f);
    ++n;
  }
  return bc::linearToLab({static_cast<float>(r / n), static_cast<float>(g / n), static_cast<float>(b / n)});
}
}  // namespace

TEST_CASE("M3 acceptance: a chroma-cast SRT camera is matched to the reference" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  bc::test::TempDir dir;
  bc::test::writeTestTs(dir.path / "ref.ts", 1280, 720, 60, 6, true, 0, 0);
  bc::test::writeTestTs(dir.path / "cast.ts", 1280, 720, 60, 6, true, 22, -18);
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::Compositor comp(*gpu, pool);
  bc::ColorGrader grader(*gpu, pool);
  bc::StateStore state;
  HybridFactory factory(*gpu, pool, conv);
  bc::Engine engine(bc::GpuTools{conv, comp, grader}, state, factory);
  bc::ColorMatchService svc(engine, dir.path / "luts");

  bc::Show show;
  for (int i = 0; i < 2; ++i) {
    bc::CameraConfig c;
    c.id = i == 0 ? "ref" : "cast";
    c.name = c.id;
    c.input.kind = "srt";
    c.input.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47941 + i}};
    c.outputs.push_back({"o-" + c.id, "fake", false, true, {}});
    show.cameras.push_back(c);
  }
  engine.apply(show);
  engine.start();
  bc::test::TsPusher p0(dir.path / "ref.ts", "srt://127.0.0.1:47941?mode=caller", true);
  bc::test::TsPusher p1(dir.path / "cast.ts", "srt://127.0.0.1:47942?mode=caller", true);
  std::this_thread::sleep_for(8s);  // connect + one full luma cycle

  const double before = bc::deltaE2000(meanCentre(factory.fake.output("o-ref"), 4s), meanCentre(factory.fake.output("o-cast"), 4s));
  auto out = svc.match("ref", {"cast"}, {});
  REQUIRE(out.size() == 1);
  show.cameras[1].stages = {{"color", bc::normalizeStageParams("color", {{"enabled", true}, {"lut", out[0].lutPath}})}};
  engine.apply(show);
  std::this_thread::sleep_for(5s);
  const double after = bc::deltaE2000(meanCentre(factory.fake.output("o-ref"), 4s), meanCentre(factory.fake.output("o-cast"), 4s));
  MESSAGE("SRT cameras, mean centre ΔE2000: before " << before << ", after " << after << ", solver residual " << out[0].residualDeltaE);
  CHECK(before > 5.0);
  CHECK(after < 3.0);
  p0.stop();
  p1.stop();
  engine.stop();
}
```

Add it to `bc_tests`.

- [ ] **Step 3: Run it**

Run: `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test.ps1 -Filter "M3 acceptance*"`
Expected: PASS with the before/after line in the log (paste it into the report). If `after` is ≥ 3, report the numbers; do not relax the bound.

- [ ] **Step 4: Follow-ups and the human check**

Append to `docs/superpowers/follow-ups.md`:
```markdown
## M3
- Real-camera colour match is a **human check**: point two physical cameras at the same scene, Match, and judge by eye (spec §9 M3 "matching works on real cameras"). Automated evidence uses two synthetic SRT streams (`tests/e2e_m3_test.cpp`).
- Scopes (waveform/vectorscope) are still out of scope; they would make checking a match easier (spec §10).
- The colour stage budget is a fixed 4 ms (`src/engine/camera.cpp`, `beginTick`).
- Colour trims from the colour-match screen are sent per change without debounce (`ui/src/screens/ColorMatch.tsx`).
```
Add any `ponytail:` comment or deferred minor from this run.

- [ ] **Step 5: Full verification**

1. `scripts/test.ps1` (all) — record passed/skipped and reasons.
2. `ui/`: `npm test`, `npm run build`.
3. `BC_FORCE_WARP=1` run — record.

- [ ] **Step 6: Commit**

```bash
git add tests docs/superpowers/follow-ups.md CMakeLists.txt
git commit -m "test: M3 acceptance (SRT cameras matched end to end) and follow-ups"
```
