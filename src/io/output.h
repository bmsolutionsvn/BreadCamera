#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "core/rate.h"
#include "core/signal_state.h"
#include "gpu/converter.h"
#include "gpu/texture_pool.h"

namespace omthx {
struct CodedFrame;
enum class Mode;
}  // namespace omthx

namespace bc {
struct MediaPacket;
// One tick's frame for one camera, shared by all of its outputs (no copies, spec §3.2).
struct OutFrame {
  uint64_t tick = 0;
  Rate rate;
  TextureRef linear;                            // RGBA16F linear
  std::shared_ptr<const CpuImage> bgra;         // set when any output needsCpu()
  std::shared_ptr<const CpuImage> uyvy;         // set for an output that wantsUyvy, on ticks without alpha
  TextureRef rgba8;                             // R8G8B8A8_UNORM, true RGBA; set when any output wantsGpuRgba8()
  std::shared_ptr<const std::vector<float>> audio;  // interleaved, this tick's samples
  int sampleRate = 48000;
  int channels = 2;
  bool hasAlpha = false;
  // A coded frame on the camera's pass-through path: the picture is not decoded at all, and the
  // fields above carry no image. Audio, if any, goes with it at the coded frame's own timestamp.
  std::shared_ptr<const omthx::CodedFrame> coded;
  // An SRT pass-through packet: the input's coded packet and its parameters, re-muxed as they came.
  std::shared_ptr<const MediaPacket> packet;
};

struct OutputStatus {
  SignalState state = SignalState::Ready;
  std::optional<ErrorInfo> error;
  uint64_t dropped = 0;
  uint64_t delivered = 0;
  bool program = false;  // a downstream receiver has us on program
  bool preview = false;  // ... on preview
  std::string note;      // a human-readable qualification, e.g. "re-muxing the source"
};

class Output {
 public:
  virtual ~Output() = default;
  virtual bool needsCpu() const = 0;
  // True when this output can send the frame straight from the GPU: OutFrame::rgba8 instead of a
  // readback. needsCpu() must be false while it says true.
  virtual bool wantsGpuRgba8() const { return false; }
  // An output that takes the tick as UYVY instead of BGRA, when it fits the frame. Only
  // meaningful while needsCpu() is true.
  virtual bool wantsUyvy(uint32_t /*width*/, uint32_t /*height*/) const { return false; }
  // True when this output can send coded OMT frames of `mode` without decoding them (pass-through).
  virtual bool takesCodedOmt(omthx::Mode /*mode*/) const { return false; }
  // True when this output re-muxes packets of `video` without decoding them (pass-through).
  virtual bool takesPackets(AVCodecID /*video*/) const { return false; }
  // The keyframe request relayed from this output's receivers since the last call (any thread,
  // true once): the pass-through input asks its source for a keyframe when it is set.
  virtual bool takeKeyframeRequest() { return false; }
  virtual void start() = 0;
  virtual void submit(OutFrame frame) = 0;  // never blocks
  virtual void stop() = 0;
  virtual OutputStatus status() const = 0;
};
}  // namespace bc
