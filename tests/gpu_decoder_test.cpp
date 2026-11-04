#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

#include <windows.h>

#include <psapi.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/d3d11va.h"
#include "io/h264_encoder.h"
#include "support/h264_black720.h"
#include "support/requires.h"

using namespace bc;

namespace {
// This fixture is a run of access units, each starting at the SPS that precedes its picture.
std::vector<std::span<const uint8_t>> accessUnits(const uint8_t* data, size_t size) {
  std::vector<size_t> starts;
  for (size_t i = 0; i + 4 <= size;) {
    size_t sc = 0;
    if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) sc = 3;
    else if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) sc = 4;
    if (sc == 0) {
      ++i;
      continue;
    }
    if ((data[i + sc] & 0x1f) == 7) starts.push_back(i);  // SPS
    i += sc;
  }
  std::vector<std::span<const uint8_t>> aus;
  for (size_t k = 0; k < starts.size(); ++k) {
    aus.emplace_back(data + starts[k], (k + 1 < starts.size() ? starts[k + 1] : size) - starts[k]);
  }
  return aus;
}

std::vector<uint8_t> blackNv12(uint32_t w, uint32_t h) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 3 / 2, 16);
  std::fill(px.begin() + static_cast<size_t>(w) * h, px.end(), 128);
  return px;
}

// Encodes `count` black frames at w x h into one Annex-B access unit per packet.
// Throws (like openH264Encoder) when no H.264 encoder opens on this machine.
std::vector<std::vector<uint8_t>> encodeBlack(uint32_t w, uint32_t h, int count) {
  auto enc = openH264Encoder(static_cast<int>(w), static_cast<int>(h), 30, 1, 2'000'000, 30, false);
  std::unique_ptr<AVCodecContext, void (*)(AVCodecContext*)> ctx(enc.ctx, [](AVCodecContext* c) {
    avcodec_free_context(&c);
  });
  const auto px = blackNv12(w, h);
  AVFrame* f = av_frame_alloc();
  AVPacket* p = av_packet_alloc();
  std::vector<std::vector<uint8_t>> aus;
  auto send = [&](AVFrame* frame) {
    if (avcodec_send_frame(ctx.get(), frame) < 0) throw std::runtime_error("send_frame failed");
    while (avcodec_receive_packet(ctx.get(), p) == 0) {
      aus.emplace_back(p->data, p->data + p->size);
      av_packet_unref(p);
    }
  };
  f->format = AV_PIX_FMT_NV12;
  f->width = static_cast<int>(w);
  f->height = static_cast<int>(h);
  if (av_frame_get_buffer(f, 0) < 0) throw std::runtime_error("frame buffer failed");
  for (int i = 0; i < count; ++i) {
    av_frame_make_writable(f);
    for (uint32_t y = 0; y < h; ++y) std::memcpy(f->data[0] + y * f->linesize[0], px.data() + y * w, w);
    for (uint32_t y = 0; y < h / 2; ++y)
      std::memcpy(f->data[1] + y * f->linesize[1], px.data() + static_cast<size_t>(w) * h + y * w, w);
    f->pts = i;
    send(f);
  }
  send(nullptr);
  av_frame_free(&f);
  av_packet_free(&p);
  return aus;
}

uint64_t privateBytes() {
  PROCESS_MEMORY_COUNTERS_EX c{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof c)) return 0;
  return c.PrivateUsage;
}

struct Gpu {
  std::unique_ptr<GpuDevice> dev = GpuDevice::create(GpuDevice::Pref::HardwareOrWarp);
  TexturePool pool{*dev};
  Converter conv{*dev, pool};
};
}  // namespace

TEST_CASE("GpuDecoder decodes black 720p on the GPU" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Gpu g;
  const auto aus = accessUnits(test::kH264Black720, test::kH264Black720Size);
  REQUIRE(aus.size() == 2);
  GpuDecoder dec(*g.dev, g.pool, AV_CODEC_ID_H264);
  int pictures = 0;
  VideoFrameIn last;
  for (int i = 0; i < 5; ++i) {
    for (auto au : aus) {
      dec.decode(au, i, [&](VideoFrameIn f) {
        ++pictures;
        last = std::move(f);
      });
    }
  }
  if (!dec.hardware()) {
    MESSAGE("the GPU has no H.264 hardware decoder; skipping");
    return;
  }
  CHECK(pictures >= 5);
  REQUIRE(last.tex != nullptr);
  CHECK(last.tex->width == 1280);
  CHECK(last.tex->height == 720);
  CHECK(last.layout == PixelLayout::NV12);
  auto linear = g.conv.toLinear(*last.tex, last.layout, last.fullRange);
  auto img = g.conv.readback(*g.conv.toBgra8(*linear, 1280, 720));
  REQUIRE(img.bgra.size() == 1280u * 720 * 4);
  const size_t centre = (static_cast<size_t>(360) * img.width + 640) * 4;
  CHECK(std::abs(static_cast<int>(img.bgra[centre]) - 0) <= 4);
  CHECK(std::abs(static_cast<int>(img.bgra[centre + 1]) - 0) <= 4);
  CHECK(std::abs(static_cast<int>(img.bgra[centre + 2]) - 0) <= 4);
}

TEST_CASE("GpuDecoder's software path matches" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Gpu g;
  const auto aus = accessUnits(test::kH264Black720, test::kH264Black720Size);
  GpuDecoder dec(*g.dev, g.pool, AV_CODEC_ID_H264, /*hardware=*/false);
  int pictures = 0;
  VideoFrameIn last;
  for (int i = 0; i < 5; ++i) {
    for (auto au : aus) {
      dec.decode(au, i, [&](VideoFrameIn f) {
        ++pictures;
        last = std::move(f);
      });
    }
  }
  CHECK(pictures >= 5);
  REQUIRE(last.tex != nullptr);
  CHECK(last.layout == PixelLayout::NV12);
  CHECK_FALSE(dec.hardware());
  auto linear = g.conv.toLinear(*last.tex, last.layout, last.fullRange);
  auto img = g.conv.readback(*g.conv.toBgra8(*linear, 1280, 720));
  const size_t centre = (static_cast<size_t>(360) * img.width + 640) * 4;
  CHECK(std::abs(static_cast<int>(img.bgra[centre]) - 0) <= 4);
  CHECK(std::abs(static_cast<int>(img.bgra[centre + 1]) - 0) <= 4);
  CHECK(std::abs(static_cast<int>(img.bgra[centre + 2]) - 0) <= 4);
}

TEST_CASE("uploadSoftwareFrame honours a padded luma stride" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Gpu g;
  AVFrame* f = av_frame_alloc();
  REQUIRE(f != nullptr);
  f->format = AV_PIX_FMT_YUV420P;
  f->width = 854;
  f->height = 480;
  REQUIRE(av_frame_get_buffer(f, 32) == 0);
  REQUIRE(f->linesize[0] > 854);  // a real decoder's luma rows are padded; this is the case under test
  // Left half white, right half black, constant down every row; a red-tinted chroma.
  for (int y = 0; y < 480; ++y) {
    std::memset(f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0], 235, 427);
    std::memset(f->data[0] + static_cast<ptrdiff_t>(y) * f->linesize[0] + 427, 16, 854 - 427);
  }
  for (int y = 0; y < 240; ++y) {
    std::memset(f->data[1] + static_cast<ptrdiff_t>(y) * f->linesize[1], 64, 427);
    std::memset(f->data[2] + static_cast<ptrdiff_t>(y) * f->linesize[2], 192, 427);
  }
  SwsContext* sws = nullptr;
  std::vector<uint8_t> scratch;
  VideoFrameIn out;
  CHECK(uploadSoftwareFrame(*g.dev, g.pool, f, 854, 480, sws, scratch, out));
  av_frame_free(&f);
  REQUIRE(out.tex != nullptr);
  CHECK(out.layout == PixelLayout::NV12);

  auto linear = g.conv.toLinear(*out.tex, out.layout, out.fullRange);
  auto img = g.conv.readback(*g.conv.toBgra8(*linear, 854, 480));
  REQUIRE(img.bgra.size() == 854u * 480 * 4);
  auto px = [&](uint32_t x, uint32_t y) { return &img.bgra[(static_cast<size_t>(y) * img.width + x) * 4]; };
  // Reading the luma tightly packed would pull lower rows' left half into this row's right half.
  CHECK(px(300, 470)[1] > 150);  // G: left half is white
  CHECK(px(440, 470)[1] < 80);   // G: right half is black
  CHECK(px(440, 100)[1] < 80);
  // Cr above neutral keeps red over blue: a U/V swap would inverse this.
  CHECK(px(300, 470)[2] > px(300, 470)[0] + 40);
}

TEST_CASE("GpuDecoder keeps memory flat" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  Gpu g;
  const auto aus = accessUnits(test::kH264Black720, test::kH264Black720Size);
  GpuDecoder dec(*g.dev, g.pool, AV_CODEC_ID_H264);
  int pictures = 0;
  auto feed = [&](int n) {
    for (int i = 0; i < n; ++i) dec.decode(aus[i % aus.size()], i, [&](VideoFrameIn) { ++pictures; });
  };
  feed(10);  // warm the decoder and its surface pool
  const int64_t before = static_cast<int64_t>(privateBytes());
  feed(300);
  // Signed: the OS may trim a few pages while the frames decode, and that is still flat, not a
  // uint64 underflow.
  const int64_t grown = static_cast<int64_t>(privateBytes()) - before;
  CHECK(grown < 64ll * 1024 * 1024);
  CHECK(pictures >= 300);
}

TEST_CASE("GpuDecoder follows a resolution change mid-stream" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  std::vector<std::vector<uint8_t>> hi, lo;
  try {
    hi = encodeBlack(1280, 720, 5);
    lo = encodeBlack(640, 360, 5);
  } catch (const std::exception& e) {
    MESSAGE("no H.264 encoder for a 360p fixture: " << e.what());
    return;
  }
  REQUIRE_FALSE(hi.empty());
  REQUIRE_FALSE(lo.empty());

  Gpu g;
  GpuDecoder dec(*g.dev, g.pool, AV_CODEC_ID_H264);
  uint32_t w = 0, h = 0;
  auto emit = [&](VideoFrameIn f) {
    w = f.tex->width;
    h = f.tex->height;
  };
  for (const auto& au : hi) dec.decode(au, 0, emit);
  CHECK(w == 1280);
  CHECK(h == 720);
  for (const auto& au : lo) dec.decode(au, 0, emit);
  CHECK(w == 640);
  CHECK(h == 360);
}
