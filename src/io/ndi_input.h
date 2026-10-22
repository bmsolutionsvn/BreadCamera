#pragma once
#include <atomic>
#include <string>
#include <thread>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

namespace bc {
// NDI receive → BGRA8 frames + 48 kHz stereo audio (spec §4.2).
class NdiInput final : public Input {
 public:
  NdiInput(GpuDevice& gpu, TexturePool& pool, std::string source, bool hx = false)
      : gpu_(gpu), pool_(pool), source_(std::move(source)), hx_(hx) {}
  ~NdiInput() override { stop(); }
  void start(InputSink& sink) override;
  void stop() override;

 private:
  void run();
  GpuDevice& gpu_;
  TexturePool& pool_;
  std::string source_;
  bool hx_ = false;  // NDI|HX: ask the source for its lowest-bandwidth stream
  InputSink* sink_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};
}  // namespace bc
