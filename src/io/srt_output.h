#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "io/queued_output.h"
#include "io/remux.h"
#include "io/srt_params.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct SwsContext;

namespace bc {
// H.264 (hardware) + AAC in MPEG-TS over SRT (spec §4.3). No alpha. In lean phase 5c it can
// re-mux the input's H.264/HEVC packets without encoding them.
class SrtOutput final : public QueuedOutput {
 public:
  explicit SrtOutput(SrtOutputOptions opts) : opts_(std::move(opts)) {}
  ~SrtOutput() override;
  bool needsCpu() const override { return true; }
  bool takesPackets(AVCodecID video) const override {
    return video == AV_CODEC_ID_H264 || video == AV_CODEC_ID_HEVC;
  }
  OutputStatus status() const override;

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "srt-out-failed"; }

 private:
  static int interrupt(void* self);
  void openEncoders(const OutFrame& frame);  // first frame, or again on a frame-size change
  void writePacket(AVCodecContext* c, AVStream* st);
  SrtOutputOptions opts_;
  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* venc_ = nullptr;
  AVCodecContext* aenc_ = nullptr;
  AVStream* vst_ = nullptr;
  AVStream* ast_ = nullptr;
  AVFrame* vframe_ = nullptr;
  AVFrame* aframe_ = nullptr;
  SwsContext* sws_ = nullptr;
  uint32_t swsW_ = 0, swsH_ = 0;
  int encW_ = 0, encH_ = 0;  // the open encoders' size; a differing frame re-opens them
  Rate encRate_{0, 1};       // the open encoder's rate; a rate change also re-opens them
  std::vector<float> audioFifo_;  // interleaved stereo
  int64_t vpts_ = 0, apts_ = 0;
  bool headerWritten_ = false;
  std::unique_ptr<RemuxWriter> remux_;  // present while packets are re-muxed
  std::atomic<bool> remuxing_{false};   // for status(): the picture path is not encoding
  bool dropUntilKey_ = false;           // a queue drop broke the GOP: resume at a keyframe
};
}  // namespace bc
