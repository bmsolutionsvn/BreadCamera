#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

namespace bc {
// Interleaves planar float audio into stereo: a mono source feeds both output channels from channel 0, and a
// source with more channels keeps the first two. False, `out` untouched, when the buffer is shorter than
// samples * channels.
bool planarToStereo(std::span<const float> planar, int samples, int channels, std::vector<float>& out);

// OMT receive through omthx: plain OMT (VMX) and OMT HX (HEVC or H.264) with one receiver, the
// mode read from the source's own header. Hx pictures arrive as full-range BT.709 NV12 and are
// uploaded on the GPU; plain OMT arrives as BGRA8. Audio is 48 kHz and is taken from planar float.
// This is the low-bandwidth door for BreadCamera's own OMT HX outputs.
class OmtInput final : public Input {
 public:
  OmtInput(GpuDevice& gpu, TexturePool& pool, std::string source) : gpu_(gpu), pool_(pool), source_(std::move(source)) {}
  ~OmtInput() override { stop(); }
  void start(InputSink& sink) override;
  void stop() override;
  // Pictures this input has decoded off the wire. A pass-through camera that needs no picture
  // leaves this flat: the coded frames are relayed without being decoded.
  uint64_t framesDecoded() const { return framesDecoded_.load(); }

 private:
  void run();
  GpuDevice& gpu_;
  TexturePool& pool_;
  std::string source_;
  InputSink* sink_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::atomic<uint64_t> framesDecoded_{0};
  std::thread thread_;
};
}  // namespace bc