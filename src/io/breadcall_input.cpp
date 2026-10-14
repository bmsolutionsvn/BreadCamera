#include "io/breadcall_input.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "core/backoff.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#if BC_WITH_LIVECALL
#include "io/lkc/lkc.h"
#endif

namespace bc {
namespace {
using namespace std::chrono;

// Like FfmpegInput: a decoder that accepts packets but emits nothing must
// surface as an error and a reconnect, not as `connecting` forever (spec §3.6).
constexpr int kMaxConsecutiveDecodeFailures = 32;
constexpr int kMinSlot = 1;
constexpr int kMaxSlot = 8;  // D4: slots 1..8
constexpr auto kNoGuestGrace = seconds(5);
constexpr auto kPoll = milliseconds(200);
// Bound on the sample queue; a decode that cannot keep up drops the newest
// sample rather than growing without limit (a live camera may drop frames).
constexpr size_t kMaxQueued = 256;

std::string averr(int rc) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(rc, buf, sizeof(buf));
  return buf;
}

// Mirrors lkc's own admission checks so a bad url/token is a terminal
// `input-config` without waiting on the client (spec: "bad url/token").
bool urlLooksValid(const std::string& url, std::string& err) {
  const auto pos = url.find("://");
  if (pos == std::string::npos) {
    err = "url has no scheme";
    return false;
  }
  const std::string scheme = url.substr(0, pos);
  const std::string rest = url.substr(pos + 3);
  const std::string host = rest.substr(0, rest.find('/'));
  if (host.empty()) {
    err = "url has no host";
    return false;
  }
  if (scheme != "ws" && scheme != "wss" && scheme != "http" && scheme != "https") {
    err = "unsupported url scheme '" + scheme + "'";
    return false;
  }
  return true;
}

// A LiveKit access token is a JWT: three non-empty base64url segments.
bool tokenLooksValid(const std::string& t) {
  int dots = 0;
  size_t seg = 0;
  for (char c : t) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '.') {
      if (seg == 0) return false;
      ++dots;
      seg = 0;
    } else if (std::isalnum(u) || c == '-' || c == '_') {
      ++seg;
    } else {
      return false;
    }
  }
  return dots == 2 && seg > 0;
}
}  // namespace

BreadCallInput::BreadCallInput(GpuDevice& gpu, TexturePool& pool, std::string url, std::string token, int slot)
    : gpu_(gpu), pool_(pool), url_(std::move(url)), token_(std::move(token)), slot_(slot) {
  static std::once_flag once;
  std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
  frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
}

BreadCallInput::~BreadCallInput() {
  stop();
  if (sws_) sws_freeContext(sws_);
  if (swr_) swr_free(&swr_);
  if (vdec_) avcodec_free_context(&vdec_);
  if (adec_) avcodec_free_context(&adec_);
  if (frame_) av_frame_free(&frame_);
  if (packet_) av_packet_free(&packet_);
}

void BreadCallInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
#if !BC_WITH_LIVECALL
  sink_->onState(SignalState::Connecting, std::nullopt);
  sink_->onState(SignalState::Offline,
                 ErrorInfo{"input-config", "BreadCall support is not built (BC_WITH_LIVECALL=0)"});
#else
  if (slot_ < kMinSlot || slot_ > kMaxSlot) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    sink_->onState(SignalState::Offline,
                   ErrorInfo{"breadcall-no-guest",
                             "no guest in slot " + std::to_string(slot_) + " (slots are 1..8)"});
    return;
  }
  std::string why;
  if (!urlLooksValid(url_, why)) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    sink_->onState(SignalState::Offline,
                   ErrorInfo{"input-config", "the BreadCall room url is unusable: " + why + ": " + url_});
    return;
  }
  if (!tokenLooksValid(token_)) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    sink_->onState(SignalState::Offline,
                   ErrorInfo{"input-config", "the BreadCall token is not a LiveKit access token (JWT)"});
    return;
  }
  // The worker emits Connecting for this and every retry attempt.
  thread_ = std::thread([this] { run(); });
#endif
}

void BreadCallInput::stop() {
  {
    std::lock_guard l(m_);
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void BreadCallInput::push(Event ev) {
  {
    std::lock_guard l(m_);
    if (stopping_ || queue_.size() >= kMaxQueued) return;
    queue_.push_back(std::move(ev));
  }
  cv_.notify_all();
}

void BreadCallInput::setFailure(ErrorInfo err, bool terminal) {
  sessionError_ = std::move(err);
  terminal_ = terminal;
  sessionDone_ = true;
}

void BreadCallInput::run() {
#if BC_WITH_LIVECALL
  Backoff backoff;
  while (!stopping_) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    {
      std::lock_guard l(m_);
      queue_.clear();
      sessionDone_ = false;
      terminal_ = false;
      sessionError_.reset();
      liveSeen_ = false;
      emittedLive_ = false;
      noGuestReported_ = false;
      noGuestDeadline_ = {};
      videoDecodeFailures_ = 0;
    }
    auto client = std::make_unique<lkc::Client>(url_, token_);
    client->onState([this](SignalState s, std::optional<ErrorInfo> e) {
      Event ev;
      ev.kind = Event::Kind::State;
      ev.state = s;
      ev.error = std::move(e);
      push(std::move(ev));
    });
    client->onVideo([this](lkc::VideoSample s) {
      Event ev;
      ev.kind = Event::Kind::Video;
      ev.bytes = std::move(s.annexB);
      ev.rtpTimestamp = s.rtpTimestamp;
      ev.receiveUs = s.receiveUs;
      push(std::move(ev));
    });
    client->onAudio([this](lkc::AudioSample s) {
      Event ev;
      ev.kind = Event::Kind::Audio;
      ev.bytes = std::move(s.opus);
      ev.rtpTimestamp = s.rtpTimestamp;
      ev.receiveUs = s.receiveUs;
      push(std::move(ev));
    });
    client->subscribe("guest_" + std::to_string(slot_));

    bool wentLive = false;
    while (!stopping_ && !sessionDone_) {
      Event ev;
      {
        std::unique_lock l(m_);
        const bool ready =
            cv_.wait_for(l, kPoll, [&] { return stopping_ || sessionDone_ || !queue_.empty(); });
        if (stopping_) break;
        if (!ready) {
          l.unlock();
          checkNoGuest();
          continue;
        }
        if (sessionDone_) break;
        ev = std::move(queue_.front());
        queue_.pop_front();
      }
      const bool wasLive = emittedLive_;
      try {
        process(std::move(ev));
      } catch (const std::exception& e) {
        setFailure(ErrorInfo{"input-failed", e.what()}, false);
      }
      if (emittedLive_ && !wasLive) wentLive = true;
    }
    // Destroy the client on this worker, never from inside one of its callbacks:
    // ~Client joins the client's worker, so any in-flight callback finishes first.
    client.reset();
    if (stopping_) break;
    if (wentLive) backoff.reset();
    sink_->onState(SignalState::Offline, sessionError_);
    if (terminal_) return;  // a config error is terminal (operator re-mints/re-points)
    std::unique_lock l(m_);
    cv_.wait_for(l, backoff.next(), [&] { return stopping_.load(); });
  }
#endif
  if (!stopping_) sink_->onState(SignalState::Offline, std::nullopt);
}

void BreadCallInput::process(Event&& ev) {
  switch (ev.kind) {
    case Event::Kind::State:
      if (ev.state == SignalState::Live) {
        // The media connection is up. Start the grace clock for the slot's guest.
        liveSeen_ = true;
        if (!emittedLive_) noGuestDeadline_ = steady_clock::now() + kNoGuestGrace;
      } else if (ev.state == SignalState::Offline) {
        const std::string code = ev.error ? ev.error->code : std::string();
        if (code == "lkc-unavailable") {
          setFailure(ErrorInfo{"input-config", ev.error ? ev.error->message : "BreadCall support is not built"},
                     true);
        } else {
          // lkc-config / lkc-unreachable / lkc-media are all recoverable while the
          // operator keeps the camera pointed at the room (review focus 1).
          setFailure(ErrorInfo{"input-failed", ev.error ? ev.error->message : "the BreadCall room dropped"},
                     false);
        }
      }
      break;
    case Event::Kind::Video:
      decodeVideo(std::move(ev));
      break;
    case Event::Kind::Audio:
      decodeAudio(std::move(ev));
      break;
  }
}

void BreadCallInput::checkNoGuest() {
  if (!liveSeen_ || emittedLive_ || noGuestReported_) return;
  if (noGuestDeadline_.time_since_epoch().count() == 0) return;
  if (steady_clock::now() < noGuestDeadline_) return;
  noGuestReported_ = true;
  // Keep the session: the PC stays connected, so a guest that joins later is
  // picked up (the slot's camera comes alive) without a reconnect storm.
  sink_->onState(SignalState::Offline,
                 ErrorInfo{"breadcall-no-guest", "no guest in slot " + std::to_string(slot_)});
}

bool BreadCallInput::openVideo() {
  if (vdec_) return true;
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec) {
    setFailure(ErrorInfo{"input-failed", "FFmpeg has no H.264 decoder"}, false);
    return false;
  }
  vdec_ = avcodec_alloc_context3(codec);
  if (!vdec_ || avcodec_open2(vdec_, codec, nullptr) < 0) {
    setFailure(ErrorInfo{"input-failed", "cannot open the H.264 decoder"}, false);
    return false;
  }
  return true;
}

bool BreadCallInput::openAudio() {
  if (adec_) return true;
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
  if (!codec) {
    setFailure(ErrorInfo{"input-failed", "FFmpeg has no Opus decoder"}, false);
    return false;
  }
  adec_ = avcodec_alloc_context3(codec);
  // No extradata: FFmpeg's Opus decoder defaults to 48 kHz stereo, matching the
  // sink contract; the packet TOC selects mono/stereo per frame.
  if (!adec_ || avcodec_open2(adec_, codec, nullptr) < 0) {
    setFailure(ErrorInfo{"input-failed", "cannot open the Opus decoder"}, false);
    return false;
  }
  return true;
}

void BreadCallInput::decodeVideo(Event&& ev) {
  if (ev.bytes.empty() || !frame_ || !packet_) return;
  if (!openVideo()) return;
  if (av_new_packet(packet_, static_cast<int>(ev.bytes.size())) < 0) {
    setFailure(ErrorInfo{"input-failed", "cannot allocate a decode packet"}, false);
    return;
  }
  std::memcpy(packet_->data, ev.bytes.data(), ev.bytes.size());
  bool got = false;
  std::string why;
  const int sendRc = avcodec_send_packet(vdec_, packet_);
  av_packet_unref(packet_);
  if (sendRc < 0) {
    // A partial/first-fragment NAL (the client starts mid-stream) is not fatal:
    // count it like FfmpegInput does and only give up after a run of them.
    why = averr(sendRc);
  } else {
    for (;;) {
      const int rc = avcodec_receive_frame(vdec_, frame_);
      if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
      if (rc < 0) {
        why = averr(rc);
        break;
      }
      got = true;
      emitVideo(frame_, ev.receiveUs);
      av_frame_unref(frame_);
      if (sessionDone_) return;  // emitVideo failed (e.g. conversion/upload)
    }
  }
  if (got) {
    videoDecodeFailures_ = 0;
  } else if (++videoDecodeFailures_ >= kMaxConsecutiveDecodeFailures) {
    setFailure(ErrorInfo{"input-failed", "video decode failed: " + std::to_string(videoDecodeFailures_) +
                                              " consecutive packets produced no frame" +
                                              (why.empty() ? "" : " (" + why + ")")},
               false);
  }
}

void BreadCallInput::decodeAudio(Event&& ev) {
  if (ev.bytes.empty() || !frame_ || !packet_) return;
  if (!openAudio()) return;
  if (av_new_packet(packet_, static_cast<int>(ev.bytes.size())) < 0) return;
  std::memcpy(packet_->data, ev.bytes.data(), ev.bytes.size());
  const int sendRc = avcodec_send_packet(adec_, packet_);
  av_packet_unref(packet_);
  if (sendRc < 0) return;  // a single bad Opus packet is not worth dropping video for
  for (;;) {
    const int rc = avcodec_receive_frame(adec_, frame_);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
    if (rc < 0) {
      av_frame_unref(frame_);
      return;
    }
    emitAudio(frame_);
    av_frame_unref(frame_);
    if (sessionDone_) return;
  }
}

void BreadCallInput::emitVideo(AVFrame* frame, int64_t receiveUs) {
  const uint32_t w = static_cast<uint32_t>(frame->width) & ~1u;
  const uint32_t h = static_cast<uint32_t>(frame->height) & ~1u;
  if (w == 0 || h == 0) return;
  sws_ = sws_getCachedContext(sws_, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                              static_cast<int>(w), static_cast<int>(h), AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
                              nullptr, nullptr);
  if (!sws_) {
    setFailure(ErrorInfo{"input-failed", "cannot convert the guest's pixel format"}, false);
    return;
  }
  const int cs = frame->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
  sws_setColorspaceDetails(sws_, sws_getCoefficients(cs), frame->color_range == AVCOL_RANGE_JPEG,
                           sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
  bgra_.resize(static_cast<size_t>(w) * h * 4);
  uint8_t* dst[1] = {bgra_.data()};
  const int dstStride[1] = {static_cast<int>(w * 4)};
  sws_scale(sws_, frame->data, frame->linesize, 0, frame->height, dst, dstStride);

  VideoFrameIn out;
  out.layout = PixelLayout::BGRA8;
  out.fullRange = true;
  out.ptsUs = receiveUs;
  out.tex = pool_.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
  {
    auto lock = gpu_.lock();
    gpu_.context()->UpdateSubresource(out.tex->tex.Get(), 0, nullptr, bgra_.data(), w * 4, 0);
  }
  if (!emittedLive_) {
    emittedLive_ = true;
    noGuestReported_ = false;
    sink_->onState(SignalState::Live, std::nullopt);
  }
  sink_->onVideo(std::move(out));  // after the GPU lock is released
}

void BreadCallInput::emitAudio(AVFrame* frame) {
  if (!swr_) {
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (swr_alloc_set_opts2(&swr_, &stereo, AV_SAMPLE_FMT_FLT, 48000, &frame->ch_layout,
                            static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) < 0 ||
        swr_init(swr_) < 0) {
      setFailure(ErrorInfo{"input-failed", "cannot set up audio resampling"}, false);
      return;
    }
  }
  const int maxOut = swr_get_out_samples(swr_, frame->nb_samples);
  if (maxOut <= 0) return;
  AudioBlock blk;
  blk.samples.resize(static_cast<size_t>(maxOut) * 2);
  uint8_t* outPtr[1] = {reinterpret_cast<uint8_t*>(blk.samples.data())};
  const int got = swr_convert(swr_, outPtr, maxOut, const_cast<const uint8_t**>(frame->extended_data),
                              frame->nb_samples);
  if (got <= 0) return;
  blk.samples.resize(static_cast<size_t>(got) * 2);
  sink_->onAudio(std::move(blk));
}
}  // namespace bc
