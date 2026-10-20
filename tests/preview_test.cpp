#include <doctest/doctest.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "core/state_store.h"
#include "engine/engine.h"
#include "engine/preview_encoder.h"
#include "engine/preview_hub.h"
#include "engine/preview_service.h"
#include "support/requires.h"

using bc::PreviewSize;

TEST_CASE("avccToAnnexB: converts SPS/PPS and rejects truncated input") {
  const std::vector<uint8_t> avcc{1, 0x42, 0xC0, 0x1E, 0xFF, 0xE1, 0x00, 0x04, 0x67, 0x42, 0xC0, 0x1E,
                                  0x01, 0x00, 0x02, 0x68, 0xCE};
  const std::vector<uint8_t> want{0, 0, 0, 1, 0x67, 0x42, 0xC0, 0x1E, 0, 0, 0, 1, 0x68, 0xCE};
  CHECK(bc::avccToAnnexB(avcc) == want);
  CHECK_THROWS_AS(bc::avccToAnnexB(std::vector<uint8_t>(avcc.begin(), avcc.begin() + 9)), std::invalid_argument);
  CHECK(bc::hasNalType(want, 7));
  CHECK(bc::hasNalType(want, 8));
  CHECK_FALSE(bc::hasNalType(want, 5));
}

TEST_CASE("encodePreviewMessage: header layout") {
  bc::PreviewPacket p{{9, 9}, true, 1234};
  auto m = bc::encodePreviewMessage("cam1", p);
  const std::vector<uint8_t> want{1, 1, 4, 0, 'c', 'a', 'm', '1', 0xD2, 0x04, 0, 0, 0, 0, 0, 0, 9, 9};
  CHECK(m == want);
}

TEST_CASE("PreviewHub: union of subscriptions, new pairs reported, clients dropped") {
  bc::PreviewHub hub;
  auto fresh = hub.subscribe("c1", {"a", "b"}, PreviewSize::Small);
  CHECK(fresh.size() == 2);
  CHECK(hub.subscribe("c1", {"a"}, PreviewSize::Small).empty());  // narrowed, nothing new
  CHECK(hub.subscribe("c2", {"a"}, PreviewSize::Large).size() == 1);
  CHECK(hub.wanted() == std::set<std::pair<std::string, PreviewSize>>{{"a", PreviewSize::Small}, {"a", PreviewSize::Large}});
  CHECK(hub.clientsFor("a", PreviewSize::Small) == std::vector<std::string>{"c1"});
  hub.dropClient("c1");
  CHECK(hub.clientsFor("a", PreviewSize::Small).empty());
  CHECK(bc::parsePreviewSize("large") == PreviewSize::Large);
  CHECK_FALSE(bc::parsePreviewSize("huge"));
}

TEST_CASE("PreviewEncoder: decodable H.264 with in-band headers" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  bc::PreviewEncoder enc(512, 288, 30);
  MESSAGE("encoder: " << enc.encoderName());
  bc::CpuImage img{512, 288, std::vector<uint8_t>(512 * 288 * 4, 128)};
  std::vector<bc::PreviewPacket> packets;
  for (int i = 0; i < 60; ++i) {
    for (size_t p = 2; p < img.bgra.size(); p += 4) img.bgra[p] = static_cast<uint8_t>(i * 4);
    if (i == 30) enc.forceKeyframe();
    auto out = enc.encode(img, i * 33'333);
    packets.insert(packets.end(), out.begin(), out.end());
  }
  REQUIRE_FALSE(packets.empty());
  CHECK(packets.front().keyframe);
  CHECK(bc::hasNalType(packets.front().annexB, 7));
  int keys = 0;
  for (auto& p : packets) keys += p.keyframe ? 1 : 0;
  CHECK(keys >= 2);

  const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
  AVCodecContext* ctx = avcodec_alloc_context3(dec);
  REQUIRE(avcodec_open2(ctx, dec, nullptr) == 0);
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  int decoded = 0;
  for (auto& p : packets) {
    pkt->data = p.annexB.data();
    pkt->size = static_cast<int>(p.annexB.size());
    if (avcodec_send_packet(ctx, pkt) >= 0) {
      while (avcodec_receive_frame(ctx, frame) >= 0) ++decoded;
    }
  }
  avcodec_send_packet(ctx, nullptr);
  while (avcodec_receive_frame(ctx, frame) >= 0) ++decoded;
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
  CHECK(decoded >= 50);
}

// D3/D4/D7: the preview encodes the CpuImage the frame tap hands it, at that image's size — no
// converter, no readback, no per-wire-size downscale. PreviewService owns no Converter, so the
// only possible source of pixels is the tap.
TEST_CASE("preview encodes the shared image at the frame size" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  bc::StateStore state;
  bc::PreviewHub hub;
  std::mutex mu;
  std::map<std::string, std::vector<std::vector<uint8_t>>> sent;
  bc::PreviewService svc(state, hub, [&](const std::string& c, std::vector<uint8_t> m) {
    std::lock_guard l(mu);
    sent[c].push_back(std::move(m));
  });
  // The wire asks for Small (512x288) but the shared frame is 320x180: a stale dims(size)
  // encoder would reject the image, so this pins encoding at the frame size.
  hub.subscribe("c1", {"a"}, PreviewSize::Small);
  auto img = std::make_shared<const bc::CpuImage>(bc::CpuImage{320, 180, std::vector<uint8_t>(320 * 180 * 4, 128)});
  // The encode happens off the tick (D4): drain each tick so the worker consumes every frame.
  for (uint64_t t = 0; t < 40; ++t) {
    svc.onTick(t, {bc::FrameTap{"a", 0, nullptr, img}});
    svc.drain();
  }
  std::vector<uint8_t> msg;
  {
    std::lock_guard l(mu);
    REQUIRE_FALSE(sent["c1"].empty());
    CHECK((sent["c1"].front()[1] & 1) == 1);  // a keyframe first
    msg = sent["c1"].front();
  }

  // The first packet decodes at the frame size, not the wire size.
  const size_t idLen = msg[2] | (msg[3] << 8);
  std::vector<uint8_t> annexB(msg.begin() + 4 + idLen + 8, msg.end());
  const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
  AVCodecContext* ctx = avcodec_alloc_context3(dec);
  REQUIRE(avcodec_open2(ctx, dec, nullptr) == 0);
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  pkt->data = annexB.data();
  pkt->size = static_cast<int>(annexB.size());
  REQUIRE(avcodec_send_packet(ctx, pkt) >= 0);
  REQUIRE(avcodec_receive_frame(ctx, frame) >= 0);
  CHECK(frame->width == 320);
  CHECK(frame->height == 180);
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
}

// D8: the shared image is one readback for every tile, but its size is now the camera's input size
// (Task 1), so a 4K camera would H.264-encode 4K per visible tile. The preview caps its encode size
// at 1280x720, downscaling the shared image on the encoder worker (off the tick) and opening the
// encoder at the capped size.
TEST_CASE("the preview caps its encode size at 1280x720" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  bc::StateStore state;
  bc::PreviewHub hub;
  std::mutex mu;
  std::map<std::string, std::vector<std::vector<uint8_t>>> sent;
  bc::PreviewService svc(state, hub, [&](const std::string& c, std::vector<uint8_t> m) {
    std::lock_guard l(mu);
    sent[c].push_back(std::move(m));
  });
  hub.subscribe("c1", {"a"}, PreviewSize::Small);
  auto img = std::make_shared<const bc::CpuImage>(
      bc::CpuImage{3840, 2160, std::vector<uint8_t>(3840 * 2160 * 4, 128)});
  for (uint64_t t = 0; t < 40; ++t) {
    svc.onTick(t, {bc::FrameTap{"a", 0, nullptr, img}});
    svc.drain();
  }
  std::vector<uint8_t> msg;
  {
    std::lock_guard l(mu);
    REQUIRE_FALSE(sent["c1"].empty());
    msg = sent["c1"].front();
  }

  // The first packet decodes at the cap, not the 4K frame size.
  const size_t idLen = msg[2] | (msg[3] << 8);
  std::vector<uint8_t> annexB(msg.begin() + 4 + idLen + 8, msg.end());
  const AVCodec* dec = avcodec_find_decoder(AV_CODEC_ID_H264);
  AVCodecContext* ctx = avcodec_alloc_context3(dec);
  REQUIRE(avcodec_open2(ctx, dec, nullptr) == 0);
  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  pkt->data = annexB.data();
  pkt->size = static_cast<int>(annexB.size());
  REQUIRE(avcodec_send_packet(ctx, pkt) >= 0);
  REQUIRE(avcodec_receive_frame(ctx, frame) >= 0);
  CHECK(frame->width == 1280);
  CHECK(frame->height == 720);
  av_frame_free(&frame);
  av_packet_free(&pkt);
  avcodec_free_context(&ctx);
}

TEST_CASE("PreviewService: subscribers get a keyframe first; others get nothing" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  bc::StateStore state;
  bc::PreviewHub hub;
  std::mutex mu;
  std::map<std::string, std::vector<std::vector<uint8_t>>> sent;
  bc::PreviewService svc(state, hub, [&](const std::string& c, std::vector<uint8_t> m) {
    std::lock_guard l(mu);
    sent[c].push_back(std::move(m));
  });
  hub.subscribe("c1", {"a"}, PreviewSize::Small);
  auto img = std::make_shared<const bc::CpuImage>(bc::CpuImage{320, 180, std::vector<uint8_t>(320 * 180 * 4, 128)});
  for (uint64_t t = 0; t < 40; ++t) {
    svc.onTick(t, {bc::FrameTap{"a", 0, nullptr, img}, bc::FrameTap{"b", 0, nullptr, img}});
    svc.drain();
  }
  std::lock_guard l(mu);
  REQUIRE(sent["c1"].size() >= 10);
  CHECK((sent["c1"].front()[1] & 1) == 1);
  CHECK(sent.count("c2") == 0);
}

// D9: past the encoder cap the excess tiles are skipped and a warning is set — unlike the old
// broken_ flag, which blanked every preview on the first open failure.
TEST_CASE("preview cap warns instead of disabling every preview" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  bc::StateStore state;
  bc::PreviewHub hub;
  std::mutex mu;
  std::map<std::string, std::vector<std::vector<uint8_t>>> sent;
  bc::PreviewService svc(state, hub, [&](const std::string& c, std::vector<uint8_t> m) {
    std::lock_guard l(mu);
    sent[c].push_back(std::move(m));
  });
  const size_t cap = bc::PreviewService::kMaxPreviewEncoders;
  const size_t n = cap + 2;
  auto img = std::make_shared<const bc::CpuImage>(bc::CpuImage{320, 180, std::vector<uint8_t>(320 * 180 * 4, 128)});
  std::vector<bc::FrameTap> frames;
  for (size_t i = 0; i < n; ++i) {
    hub.subscribe("c" + std::to_string(i), {"cam" + std::to_string(i)}, PreviewSize::Small);
    frames.push_back(bc::FrameTap{"cam" + std::to_string(i), 0, nullptr, img});
  }
  for (uint64_t t = 0; t < 20; ++t) {
    svc.onTick(t, frames);
    svc.drain();
  }
  std::lock_guard l(mu);
  // The cameras within the cap still encode...
  for (size_t i = 0; i < cap; ++i) CHECK_FALSE(sent["c" + std::to_string(i)].empty());
  // ...the excess tiles are skipped...
  for (size_t i = cap; i < n; ++i) CHECK(sent.count("c" + std::to_string(i)) == 0);
  // ...and the warning is set rather than every preview being blanked.
  CHECK(state.snapshot().contains(nlohmann::json::json_pointer("/engine/warnings/preview")));
}

// D9: one camera whose encoder cannot open must be dropped on its own — the other cameras keep
// encoding and a warning is set. (The old broken_ flag blanked every preview on the first open
// failure; the cap test above only covers the skip-and-warn path, not this isolation.)
TEST_CASE("preview: one camera failing to open keeps the others encoding" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  bc::StateStore state;
  bc::PreviewHub hub;
  std::mutex mu;
  std::map<std::string, std::vector<std::vector<uint8_t>>> sent;
  bc::PreviewService svc(state, hub, [&](const std::string& c, std::vector<uint8_t> m) {
    std::lock_guard l(mu);
    sent[c].push_back(std::move(m));
  });
  // A zero-sized image opens no encoder: PreviewEncoder throws, which must drop only this tile.
  auto bad = std::make_shared<const bc::CpuImage>(bc::CpuImage{0, 0, {}});
  auto good = std::make_shared<const bc::CpuImage>(bc::CpuImage{320, 180, std::vector<uint8_t>(320 * 180 * 4, 128)});

  // First only the camera whose encoder cannot open. It fails and is dropped; a warning is set.
  hub.subscribe("c1", {"bad"}, PreviewSize::Small);
  for (uint64_t t = 0; t < 8; t += 2) {
    svc.onTick(t, {bc::FrameTap{"bad", 0, nullptr, bad}});
    svc.drain();
  }
  // Now a healthy camera starts streaming: the earlier failure must not blank it. (Under the old
  // global broken_ flag no later camera would ever encode, so the check below would fail.)
  hub.subscribe("c0", {"good"}, PreviewSize::Small);
  for (uint64_t t = 8; t < 48; t += 2) {
    svc.onTick(t, {bc::FrameTap{"bad", 0, nullptr, bad}, bc::FrameTap{"good", 0, nullptr, good}});
    svc.drain();
  }
  std::lock_guard l(mu);
  CHECK_FALSE(sent["c0"].empty());   // the good camera keeps encoding
  CHECK(sent.count("c1") == 0);      // the failed tile sends nothing
  CHECK(state.snapshot().contains(nlohmann::json::json_pointer("/engine/warnings/preview")));
}
