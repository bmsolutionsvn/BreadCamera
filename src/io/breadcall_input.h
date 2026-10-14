#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;

namespace bc {
// C1: a BreadCall guest slot as a camera. Joins a LiveKit room through the
// shared lkc::Client as a hidden receiver (R5: receive only), subscribes to
// `guest_<slot>`, decodes the guest's H.264 to BGRA8 and Opus to 48 kHz stereo
// float with the FFmpeg BreadCamera already links, and feeds the InputSink
// (spec D4/D5).
//
// Threading: one worker thread owns the client lifecycle (join, retry with
// backoff) and the codecs. The lkc callbacks run on the client's worker and only
// enqueue a sample, so a slow decode can never stall the transport, the sink is
// never called from inside an lkc callback, and the Client is only destroyed on
// this worker (never from inside one of its own callbacks, which would deadlock
// ~Client).
class BreadCallInput final : public Input {
 public:
  BreadCallInput(GpuDevice& gpu, TexturePool& pool, std::string url, std::string token, int slot);
  ~BreadCallInput() override;
  void start(InputSink& sink) override;
  void stop() override;

 private:
  struct Event {
    enum class Kind { Video, Audio, State } kind = Kind::State;
    std::vector<uint8_t> bytes;  // H.264 Annex-B (Video) or one Opus packet (Audio)
    uint32_t rtpTimestamp = 0;
    int64_t receiveUs = 0;
    SignalState state = SignalState::Connecting;
    std::optional<ErrorInfo> error;
  };

  void run();
  void push(Event ev);
  void process(Event&& ev);
  void checkNoGuest();
  void decodeVideo(Event&& ev);
  void decodeAudio(Event&& ev);
  bool openVideo();
  bool openAudio();
  void emitVideo(AVFrame* frame, int64_t receiveUs);
  void emitAudio(AVFrame* frame);
  void setFailure(ErrorInfo err, bool terminal);

  GpuDevice& gpu_;
  TexturePool& pool_;
  std::string url_;
  std::string token_;
  int slot_;

  InputSink* sink_ = nullptr;
  std::thread thread_;
  std::atomic<bool> stopping_{false};

  std::mutex m_;
  std::condition_variable cv_;
  std::deque<Event> queue_;

  // Session state, touched only on the worker thread.
  bool sessionDone_ = false;
  bool terminal_ = false;
  std::optional<ErrorInfo> sessionError_;
  bool liveSeen_ = false;      // lkc reported the media connection up
  bool emittedLive_ = false;   // we reported Live (a decoded frame arrived)
  bool noGuestReported_ = false;
  std::chrono::steady_clock::time_point noGuestDeadline_{};
  int videoDecodeFailures_ = 0;

  AVCodecContext* vdec_ = nullptr;
  AVCodecContext* adec_ = nullptr;
  SwsContext* sws_ = nullptr;
  SwrContext* swr_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  std::vector<uint8_t> bgra_;
};
}  // namespace bc
