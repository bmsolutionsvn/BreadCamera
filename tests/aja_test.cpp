// DOCTEST_CONFIG_DOUBLE_STRINGIFY: without it doctest's ADL toString picks
// bc::toString(SignalState) (string_view) and string_view + const char* fails to compile.
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_AJA
#include <thread>

#include "io/aja.h"
#include "io/aja_output.h"

using namespace std::chrono_literals;

TEST_CASE("AJA: listing runs; a missing device is reported, not fatal") {
  MESSAGE(bc::listAjaDevices().size() << " AJA device(s)");
  bc::AjaOutput out(7, 1, {60, 1});
  out.start();
  std::this_thread::sleep_for(500ms);
  auto st = out.status();
  CHECK(st.state == bc::SignalState::Offline);
  REQUIRE(st.error);
  CHECK(st.error->code == "aja-failed");
  out.stop();
}

TEST_CASE("AJA: the first card's output 1 plays 2 s of frames" BC_REQUIRES(bc::test::Req::Aja)) {
  bc::AjaOutput out(0, 1, {60, 1});
  out.start();
  auto img = std::make_shared<bc::CpuImage>();
  img->width = 1920;
  img->height = 1080;
  img->bgra.assign(1920 * 1080 * 4, 100);
  for (uint64_t t = 0; t < 120; ++t) {
    bc::OutFrame f;
    f.tick = t;
    f.rate = {60, 1};
    f.bgra = img;
    out.submit(std::move(f));
    std::this_thread::sleep_for(16ms);
  }
  CHECK_FALSE(out.status().error);
  out.stop();
}
#else
TEST_CASE("AJA: the first card's output 1 plays 2 s of frames" BC_REQUIRES(bc::test::Req::Aja)) {}
#endif