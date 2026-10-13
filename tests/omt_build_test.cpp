#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_OMT
#include <string>
#include <vector>

#include "omthx/omthx.h"

TEST_CASE("omthx is linked and discovery runs without a transmitter" BC_REQUIRES(bc::test::Req::Omt)) {
  const std::vector<std::string> names = omthx::discover();  // may be empty; must not throw
  CHECK(names.size() < 10000);
}
#else
TEST_CASE("omthx is linked and discovery runs without a transmitter" BC_REQUIRES(bc::test::Req::Omt)) {}
#endif