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
