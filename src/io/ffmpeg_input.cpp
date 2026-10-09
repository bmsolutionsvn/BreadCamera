#include "io/ffmpeg_input.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <cstdlib>
#include <memory>
#include <stdexcept>

#include <srt/srt.h>

#include "core/backoff.h"

namespace bc {
namespace {
using namespace std::chrono;

struct FmtDel { void operator()(AVFormatContext* f) const { avformat_close_input(&f); } };
struct CodecDel { void operator()(AVCodecContext* c) const { avcodec_free_context(&c); } };
struct PacketDel { void operator()(AVPacket* p) const { av_packet_free(&p); } };
struct FrameDel { void operator()(AVFrame* f) const { av_frame_free(&f); } };
using FmtPtr = std::unique_ptr<AVFormatContext, FmtDel>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDel>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDel>;
using FramePtr = std::unique_ptr<AVFrame, FrameDel>;

int64_t nowMs() { return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count(); }

// Consecutive video packets that produce no frame before runOnce gives up and throws
// (feeding the Offline + error + reconnect path, spec §3.6). A decoder that accepts
// packets but emits nothing — the hardware-decode stall this guards — would otherwise
// sit in `connecting` forever: the read timeout only covers packet starvation.
// 32 is deliberately generous: real decoders emit their first frame within at most
// ~16 input packets (reorder/DPB bound), so this tolerates 2x startup jitter and still
// trips within about a second of video at 30 fps.
constexpr int kMaxConsecutiveDecodeFailures = 32;

std::string averr(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(rc, buf, sizeof(buf));
  return buf;
}

AVPixelFormat pickFormat(AVCodecContext*, const AVPixelFormat* fmts) {
  for (auto* p = fmts; *p != AV_PIX_FMT_NONE; ++p) if (*p == AV_PIX_FMT_D3D11) return *p;
  for (auto* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
    if (!(av_pix_fmt_desc_get(*p)->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
  }
  return AV_PIX_FMT_NONE;
}

CodecPtr openDecoder(AVStream* st, AVBufferRef* hw) {
  const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!codec) throw std::runtime_error(std::string("unsupported codec ") + avcodec_get_name(st->codecpar->codec_id));
  CodecPtr ctx(avcodec_alloc_context3(codec));
  avcodec_parameters_to_context(ctx.get(), st->codecpar);
  ctx->pkt_timebase = st->time_base;
  if (hw) {
    ctx->hw_device_ctx = av_buffer_ref(hw);
    ctx->get_format = &pickFormat;
    ctx->extra_hw_frames = 8;
  }
  int rc = avcodec_open2(ctx.get(), codec, nullptr);
  if (rc < 0) throw std::runtime_error("cannot open decoder: " + averr(rc));
  return ctx;
}
}  // namespace

FfmpegInput::FfmpegInput(GpuDevice& gpu, TexturePool& pool, FfmpegInputOptions opts)
    : gpu_(gpu), pool_(pool), opts_(std::move(opts)) {
  static std::once_flag once;
  std::call_once(once, [] {
    av_log_set_level(AV_LOG_ERROR);
    // Hold one libsrt instance reference for the whole process. FFmpeg pairs
    // srt_startup()/srt_cleanup() per URL, and a cleanup that drops the instance
    // count to zero runs the socket GC with a fixed 1 s grace period (Haivision/srt
    // #3379, fixed in 1.5.8) — that would break stop()'s 1 s bound on every failed
    // or closed SRT open. The held reference keeps FFmpeg's cleanups on the cheap
    // early-return path; the atexit releases it so the GC thread joins at exit.
    srt_startup();
    std::atexit([] { srt_cleanup(); });
  });
}

FfmpegInput::~FfmpegInput() { stop(); }

void FfmpegInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
  thread_ = std::thread([this] { run(); });
}

void FfmpegInput::stop() {
  {
    // Hold waitMutex_ across the store: the backoff wait evaluates its predicate
    // under this mutex, so a notify that is not ordered by it can land between the
    // predicate check and the wait's atomic block — a lost wakeup that costs up to
    // the full backoff (5 s cap) before stop() gets to join.
    std::lock_guard l(waitMutex_);
    stopping_ = true;
  }
  waitCv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

int FfmpegInput::interruptCb(void* self) {
  auto* in = static_cast<FfmpegInput*>(self);
  return (in->stopping_ || nowMs() > in->deadlineMs_) ? 1 : 0;
}

std::string FfmpegInput::safeUrl() const { return opts_.url.substr(0, opts_.url.find('?')); }

AVBufferRef* FfmpegInput::makeHwDevice() {
  AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  auto* dctx = reinterpret_cast<AVHWDeviceContext*>(ref->data);
  auto* d3d = static_cast<AVD3D11VADeviceContext*>(dctx->hwctx);
  d3d->device = gpu_.device();
  d3d->device->AddRef();
  // FFmpeg takes our context lock, so its decode calls never interleave with ours.
  d3d->lock_ctx = &gpu_.contextMutex();
  d3d->lock = [](void* m) { static_cast<std::recursive_mutex*>(m)->lock(); };
  d3d->unlock = [](void* m) { static_cast<std::recursive_mutex*>(m)->unlock(); };
  int rc = av_hwdevice_ctx_init(ref);
  if (rc < 0) {
    av_buffer_unref(&ref);
    throw std::runtime_error("d3d11va init failed: " + averr(rc));
  }
  return ref;
}

void FfmpegInput::run() {
  Backoff backoff;
  while (!stopping_) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    wentLive_ = false;
    std::optional<ErrorInfo> err;
    try {
      runOnce();
      err = ErrorInfo{"input-ended", "the stream ended: " + safeUrl()};
    } catch (const std::exception& e) {
      err = ErrorInfo{"input-failed", e.what()};
    }
    if (wentLive_) backoff.reset();
    if (stopping_) break;
    sink_->onState(SignalState::Offline, err);
    if (!opts_.reconnect) return;
    std::unique_lock l(waitMutex_);
    waitCv_.wait_for(l, backoff.next(), [&] { return stopping_.load(); });
  }
  sink_->onState(SignalState::Offline, std::nullopt);
}

void FfmpegInput::runOnce() {
  AVFormatContext* raw = avformat_alloc_context();
  raw->interrupt_callback = {&FfmpegInput::interruptCb, this};
  deadlineMs_ = INT64_MAX;  // a listener may wait for its caller indefinitely; stop() interrupts
  AVDictionary* dict = nullptr;
  for (auto& [k, v] : opts_.avOptions) av_dict_set(&dict, k.c_str(), v.c_str(), 0);
  av_dict_set(&dict, "analyzeduration", "1000000", 0);
  av_dict_set(&dict, "probesize", "2000000", 0);
  int rc = avformat_open_input(&raw, opts_.url.c_str(), nullptr, &dict);
  av_dict_free(&dict);
  if (rc < 0) throw std::runtime_error("cannot open " + safeUrl() + ": " + averr(rc));
  FmtPtr fmt(raw);

  deadlineMs_ = nowMs() + opts_.readTimeout.count() * 3;
  rc = avformat_find_stream_info(fmt.get(), nullptr);
  if (rc < 0) throw std::runtime_error("cannot read stream info from " + safeUrl() + ": " + averr(rc));
  const int vi = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vi < 0) throw std::runtime_error("no video stream in " + safeUrl());
  const int ai = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, vi, nullptr, 0);

  AVBufferRef* hw = nullptr;
  if (opts_.allowHwDecode && gpu_.supportsVideoDecode()) {
    try {
      hw = makeHwDevice();
    } catch (const std::exception&) {
      hw = nullptr;  // fall back to software decode
    }
  }
  std::unique_ptr<AVBufferRef*, void (*)(AVBufferRef**)> hwGuard(&hw, av_buffer_unref);
  AVStream* vst = fmt->streams[vi];
  CodecPtr vdec = openDecoder(vst, hw);
  CodecPtr adec = ai >= 0 ? openDecoder(fmt->streams[ai], nullptr) : nullptr;

  PacketPtr pkt(av_packet_alloc());
  FramePtr frame(av_frame_alloc());
  paceFirstUs_ = INT64_MIN;
  struct Cleanup {
    FfmpegInput* self;
    ~Cleanup() {
      sws_freeContext(self->sws_);
      self->sws_ = nullptr;
      swr_free(&self->swr_);
    }
  } cleanup{this};

  int decodeFailures = 0;  // consecutive video packets that produced no frame

  while (!stopping_) {
    deadlineMs_ = nowMs() + opts_.readTimeout.count();
    rc = av_read_frame(fmt.get(), pkt.get());
    if (rc == AVERROR_EOF) return;
    if (rc < 0) {
      if (stopping_) return;
      if (rc == AVERROR_EXIT) {
        throw std::runtime_error("no data from " + safeUrl() + " for " + std::to_string(opts_.readTimeout.count()) + " ms");
      }
      throw std::runtime_error("read failed on " + safeUrl() + ": " + averr(rc));
    }
    AVCodecContext* dec = pkt->stream_index == vi ? vdec.get() : (pkt->stream_index == ai ? adec.get() : nullptr);
    if (dec) {
      bool gotFrame = false;
      std::string why;
      const int sendRc = avcodec_send_packet(dec, pkt.get());
      if (sendRc < 0) {
        why = averr(sendRc);
      } else {
        for (;;) {
          const int recvRc = avcodec_receive_frame(dec, frame.get());
          if (recvRc < 0) {
            if (recvRc != AVERROR(EAGAIN) && recvRc != AVERROR_EOF) why = averr(recvRc);
            break;
          }
          gotFrame = true;
          if (dec == vdec.get()) emitVideo(frame.get(), vst);
          else emitAudio(frame.get());
          av_frame_unref(frame.get());
        }
      }
      if (dec == vdec.get()) {
        if (gotFrame) {
          decodeFailures = 0;
        } else if (++decodeFailures >= kMaxConsecutiveDecodeFailures) {
          // A decode that accepts packets but emits nothing must surface as an error
          // and a reconnect, not as a silent `connecting` forever (spec §3.6).
          throw std::runtime_error("video decode failed on " + safeUrl() + ": " +
                                   std::to_string(decodeFailures) + " consecutive packets produced no frame" +
                                   (why.empty() ? "" : " (" + why + ")"));
        }
      }
    }
    av_packet_unref(pkt.get());
  }
}

void FfmpegInput::pace(int64_t ptsUs) {
  if (!opts_.realtime) return;
  if (paceFirstUs_ == INT64_MIN) {
    paceFirstUs_ = ptsUs;
    paceStart_ = steady_clock::now();
    return;
  }
  std::unique_lock l(waitMutex_);
  waitCv_.wait_until(l, paceStart_ + microseconds(ptsUs - paceFirstUs_), [&] { return stopping_.load(); });
}

void FfmpegInput::emitVideo(AVFrame* f, AVStream* st) {
  VideoFrameIn out;
  out.fullRange = f->color_range == AVCOL_RANGE_JPEG;
  out.ptsUs = f->best_effort_timestamp == AV_NOPTS_VALUE ? 0 : av_rescale_q(f->best_effort_timestamp, st->time_base, {1, 1'000'000});
  const uint32_t w = static_cast<uint32_t>(f->width) & ~1u;
  const uint32_t h = static_cast<uint32_t>(f->height) & ~1u;
  if (f->format == AV_PIX_FMT_D3D11) {
    auto* src = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
    const UINT index = static_cast<UINT>(reinterpret_cast<intptr_t>(f->data[1]));
    D3D11_TEXTURE2D_DESC d{};
    src->GetDesc(&d);
    out.layout = d.Format == DXGI_FORMAT_P010 ? PixelLayout::P010 : PixelLayout::NV12;
    out.tex = pool_.acquire(w, h, d.Format, D3D11_BIND_SHADER_RESOURCE);
    const D3D11_BOX box{0, 0, 0, w, h, 1};
    auto lock = gpu_.lock();
    gpu_.context()->CopySubresourceRegion(out.tex->tex.Get(), 0, 0, 0, 0, src, index, &box);
  } else {
    sws_ = sws_getCachedContext(sws_, f->width, f->height, static_cast<AVPixelFormat>(f->format), static_cast<int>(w),
                                static_cast<int>(h), AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) throw std::runtime_error("cannot convert pixel format");
    const int cs = f->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    sws_setColorspaceDetails(sws_, sws_getCoefficients(cs), f->color_range == AVCOL_RANGE_JPEG,
                             sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    swBuf_.resize(static_cast<size_t>(w) * h * 4);
    uint8_t* dst[1] = {swBuf_.data()};
    const int dstStride[1] = {static_cast<int>(w * 4)};
    sws_scale(sws_, f->data, f->linesize, 0, f->height, dst, dstStride);
    out.tex = pool_.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    {
      auto lock = gpu_.lock();
      gpu_.context()->UpdateSubresource(out.tex->tex.Get(), 0, nullptr, swBuf_.data(), w * 4, 0);
    }
    out.layout = PixelLayout::BGRA8;
    out.fullRange = true;
  }
  pace(out.ptsUs);
  if (!wentLive_) {
    wentLive_ = true;
    sink_->onState(SignalState::Live, std::nullopt);
  }
  sink_->onVideo(std::move(out));
}

void FfmpegInput::emitAudio(AVFrame* f) {
  if (!swr_) {
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_FLT, 48000, &f->ch_layout,
                            static_cast<AVSampleFormat>(f->format), f->sample_rate, 0, nullptr) < 0 ||
        swr_init(swr_) < 0) {
      throw std::runtime_error("cannot set up audio resampling");
    }
  }
  // ponytail: the resampler is built from the first frame; a mid-stream format change is
  // not followed. Rebuild on change if a real source does that.
  const int maxOut = swr_get_out_samples(swr_, f->nb_samples);
  AudioBlock blk;
  blk.samples.resize(static_cast<size_t>(maxOut) * 2);
  uint8_t* outPtr[1] = {reinterpret_cast<uint8_t*>(blk.samples.data())};
  const int got = swr_convert(swr_, outPtr, maxOut, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
  if (got <= 0) return;
  blk.samples.resize(static_cast<size_t>(got) * 2);
  sink_->onAudio(std::move(blk));
}
}  // namespace bc
