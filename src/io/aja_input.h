#pragma once
#include <atomic>
#include <thread>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

namespace bc {
// AJA SDI capture through AutoCirculate (spec §4.2). EXPERIMENTAL — not verified on hardware.
class AjaInput final : public Input {
 public:
  AjaInput(GpuDevice& gpu, TexturePool& pool, int deviceIndex, int channel)
      : gpu_(gpu), pool_(pool), index_(deviceIndex), channel_(channel) {}
  ~AjaInput() override { stop(); }
  void start(InputSink& sink) override;
  void stop() override;

 private:
  void run();
  void runOnce();
  GpuDevice& gpu_;
  TexturePool& pool_;
  int index_, channel_;
  InputSink* sink_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};
}  // namespace bc