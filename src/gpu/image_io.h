#pragma once
#include <filesystem>

#include "gpu/converter.h"

namespace bc {
// Loads PNG/JPEG/BMP through WIC as BGRA8. Throws std::runtime_error.
CpuImage loadImageBgra(const std::filesystem::path& path);
}  // namespace bc
