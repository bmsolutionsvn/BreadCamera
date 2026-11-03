#include <doctest/doctest.h>

#include "support/requires.h"

#if BC_WITH_NDI
#include <Processing.NDI.Lib.h>

#include <memory>
#include <string>
#include <thread>

#include "io/ndi_output.h"
#include "io/uyvy.h"

using namespace std::chrono_literals;

TEST_CASE("NdiOutput: sends video and audio a receiver can read" BC_REQUIRES(bc::test::Req::Ndi)) {
  REQUIRE(NDIlib_initialize());
  const std::string name = "BreadCamera Test " + std::to_string(GetCurrentProcessId());
  bc::NdiOutput out(name);
  out.start();

  auto img = std::make_shared<bc::CpuImage>();
  img->width = 64;
  img->height = 36;
  img->bgra.resize(64 * 36 * 4);
  for (size_t i = 0; i < img->bgra.size(); i += 4) {
    img->bgra[i] = 0;        // B
    img->bgra[i + 1] = 0;    // G
    img->bgra[i + 2] = 200;  // R
    img->bgra[i + 3] = 255;
  }
  auto audio = std::make_shared<std::vector<float>>(1600 * 2, 0.25f);  // 1600 stereo frames

  std::atomic<bool> sending{true};
  std::thread sender([&] {
    uint64_t tick = 0;
    while (sending) {
      bc::OutFrame f;
      f.tick = tick++;
      f.rate = {30, 1};
      f.bgra = img;
      f.audio = audio;
      out.submit(std::move(f));
      std::this_thread::sleep_for(33ms);
    }
  });

  NDIlib_find_create_t fc{};
  fc.show_local_sources = true;
  NDIlib_find_instance_t find = NDIlib_find_create_v2(&fc);
  REQUIRE(find);
  const NDIlib_source_t* match = nullptr;
  for (int attempt = 0; attempt < 16 && !match; ++attempt) {
    NDIlib_find_wait_for_sources(find, 500);
    uint32_t n = 0;
    const NDIlib_source_t* srcs = NDIlib_find_get_current_sources(find, &n);
    for (uint32_t i = 0; i < n; ++i) {
      if (std::string(srcs[i].p_ndi_name).find(name) != std::string::npos) match = &srcs[i];
    }
  }
  REQUIRE(match);

  NDIlib_recv_create_v3_t rc{};
  rc.source_to_connect_to = *match;
  rc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
  rc.bandwidth = NDIlib_recv_bandwidth_highest;
  NDIlib_recv_instance_t recv = NDIlib_recv_create_v3(&rc);
  REQUIRE(recv);
  bool gotVideo = false, gotAudio = false;
  int red = -1, channels = 0;
  for (int i = 0; i < 200 && !(gotVideo && gotAudio); ++i) {
    NDIlib_video_frame_v2_t v{};
    NDIlib_audio_frame_v3_t a{};
    switch (NDIlib_recv_capture_v3(recv, &v, &a, nullptr, 100)) {
      case NDIlib_frame_type_video:
        gotVideo = true;
        red = v.p_data[2];
        NDIlib_recv_free_video_v2(recv, &v);
        break;
      case NDIlib_frame_type_audio:
        gotAudio = true;
        channels = a.no_channels;
        NDIlib_recv_free_audio_v3(recv, &a);
        break;
      default:
        break;
    }
  }
  NDIlib_recv_destroy(recv);
  NDIlib_find_destroy(find);
  sending = false;
  sender.join();
  out.stop();

  CHECK(gotVideo);
  CHECK(std::abs(red - 200) <= 8);
  CHECK(gotAudio);
  CHECK(channels == 2);
  CHECK(out.status().error == std::nullopt);
}

TEST_CASE("NdiOutput: a UYVY frame reaches a receiver as UYVY" BC_REQUIRES(bc::test::Req::Ndi)) {
  REQUIRE(NDIlib_initialize());
  const std::string name = "BreadCamera UYVY Test " + std::to_string(GetCurrentProcessId());
  bc::NdiOutput out(name);
  out.start();

  std::vector<uint8_t> bgra(64 * 36 * 4);
  for (size_t i = 0; i < bgra.size(); i += 4) {
    bgra[i] = 0;        // B
    bgra[i + 1] = 0;    // G
    bgra[i + 2] = 200;  // R
    bgra[i + 3] = 255;
  }
  auto img = std::make_shared<bc::CpuImage>();
  img->width = 64;
  img->height = 36;
  img->layout = bc::CpuLayout::Uyvy;
  img->bgra.resize(64 * 36 * 2);
  bc::bgraToUyvy(bgra.data(), 64, 36, img->bgra.data(), 64 * 2, true);

  std::atomic<bool> sending{true};
  std::thread sender([&] {
    uint64_t tick = 0;
    while (sending) {
      bc::OutFrame f;
      f.tick = tick++;
      f.rate = {30, 1};
      f.uyvy = img;
      out.submit(std::move(f));
      std::this_thread::sleep_for(33ms);
    }
  });

  NDIlib_find_create_t fc{};
  fc.show_local_sources = true;
  NDIlib_find_instance_t find = NDIlib_find_create_v2(&fc);
  REQUIRE(find);
  const NDIlib_source_t* match = nullptr;
  for (int attempt = 0; attempt < 16 && !match; ++attempt) {
    NDIlib_find_wait_for_sources(find, 500);
    uint32_t n = 0;
    const NDIlib_source_t* srcs = NDIlib_find_get_current_sources(find, &n);
    for (uint32_t i = 0; i < n; ++i) {
      if (std::string(srcs[i].p_ndi_name).find(name) != std::string::npos) match = &srcs[i];
    }
  }
  REQUIRE(match);

  NDIlib_recv_create_v3_t rc{};
  rc.source_to_connect_to = *match;
  rc.color_format = NDIlib_recv_color_format_UYVY_BGRA;  // no alpha: UYVY in, no conversion
  rc.allow_video_fields = false;
  rc.bandwidth = NDIlib_recv_bandwidth_highest;
  NDIlib_recv_instance_t recv = NDIlib_recv_create_v3(&rc);
  REQUIRE(recv);
  bool gotVideo = false;
  int fourCc = 0, luma = -1, width = -1, height = -1;
  for (int i = 0; i < 200 && !gotVideo; ++i) {
    NDIlib_video_frame_v2_t v{};
    switch (NDIlib_recv_capture_v3(recv, &v, nullptr, nullptr, 100)) {
      case NDIlib_frame_type_video:
        gotVideo = true;
        fourCc = v.FourCC;
        width = v.xres;
        height = v.yres;
        if (v.FourCC == NDIlib_FourCC_type_UYVY && v.xres == 64 && v.yres == 36) {
          const size_t centre = static_cast<size_t>(v.yres / 2) * static_cast<size_t>(v.line_stride_in_bytes) +
                                static_cast<size_t>(v.xres / 2) * 2;
          luma = v.p_data[centre + 1];  // Y of the centre pixel (its U is one byte back)
        }
        NDIlib_recv_free_video_v2(recv, &v);
        break;
      default:
        break;
    }
  }
  NDIlib_recv_destroy(recv);
  NDIlib_find_destroy(find);
  sending = false;
  sender.join();
  out.stop();

  CHECK(gotVideo);
  CHECK(width == 64);
  CHECK(height == 36);
  CHECK(fourCc == NDIlib_FourCC_type_UYVY);
  const int want = img->bgra[(static_cast<size_t>(36 / 2) * 64 + 32) * 2 + 1];
  CHECK(std::abs(luma - want) <= 3);
}
#else
TEST_CASE("NdiOutput: sends video and audio a receiver can read" BC_REQUIRES(bc::test::Req::Ndi)) {}
#endif
