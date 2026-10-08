#include "engine/preview_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <cstring>
#include <stdexcept>

namespace bc {
namespace {
std::string averr(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(rc, buf, sizeof(buf));
  return buf;
}
bool isAnnexB(std::span<const uint8_t> d) {
  return d.size() >= 4 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || (d[2] == 0 && d[3] == 1));
}
}  // namespace

std::vector<uint8_t> avccToAnnexB(std::span<const uint8_t> a) {
  auto need = [&](size_t pos, size_t n) {
    if (pos + n > a.size()) throw std::invalid_argument("truncated avcC");
  };
  need(0, 6);
  if (a[0] != 1) throw std::invalid_argument("not avcC");
  std::vector<uint8_t> out;
  size_t pos = 5;
  auto copySets = [&](int count) {
    for (int i = 0; i < count; ++i) {
      need(pos, 2);
      const size_t len = (static_cast<size_t>(a[pos]) << 8) | a[pos + 1];
      pos += 2;
      need(pos, len);
      out.insert(out.end(), {0, 0, 0, 1});
      out.insert(out.end(), a.begin() + pos, a.begin() + pos + len);
      pos += len;
    }
  };
  const int sps = a[pos++] & 0x1F;
  copySets(sps);
  need(pos, 1);
  const int pps = a[pos++];
  copySets(pps);
  return out;
}

bool hasNalType(std::span<const uint8_t> d, int type) {
  for (size_t i = 0; i + 3 < d.size(); ++i) {
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1 && (d[i + 3] & 0x1F) == type) return true;
  }
  return false;
}

std::vector<uint8_t> encodePreviewMessage(std::string_view id, const PreviewPacket& p) {
  std::vector<uint8_t> m;
  m.reserve(4 + id.size() + 8 + p.annexB.size());
  m.push_back(1);
  m.push_back(p.keyframe ? 1 : 0);
  m.push_back(static_cast<uint8_t>(id.size() & 0xFF));
  m.push_back(static_cast<uint8_t>((id.size() >> 8) & 0xFF));
  m.insert(m.end(), id.begin(), id.end());
  for (int i = 0; i < 8; ++i) m.push_back(static_cast<uint8_t>((static_cast<uint64_t>(p.ptsUs) >> (8 * i)) & 0xFF));
  m.insert(m.end(), p.annexB.begin(), p.annexB.end());
  return m;
}

const std::vector<std::string>& PreviewEncoder::encoderOrder() {
  static const std::vector<std::string> order{"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf"};
  return order;
}

PreviewEncoder::PreviewEncoder(int w, int h, int fps) : w_(w), h_(h) {
  std::string tried;
  for (const auto& name : encoderOrder()) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) {
      tried += name + " (not built); ";
      continue;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = w;
    c->height = h;
    c->time_base = {1, fps};
    c->framerate = {fps, 1};
    c->pix_fmt = AV_PIX_FMT_NV12;
    c->gop_size = fps;
    c->max_b_frames = 0;
    c->bit_rate = w >= 1280 ? 4'000'000 : 1'000'000;
    c->flags |= AV_CODEC_FLAG_LOW_DELAY | AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* o = nullptr;
    if (name == "h264_nvenc") {
      av_dict_set(&o, "preset", "p1", 0);
      av_dict_set(&o, "tune", "ull", 0);
      av_dict_set(&o, "zerolatency", "1", 0);
      av_dict_set(&o, "forced-idr", "1", 0);
    } else if (name == "h264_amf") {
      av_dict_set(&o, "usage", "ultralowlatency", 0);
      av_dict_set(&o, "forced_idr", "1", 0);
    } else if (name == "h264_qsv") {
      av_dict_set(&o, "preset", "veryfast", 0);
      av_dict_set(&o, "async_depth", "1", 0);
    } else if (name == "h264_mf") {
      av_dict_set(&o, "rate_control", "cbr", 0);
      av_dict_set(&o, "scenario", "display_remoting", 0);
    }
    const int rc = avcodec_open2(c, codec, &o);
    av_dict_free(&o);
    if (rc < 0) {
      tried += name + " (" + averr(rc) + "); ";
      avcodec_free_context(&c);
      continue;
    }
    ctx_ = c;
    name_ = name;
    break;
  }
  if (!ctx_) throw std::runtime_error("no H.264 encoder could be opened: " + tried);
  if (ctx_->extradata_size > 0) {
    std::span<const uint8_t> ex(ctx_->extradata, static_cast<size_t>(ctx_->extradata_size));
    headers_ = isAnnexB(ex) ? std::vector<uint8_t>(ex.begin(), ex.end()) : avccToAnnexB(ex);
  }
  sws_ = sws_getContext(w, h, AV_PIX_FMT_BGRA, w, h, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
  frame_ = av_frame_alloc();
  frame_->format = AV_PIX_FMT_NV12;
  frame_->width = w;
  frame_->height = h;
  if (!sws_ || av_frame_get_buffer(frame_, 0) < 0) throw std::runtime_error("cannot allocate the preview frame");
  forceKey_ = true;
}

PreviewEncoder::~PreviewEncoder() {
  sws_freeContext(sws_);
  av_frame_free(&frame_);
  avcodec_free_context(&ctx_);
}

std::vector<PreviewPacket> PreviewEncoder::encode(const CpuImage& img, int64_t ptsUs) {
  if (static_cast<int>(img.width) != w_ || static_cast<int>(img.height) != h_) {
    throw std::invalid_argument("preview image has the wrong size");
  }
  av_frame_make_writable(frame_);
  const uint8_t* src[1] = {img.bgra.data()};
  const int srcStride[1] = {w_ * 4};
  sws_scale(sws_, src, srcStride, 0, h_, frame_->data, frame_->linesize);
  frame_->pts = index_++;
  frame_->pict_type = forceKey_.exchange(false) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
  std::vector<PreviewPacket> out;
  if (avcodec_send_frame(ctx_, frame_) < 0) return out;
  AVPacket* pkt = av_packet_alloc();
  while (avcodec_receive_packet(ctx_, pkt) >= 0) {
    PreviewPacket p;
    p.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    p.ptsUs = ptsUs;
    std::span<const uint8_t> data(pkt->data, static_cast<size_t>(pkt->size));
    if (p.keyframe && !hasNalType(data, 7)) p.annexB = headers_;
    p.annexB.insert(p.annexB.end(), data.begin(), data.end());
    out.push_back(std::move(p));
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  return out;
}
}  // namespace bc
