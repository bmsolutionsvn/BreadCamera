#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_OMT
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"
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

// The frame as the camera's GPU path hands it over: R8G8B8A8_UNORM in true RGBA order.
bc::TextureRef rgba8Texture(bc::Converter& conv, uint32_t w, uint32_t h, uint8_t r, uint8_t g, uint8_t b) {
  std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
  for (size_t i = 0; i < bgra.size(); i += 4) {
    bgra[i + 0] = b;
    bgra[i + 1] = g;
    bgra[i + 2] = r;
    bgra[i + 3] = 255;
  }
  return conv.toBgra8(*conv.toLinear(*conv.uploadBgra8(bgra.data(), w, h, w * 4), bc::PixelLayout::BGRA8, true), w, h,
                      /*swapRb=*/false);
}

std::string waitForAddress(const bc::OmthOutput& out) {
  std::string address;
  for (int i = 0; i < 50 && address.empty(); ++i) {
    address = out.address();
    if (address.empty()) std::this_thread::sleep_for(100ms);
  }
  return address;
}

// The centre pixel of a received BGRA picture.
std::array<int, 3> centreBgr(const omthx::VideoFrame& vf) {
  const size_t c = (static_cast<size_t>(vf.format.height / 2) * vf.format.width + vf.format.width / 2) * 4;
  return {vf.bgra[c], vf.bgra[c + 1], vf.bgra[c + 2]};
}
}  // namespace

TEST_CASE("OmthOutput: the HX output without alpha uses the GPU path") {
  const std::string name = "BreadCamera OMT path " + std::to_string(GetCurrentProcessId());
  bc::OmthOutput hx(name, omthx::Mode::Hx, omthx::Codec::Hevc, /*keyed=*/false);
  CHECK_FALSE(hx.needsCpu());
  CHECK(hx.wantsGpuRgba8());

  bc::OmthOutput full(name, omthx::Mode::Full, omthx::Codec::Hevc, /*keyed=*/false);
  CHECK(full.needsCpu());
  CHECK_FALSE(full.wantsGpuRgba8());

  bc::OmthOutput keyed(name, omthx::Mode::Hx, omthx::Codec::Hevc, /*keyed=*/true);
  CHECK(keyed.needsCpu());  // a keyed output needs the frame's alpha, which stays on the CPU path
  CHECK_FALSE(keyed.wantsGpuRgba8());
}

TEST_CASE("OmthOutput: an HX output sends the GPU frame" BC_REQUIRES(bc::test::Req::Omt)
              BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  const bc::TextureRef rgba8 = rgba8Texture(conv, 640, 360, 230, 40, 20);

  const std::string name = "BreadCamera OMT gpu " + std::to_string(GetCurrentProcessId());
  bc::OmthOutput out(name, omthx::Mode::Hx, omthx::Codec::Hevc, /*keyed=*/false);
  REQUIRE_FALSE(out.needsCpu());
  REQUIRE(out.wantsGpuRgba8());
  out.start();
  const bc::Rate rate{30, 1};
  std::atomic<bool> run{true};
  std::thread feeder([&] {
    for (uint64_t t = 0; run && t < 120; ++t) {
      bc::OutFrame f;
      f.tick = t;
      f.rate = rate;
      f.rgba8 = rgba8;  // no bgra: the camera did not read this frame back
      out.submit(std::move(f));
      std::this_thread::sleep_for(33ms);
    }
  });

  const std::string address = waitForAddress(out);
  REQUIRE_FALSE(address.empty());
  omthx::Receiver rx(address);
  omthx::VideoFrame vf;
  bool got = false;
  for (int i = 0; i < 60 && !got; ++i) {
    if (rx.receiveVideo(vf, 200)) got = vf.format.width == 640 && vf.format.height == 360;
  }
  const omthx::ReceiverStats stats = rx.stats();
  run = false;
  feeder.join();
  out.stop();

  REQUIRE(got);
  const auto bgr = centreBgr(vf);
  CHECK(std::abs(bgr[2] - 230) <= 8);  // R
  CHECK(std::abs(bgr[1] - 40) <= 8);   // G
  CHECK(std::abs(bgr[0] - 20) <= 8);   // B
  // "(d3d11)" says the source encoded the picture it was handed, without a copy through memory.
  CHECK(stats.encoder.find("(d3d11)") != std::string::npos);
}

TEST_CASE("OmthOutput: an odd-sized frame falls back to the CPU path" BC_REQUIRES(bc::test::Req::Omt)
              BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};

  const std::string name = "BreadCamera OMT odd " + std::to_string(GetCurrentProcessId());
  bc::OmthOutput out(name, omthx::Mode::Hx, omthx::Codec::Hevc, /*keyed=*/false);
  out.start();
  const bc::Rate rate{30, 1};

  bc::OutFrame odd;
  odd.tick = 0;
  odd.rate = rate;
  odd.rgba8 = rgba8Texture(conv, 641, 360, 230, 40, 20);  // an odd width: the encoder's blit cannot take it
  out.submit(std::move(odd));

  for (int i = 0; i < 50 && !out.needsCpu(); ++i) std::this_thread::sleep_for(100ms);
  REQUIRE(out.needsCpu());
  for (int i = 0; i < 50 && out.status().state != bc::SignalState::Live; ++i) std::this_thread::sleep_for(100ms);
  const bc::OutputStatus status = out.status();
  CHECK(bc::toString(status.state) == "live");  // a warning, not a failure: the output keeps sending
  REQUIRE(status.error.has_value());
  CHECK(status.error->code == "omt-gpu-refused");
  CHECK(status.error->message.rfind("OMT HX is sending through memory:", 0) == 0);

  // From the next frame on, the camera reads back: the frames carry bgra and reach the receiver.
  auto img = std::make_shared<bc::CpuImage>();
  img->width = 640;
  img->height = 360;
  img->bgra.resize(640 * 360 * 4);
  for (size_t i = 0; i < img->bgra.size(); i += 4) {
    img->bgra[i + 0] = 20;  // B
    img->bgra[i + 1] = 40;  // G
    img->bgra[i + 2] = 230;  // R
    img->bgra[i + 3] = 255;
  }
  std::atomic<bool> run{true};
  std::thread feeder([&] {
    for (uint64_t t = 1; run; ++t) {
      out.submit(frameWith(img, t, rate, false));
      std::this_thread::sleep_for(33ms);
    }
  });
  omthx::Receiver rx(out.address());
  omthx::VideoFrame vf;
  bool got = false;
  for (int i = 0; i < 60 && !got; ++i) {
    if (rx.receiveVideo(vf, 200)) got = vf.format.width == 640 && vf.format.height == 360;
  }
  run = false;
  feeder.join();
  out.stop();
  REQUIRE(got);
  const auto bgr = centreBgr(vf);
  CHECK(std::abs(bgr[2] - 230) <= 8);
  CHECK(std::abs(bgr[1] - 40) <= 8);
  CHECK(std::abs(bgr[0] - 20) <= 8);
}

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

TEST_CASE("OmthOutput: an omt hx output sends H.264 when asked" BC_REQUIRES(bc::test::Req::Omt)) {
  const std::string name = "BreadCamera OMT h264 " + std::to_string(GetCurrentProcessId());
  bc::OmthOutput out(name, omthx::Mode::Hx, omthx::Codec::H264);
  auto img = std::make_shared<bc::CpuImage>();
  // 640x360 like the HEVC case: the hardware H.264 encoder (h264_nvenc) also refuses tiny pictures.
  img->width = 640;
  img->height = 360;
  img->bgra.assign(640 * 360 * 4, 200);
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
    if (rx.receiveVideo(vf, 200)) got = vf.format.width == 640 && vf.format.height == 360;
  }
  const auto stats = rx.stats();
  run = false;
  feeder.join();
  out.stop();
  REQUIRE(got);
  CHECK(stats.codec == omthx::Codec::H264);  // the receiver sees the H.264 source, not HEVC
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

TEST_CASE("OmthOutput: status carries the tally a receiver sets" BC_REQUIRES(bc::test::Req::Omt)) {
  bc::OmthOutput out("BreadCamera OMT tally " + std::to_string(GetCurrentProcessId()), omthx::Mode::Hx);
  auto img = std::make_shared<bc::CpuImage>();
  img->width = 640;
  img->height = 360;
  img->bgra.assign(640 * 360 * 4, 200);
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

  // Off until a receiver says otherwise.
  const auto off = out.status();
  CHECK_FALSE(off.program);
  CHECK_FALSE(off.preview);

  omthx::Receiver rx(address);
  omthx::VideoFrame vf;
  for (int i = 0; i < 60; ++i) {
    if (rx.receiveVideo(vf, 200)) break;  // make sure the receiver is connected
  }
  rx.setTally(omthx::Tally{.preview = false, .program = true});
  bool program = false;
  for (int i = 0; i < 60 && !program; ++i) {
    program = out.status().program;
    if (!program) std::this_thread::sleep_for(100ms);
  }

  run = false;
  feeder.join();
  out.stop();
  CHECK(program);
}
#else
TEST_CASE("OmthOutput: an omthx receiver gets our picture with alpha" BC_REQUIRES(bc::test::Req::Omt)) {}
TEST_CASE("OmthOutput: full mode (plain OMT) also delivers a picture" BC_REQUIRES(bc::test::Req::Omt)) {}
TEST_CASE("OmthOutput: status carries the tally a receiver sets" BC_REQUIRES(bc::test::Req::Omt)) {}
TEST_CASE("OmthOutput: the HX output without alpha uses the GPU path") {}
TEST_CASE("OmthOutput: an HX output sends the GPU frame") {}
TEST_CASE("OmthOutput: an odd-sized frame falls back to the CPU path") {}
#endif
