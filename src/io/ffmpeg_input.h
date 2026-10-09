#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"
#include "io/srt_params.h"

struct AVFrame;
struct AVStream;
struct AVBufferRef;
struct SwsContext;
struct SwrContext;

namespace bc {
// Any FFmpeg URL (SRT in the product, files in tests). Decodes on the shared D3D11 device
// when it can (d3d11va), else in software to BGRA (spec §3.2). Reconnects with backoff.
class FfmpegInput final : public Input {
 public:
  FfmpegInput(GpuDevice& gpu, TexturePool& pool, FfmpegInputOptions opts);
  ~FfmpegInput() override;
  void start(InputSink& sink) override;
  void stop() override;

 private:
  void run();
  void runOnce();
  void emitVideo(AVFrame* f, AVStream* st);
  void emitAudio(AVFrame* f);
  void pace(int64_t ptsUs);
  AVBufferRef* makeHwDevice();
  std::string safeUrl() const;
  static int interruptCb(void* self);

  GpuDevice& gpu_;
  TexturePool& pool_;
  FfmpegInputOptions opts_;
  InputSink* sink_ = nullptr;
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<int64_t> deadlineMs_{INT64_MAX};
  std::mutex waitMutex_;
  std::condition_variable waitCv_;
  bool wentLive_ = false;
  SwsContext* sws_ = nullptr;
  SwrContext* swr_ = nullptr;
  std::vector<uint8_t> swBuf_;
  int64_t paceFirstUs_ = INT64_MIN;
  std::chrono::steady_clock::time_point paceStart_{};
};
}  // namespace bc
