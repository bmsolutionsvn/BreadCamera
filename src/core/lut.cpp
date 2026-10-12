#include "core/lut.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace bc {
Lut3D Lut3D::identity(int size) {
  if (size < 2 || size > 65) throw std::invalid_argument("LUT size must be 2 to 65");
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
  if (size < 2 || data.empty()) throw std::invalid_argument("LUT is not initialised");
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
  if (lut.size < 2 || lut.data.size() != static_cast<size_t>(lut.size) * lut.size * lut.size)
    throw CubeError("the LUT's size does not match its data");
  std::filesystem::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::trunc);
  if (!f) throw CubeError("cannot write " + path.string());
  f << "TITLE \"" << title << "\"\nLUT_3D_SIZE " << lut.size << "\n" << std::fixed << std::setprecision(6);
  for (auto& c : lut.data) f << c.r << ' ' << c.g << ' ' << c.b << '\n';
  if (!f) throw CubeError("cannot write " + path.string());
}
}  // namespace bc
