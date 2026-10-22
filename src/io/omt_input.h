#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

namespace bc {
// OMT receive through omthx: plain OMT (VMX) and OMT HX (HEVC or H.264) with one receiver, the
// mode read from the source's own header. Pictures arrive as BGRA8; audio is 48 kHz and is taken
// from planar float. This is the low-bandwidth door for BreadCamera's own OMT HX outputs.
class OmtInput final : public Input {
 public:
  OmtInput(GpuDevice& gpu, TexturePool& pool, std::string source) : gpu_(gpu), pool_(pool), source_(std::move(source)) {}
  ~OmtInput() override { stop(); }
  void start(InputSink& sink) override;
  void stop() override;

 private:
  void run();
  GpuDevice& gpu_;
  TexturePool& pool_;
  std::string source_;
  InputSink* sink_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
};
}  // namespace bc