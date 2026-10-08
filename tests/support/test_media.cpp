#include "support/test_media.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace bc::test {
namespace {
void check(int rc, const char* what) {
  if (rc < 0) {
    char buf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(rc, buf, sizeof(buf));
    throw std::runtime_error(std::string(what) + ": " + buf);
  }
}
struct CodecDel { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
struct FrameDel { void operator()(AVFrame* f) const { av_frame_free(&f); } };
struct PacketDel { void operator()(AVPacket* p) const { av_packet_free(&p); } };
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDel>;
using FramePtr = std::unique_ptr<AVFrame, FrameDel>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDel>;

void drain(AVFormatContext* oc, AVCodecContext* c, AVStream* st) {
  PacketPtr pkt(av_packet_alloc());
  for (;;) {
    int rc = avcodec_receive_packet(c, pkt.get());
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return;
    check(rc, "receive_packet");
    av_packet_rescale_ts(pkt.get(), c->time_base, st->time_base);
    pkt->stream_index = st->index;
    check(av_interleaved_write_frame(oc, pkt.get()), "write_frame");
  }
}
}  // namespace

void writeTestTs(const std::filesystem::path& path, int width, int height, int fps, int seconds, bool withAudio) {
  const std::string file = path.string();
  AVFormatContext* oc = nullptr;
  check(avformat_alloc_output_context2(&oc, nullptr, "mpegts", file.c_str()), "alloc_output");
  std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> ocGuard(oc, &avformat_free_context);

  const AVCodec* vc = avcodec_find_encoder(AV_CODEC_ID_MPEG2VIDEO);
  CodecPtr v(avcodec_alloc_context3(vc));
  v->width = width;
  v->height = height;
  v->time_base = {1, fps};
  v->framerate = {fps, 1};
  v->pix_fmt = AV_PIX_FMT_YUV420P;
  v->gop_size = fps / 2;
  v->max_b_frames = 0;
  v->bit_rate = 8'000'000;
  check(avcodec_open2(v.get(), vc, nullptr), "open mpeg2video");
  AVStream* vs = avformat_new_stream(oc, nullptr);
  avcodec_parameters_from_context(vs->codecpar, v.get());
  vs->time_base = v->time_base;

  CodecPtr a;
  AVStream* as = nullptr;
  if (withAudio) {
    const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_MP2);
    a.reset(avcodec_alloc_context3(ac));
    a->sample_rate = 48000;
    a->sample_fmt = AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&a->ch_layout, 2);
    a->bit_rate = 192000;
    a->time_base = {1, 48000};
    check(avcodec_open2(a.get(), ac, nullptr), "open mp2");
    as = avformat_new_stream(oc, nullptr);
    avcodec_parameters_from_context(as->codecpar, a.get());
    as->time_base = a->time_base;
  }

  check(avio_open(&oc->pb, file.c_str(), AVIO_FLAG_WRITE), "avio_open");
  check(avformat_write_header(oc, nullptr), "write_header");

  FramePtr vf(av_frame_alloc());
  vf->format = AV_PIX_FMT_YUV420P;
  vf->width = width;
  vf->height = height;
  check(av_frame_get_buffer(vf.get(), 0), "frame buffer");
  FramePtr af;
  if (a) {
    af.reset(av_frame_alloc());
    af->format = AV_SAMPLE_FMT_S16;
    av_channel_layout_copy(&af->ch_layout, &a->ch_layout);
    af->sample_rate = 48000;
    af->nb_samples = a->frame_size;
    check(av_frame_get_buffer(af.get(), 0), "audio buffer");
  }

  int64_t audioPts = 0;
  for (int i = 0; i < fps * seconds; ++i) {
    check(av_frame_make_writable(vf.get()), "writable");
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x)
        vf->data[0][y * vf->linesize[0] + x] = static_cast<uint8_t>(16 + (x + i * 4) % 220);
    for (int y = 0; y < height / 2; ++y)
      for (int x = 0; x < width / 2; ++x) {
        vf->data[1][y * vf->linesize[1] + x] = 128;
        vf->data[2][y * vf->linesize[2] + x] = static_cast<uint8_t>(64 + i % 128);
      }
    vf->pts = i;
    check(avcodec_send_frame(v.get(), vf.get()), "send video");
    drain(oc, v.get(), vs);
    while (a && audioPts < static_cast<int64_t>(i + 1) * 48000 / fps) {
      check(av_frame_make_writable(af.get()), "writable audio");
      auto* s = reinterpret_cast<int16_t*>(af->data[0]);
      for (int n = 0; n < af->nb_samples; ++n) {
        auto val = static_cast<int16_t>(std::sin(2.0 * 3.141592653589793 * 1000.0 * (audioPts + n) / 48000.0) * 8000);
        s[2 * n] = s[2 * n + 1] = val;
      }
      af->pts = audioPts;
      audioPts += af->nb_samples;
      check(avcodec_send_frame(a.get(), af.get()), "send audio");
      drain(oc, a.get(), as);
    }
  }
  avcodec_send_frame(v.get(), nullptr);
  drain(oc, v.get(), vs);
  if (a) {
    avcodec_send_frame(a.get(), nullptr);
    drain(oc, a.get(), as);
  }
  check(av_write_trailer(oc), "trailer");
  avio_closep(&oc->pb);
}

TsPusher::TsPusher(std::filesystem::path file, std::string url, bool loop)
    : file_(std::move(file)), url_(std::move(url)), loop_(loop), thread_([this] { run(); }) {}

TsPusher::~TsPusher() { stop(); }

void TsPusher::stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
}

int TsPusher::interrupt(void* self) { return static_cast<TsPusher*>(self)->stop_ ? 1 : 0; }

void TsPusher::run() {
  using namespace std::chrono;
  try {
    while (!stop_) {
      AVFormatContext* in = nullptr;
      check(avformat_open_input(&in, file_.string().c_str(), nullptr, nullptr), "open test file");
      std::unique_ptr<AVFormatContext*, void (*)(AVFormatContext**)> inGuard(&in, avformat_close_input);
      check(avformat_find_stream_info(in, nullptr), "stream info");

      AVFormatContext* out = nullptr;
      check(avformat_alloc_output_context2(&out, nullptr, "mpegts", url_.c_str()), "alloc output");
      std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> outGuard(out, &avformat_free_context);
      for (unsigned i = 0; i < in->nb_streams; ++i) {
        AVStream* o = avformat_new_stream(out, nullptr);
        avcodec_parameters_copy(o->codecpar, in->streams[i]->codecpar);
        o->codecpar->codec_tag = 0;
        o->time_base = in->streams[i]->time_base;
      }
      AVIOInterruptCB cb{&TsPusher::interrupt, this};
      int rc = AVERROR(ECONNREFUSED);
      while (!stop_ && (rc = avio_open2(&out->pb, url_.c_str(), AVIO_FLAG_WRITE, &cb, nullptr)) < 0) {
        std::this_thread::sleep_for(200ms);  // receiver not up yet
      }
      if (stop_) return;
      check(avformat_write_header(out, nullptr), "write header");

      PacketPtr pkt(av_packet_alloc());
      const auto t0 = steady_clock::now();
      int64_t firstUs = AV_NOPTS_VALUE, offsetUs = 0, lastUs = 0;
      while (!stop_) {
        rc = av_read_frame(in, pkt.get());
        if (rc == AVERROR_EOF) {
          if (!loop_) break;
          offsetUs = lastUs + 20'000;
          av_seek_frame(in, -1, 0, AVSEEK_FLAG_BACKWARD);
          firstUs = AV_NOPTS_VALUE;
          continue;
        }
        check(rc, "read test file");
        AVStream* is = in->streams[pkt->stream_index];
        const int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        int64_t us = av_rescale_q(ts, is->time_base, {1, 1'000'000});
        if (firstUs == AV_NOPTS_VALUE) firstUs = us;
        us = us - firstUs + offsetUs;
        lastUs = std::max(lastUs, us);
        while (!stop_ && steady_clock::now() < t0 + microseconds(us)) std::this_thread::sleep_for(1ms);
        const int64_t shift = av_rescale_q(offsetUs, {1, 1'000'000}, is->time_base) -
                              av_rescale_q(firstUs, {1, 1'000'000}, is->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += shift;
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += shift;
        av_packet_rescale_ts(pkt.get(), is->time_base, out->streams[pkt->stream_index]->time_base);
        rc = av_interleaved_write_frame(out, pkt.get());
        av_packet_unref(pkt.get());
        if (rc < 0) break;  // receiver went away: reconnect from the top
      }
      av_write_trailer(out);
      avio_closep(&out->pb);
      if (!loop_) return;
    }
  } catch (const std::exception& e) {
    error_ = e.what();
    failed_ = true;
  }
}
}  // namespace bc::test
