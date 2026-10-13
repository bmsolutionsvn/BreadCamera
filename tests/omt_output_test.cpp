#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_OMT
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>

#include "io/omt_output.h"
#include "omthx/omthx.h"

using namespace std::chrono_literals;

namespace {
bc::OutFrame frameWith(std::shared_ptr<const bc::CpuImage> img, uint64_t tick, const bc::Rate& rate, bool alpha) {
  bc::OutFrame f;
  f.tick = tick;
  f.rate = rate;
  f.bgra = std::move(img);
  f.hasAlpha = alpha;
  return f;
}
}  // namespace

TEST_CASE("OmthOutput: an omthx receiver gets our picture with alpha" BC_REQUIRES(bc::test::Req::Omt)) {
  const std::string name = "BreadCamera OMT " + std::to_string(GetCurrentProcessId());
  bc::OmthOutput out(name, omthx::Mode::Hx);
  auto img = std::make_shared<bc::CpuImage>();
  // 640x360, not the plan's 64x36: the AMD AMF HEVC encoder this PC selects cannot encode a coded
  // picture below about 128x128 (it refuses to open) and deadlocks at the 128x128 minimum, so a
  // normal 360p frame is used. An alpha frame is coded at twice its height (640x736) here.
  img->width = 640;
  img->height = 360;
  img->bgra.resize(640 * 360 * 4);
  for (size_t i = 0; i < img->bgra.size(); i += 4) {
    img->bgra[i] = 0;      // B
    img->bgra[i + 1] = 0;  // G
    img->bgra[i + 2] = 200;  // R
    img->bgra[i + 3] = 128;  // A
  }
  out.start();
  const bc::Rate rate{30, 1};
  std::atomic<bool> run{true};
  std::thread feeder([&] {
    for (uint64_t t = 0; run; ++t) {
      out.submit(frameWith(img, t, rate, true));
      std::this_thread::sleep_for(33ms);
    }
  });

  // The address is published when the worker thread opens the sender; poll for it.
  std::string address;
  for (int i = 0; i < 50 && address.empty(); ++i) {
    address = out.address();
    if (address.empty()) std::this_thread::sleep_for(100ms);
  }
  REQUIRE_FALSE(address.empty());
  omthx::Receiver rx(address);
  omthx::VideoFrame vf;
  bool got = false;
  for (int i = 0; i < 60 && !got; ++i) {
    if (rx.receiveVideo(vf, 200)) got = vf.bgra.size() == 640u * 360 * 4;
  }
  run = false;
  feeder.join();
  out.stop();

  REQUIRE(got);
  CHECK(vf.format.width == 640);
  CHECK(vf.format.height == 360);
  CHECK(vf.format.alpha);
  CHECK(vf.premultiplied);  // the wire carries premultiplied alpha; the receiver says so
  // Straight (200,128) is premultiplied on the wire to about 200*128/255 = 100.
  const int r = vf.bgra[2];
  const int a = vf.bgra[3];
  CHECK(std::abs(r - 100) <= 25);
  CHECK(std::abs(a - 128) <= 20);
}

TEST_CASE("OmthOutput: full mode (plain OMT) also delivers a picture" BC_REQUIRES(bc::test::Req::Omt)) {
  const std::string name = "BreadCamera OMT full " + std::to_string(GetCurrentProcessId());
  bc::OmthOutput out(name, omthx::Mode::Full);
  auto img = std::make_shared<bc::CpuImage>();
  img->width = 64;
  img->height = 36;
  img->bgra.assign(64 * 36 * 4, 200);
  out.start();
  const bc::Rate rate{30, 1};
  std::atomic<bool> run{true};
  std::thread feeder([&] {
    for (uint64_t t = 0; run; ++t) {
      out.submit(frameWith(img, t, rate, false));
      std::this_thread::sleep_for(33ms);
    }
  });
  std::string address;
  for (int i = 0; i < 50 && address.empty(); ++i) {
    address = out.address();
    if (address.empty()) std::this_thread::sleep_for(100ms);
  }
  REQUIRE_FALSE(address.empty());
  omthx::Receiver rx(address);
  omthx::VideoFrame vf;
  bool got = false;
  for (int i = 0; i < 60 && !got; ++i) {
    if (rx.receiveVideo(vf, 200)) got = vf.format.width == 64 && vf.format.height == 36;
  }
  run = false;
  feeder.join();
  out.stop();
  REQUIRE(got);
}
#else
TEST_CASE("OmthOutput: an omthx receiver gets our picture with alpha" BC_REQUIRES(bc::test::Req::Omt)) {}
TEST_CASE("OmthOutput: full mode (plain OMT) also delivers a picture" BC_REQUIRES(bc::test::Req::Omt)) {}
#endif
