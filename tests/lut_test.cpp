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

TEST_CASE("Lut3D: identity refuses a degenerate size") {
  CHECK_THROWS_AS(bc::Lut3D::identity(1), std::invalid_argument);
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
