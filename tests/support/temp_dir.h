#pragma once
#include <filesystem>
#include <random>
#include <string>

namespace bc::test {
// A unique folder under %TEMP%, removed on destruction.
struct TempDir {
  std::filesystem::path path;
  TempDir() {
    std::random_device rd;
    path = std::filesystem::temp_directory_path() / ("bc-test-" + std::to_string(rd()));
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};
}  // namespace bc::test
