// DOCTEST_CONFIG_DOUBLE_STRINGIFY: without it doctest's ADL toString picks
// bc::toString(SignalState) (string_view) and string_view + const char* fails to compile.
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_DECKLINK
#include <windows.h>

#include <objbase.h>
#include <oleauto.h>
#include <thread>

#include "DeckLinkAPI_h.h"
#include "gpu/device.h"
#include "io/decklink.h"
#include "io/decklink_input.h"
#include "io/decklink_output.h"

using namespace std::chrono_literals;

TEST_CASE("DeckLink: display modes map to rates") {
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p2398) == bc::Rate{24000, 1001});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p24) == bc::Rate{24, 1});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p25) == bc::Rate{25, 1});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p2997) == bc::Rate{30000, 1001});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p30) == bc::Rate{30, 1});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p50) == bc::Rate{50, 1});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p5994) == bc::Rate{60000, 1001});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p6000) == bc::Rate{60, 1});
  CHECK(bc::deckLinkRateFor(bmdModeHD1080p120) == bc::Rate{120, 1});
  CHECK_FALSE(bc::deckLinkRateFor(bmdModeHD720p50));
}

TEST_CASE("DeckLink: a missing device index is reported, not fatal") {
  bc::DeckLinkOutput out(99, {60, 1}, false);
  out.start();
  std::this_thread::sleep_for(500ms);
  auto st = out.status();
  CHECK(st.state == bc::SignalState::Offline);
  REQUIRE(st.error);
  CHECK(st.error->code == "decklink-failed");
  out.stop();
}

TEST_CASE("DeckLink: the first card's output plays 2 s of frames" BC_REQUIRES(bc::test::Req::DeckLink)) {
  auto info = bc::listDeckLinkDevices().front();
  MESSAGE("card: " << info.name << " in=" << info.hasInput << " out=" << info.hasOutput << " extKey=" << info.externalKeying);
  if (!info.hasOutput) return;
  bc::DeckLinkOutput out(info.index, {60, 1}, false);
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
  CHECK(out.status().state == bc::SignalState::Live);
  CHECK_FALSE(out.status().error);
  out.stop();
}
#else
TEST_CASE("DeckLink: the first card's output plays 2 s of frames" BC_REQUIRES(bc::test::Req::DeckLink)) {}
#endif