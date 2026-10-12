#include "io/srt_output.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
}

#include <stdexcept>

#include "io/h264_encoder.h"

namespace bc {
namespace {
void check(int rc, const char* what) {
  if (rc < 0) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(rc, buf, sizeof(buf));
    throw std::runtime_error(std::string(what) + ": " + buf);
  }
}
}  // namespace

int SrtOutput::interrupt(void* self) { return static_cast<SrtOutput*>(self)->stopRequested() ? 1 : 0; }

void SrtOutput::open() {
  check(avformat_alloc_output_context2(&fmt_, nullptr, "mpegts", opts_.url.c_str()), "mpegts muxer");
  fmt_->interrupt_callback = {&SrtOutput::interrupt, this};
  AVDictionary* d = nullptr;
  for (auto& [k, v] : opts_.avOptions) av_dict_set(&d, k.c_str(), v.c_str(), 0);
  const int rc = avio_open2(&fmt_->pb, opts_.url.c_str(), AVIO_FLAG_WRITE, &fmt_->interrupt_callback, &d);
  av_dict_free(&d);
  check(rc, ("cannot open " + opts_.url).c_str());
}

void SrtOutput::openEncoders(const OutFrame& f) {
  encW_ = static_cast<int>(f.bgra->width) & ~1;  // H.264 4:2:0 needs even sizes
  encH_ = static_cast<int>(f.bgra->height) & ~1;
  auto v = openH264Encoder(encW_, encH_, f.rate.num, f.rate.den, int64_t{opts_.bitrateKbps} * 1000,
                           f.rate.num / f.rate.den, false);
  venc_ = v.ctx;
  vst_ = avformat_new_stream(fmt_, nullptr);
  avcodec_parameters_from_context(vst_->codecpar, venc_);
  vst_->time_base = venc_->time_base;

  const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
  aenc_ = avcodec_alloc_context3(ac);
  aenc_->sample_rate = 48000;
  aenc_->sample_fmt = AV_SAMPLE_FMT_FLTP;
  av_channel_layout_default(&aenc_->ch_layout, 2);
  aenc_->bit_rate = 160000;
  aenc_->time_base = {1, 48000};
  check(avcodec_open2(aenc_, ac, nullptr), "aac encoder");
  ast_ = avformat_new_stream(fmt_, nullptr);
  avcodec_parameters_from_context(ast_->codecpar, aenc_);
  ast_->time_base = aenc_->time_base;

  vframe_ = av_frame_alloc();
  vframe_->format = AV_PIX_FMT_NV12;
  vframe_->width = encW_;
  vframe_->height = encH_;
  check(av_frame_get_buffer(vframe_, 0), "video frame");
  aframe_ = av_frame_alloc();
  aframe_->format = AV_SAMPLE_FMT_FLTP;
  av_channel_layout_copy(&aframe_->ch_layout, &aenc_->ch_layout);
  aframe_->sample_rate = 48000;
  aframe_->nb_samples = aenc_->frame_size;  // 1024
  check(av_frame_get_buffer(aframe_, 0), "audio frame");
  check(avformat_write_header(fmt_, nullptr), "mpegts header");
  headerWritten_ = true;
}

void SrtOutput::writePacket(AVCodecContext* c, AVStream* st) {
  AVPacket* pkt = av_packet_alloc();
  while (avcodec_receive_packet(c, pkt) >= 0) {
    av_packet_rescale_ts(pkt, c->time_base, st->time_base);
    pkt->stream_index = st->index;
    const int rc = av_interleaved_write_frame(fmt_, pkt);
    if (rc < 0) {
      av_packet_free(&pkt);
      check(rc, "srt write");  // throws → close, offline, retry
    }
  }
  av_packet_free(&pkt);
}

void SrtOutput::deliver(const OutFrame& f) {
  if (!f.bgra || f.bgra->bgra.empty()) return;
  if (!venc_) openEncoders(f);
  if (!sws_ || swsW_ != f.bgra->width || swsH_ != f.bgra->height) {
    sws_ = sws_getCachedContext(sws_, static_cast<int>(f.bgra->width), static_cast<int>(f.bgra->height), AV_PIX_FMT_BGRA,
                                encW_, encH_, AV_PIX_FMT_NV12, SWS_BILINEAR, nullptr, nullptr, nullptr);
    swsW_ = f.bgra->width;
    swsH_ = f.bgra->height;
  }
  check(av_frame_make_writable(vframe_), "video frame writable");
  const uint8_t* src[1] = {f.bgra->bgra.data()};
  const int stride[1] = {static_cast<int>(f.bgra->width * 4)};
  sws_scale(sws_, src, stride, 0, static_cast<int>(f.bgra->height), vframe_->data, vframe_->linesize);
  vframe_->pts = vpts_++;
  check(avcodec_send_frame(venc_, vframe_), "encode video");
  writePacket(venc_, vst_);

  if (f.audio) audioFifo_.insert(audioFifo_.end(), f.audio->begin(), f.audio->end());
  const int n = aframe_->nb_samples;
  while (static_cast<int>(audioFifo_.size()) >= n * 2) {
    check(av_frame_make_writable(aframe_), "audio frame writable");
    auto* l = reinterpret_cast<float*>(aframe_->data[0]);
    auto* r = reinterpret_cast<float*>(aframe_->data[1]);
    for (int i = 0; i < n; ++i) {
      l[i] = audioFifo_[static_cast<size_t>(i) * 2];
      r[i] = audioFifo_[static_cast<size_t>(i) * 2 + 1];
    }
    audioFifo_.erase(audioFifo_.begin(), audioFifo_.begin() + n * 2);
    aframe_->pts = apts_;
    apts_ += n;
    check(avcodec_send_frame(aenc_, aframe_), "encode audio");
    writePacket(aenc_, ast_);
  }
}

void SrtOutput::close() noexcept {
  if (fmt_ && headerWritten_) av_write_trailer(fmt_);
  if (fmt_ && fmt_->pb) avio_closep(&fmt_->pb);
  if (fmt_) avformat_free_context(fmt_);
  fmt_ = nullptr;
  avcodec_free_context(&venc_);
  avcodec_free_context(&aenc_);
  av_frame_free(&vframe_);
  av_frame_free(&aframe_);
  sws_freeContext(sws_);
  sws_ = nullptr;
  swsW_ = swsH_ = 0;
  audioFifo_.clear();
  vpts_ = apts_ = 0;
  headerWritten_ = false;
  vst_ = ast_ = nullptr;
}
}  // namespace bc
