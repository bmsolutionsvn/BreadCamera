#include "io/h264_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <stdexcept>

namespace bc {
namespace {
std::string averr(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(rc, buf, sizeof(buf));
  return buf;
}
}  // namespace

const std::vector<std::string>& h264EncoderOrder() {
  static const std::vector<std::string> order{"h264_nvenc", "h264_amf", "h264_qsv", "h264_mf"};
  return order;
}

OpenedEncoder openH264Encoder(int w, int h, int fpsNum, int fpsDen, int64_t bitRate, int gop, bool globalHeader) {
  std::string tried;
  for (const auto& name : h264EncoderOrder()) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (!codec) {
      tried += name + " (not built); ";
      continue;
    }
    AVCodecContext* c = avcodec_alloc_context3(codec);
    c->width = w;
    c->height = h;
    c->time_base = {fpsDen, fpsNum};
    c->framerate = {fpsNum, fpsDen};
    c->pix_fmt = AV_PIX_FMT_NV12;
    c->gop_size = gop;
    c->max_b_frames = 0;
    c->bit_rate = bitRate;
    c->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (globalHeader) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
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
    return {c, name};
  }
  throw std::runtime_error("no H.264 encoder could be opened: " + tried);
}
}  // namespace bc