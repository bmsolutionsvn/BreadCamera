#pragma once
#include <cstdint>
#include <optional>
#include <vector>

#include "core/signal_state.h"
#include "gpu/converter.h"
#include "gpu/texture_pool.h"

namespace bc {
struct VideoFrameIn {
  TextureRef tex;  // NV12/P010 (planar SRVs) or BGRA8 (srv)
  PixelLayout layout = PixelLayout::NV12;
  bool fullRange = false;
  int64_t ptsUs = 0;
};

struct AudioBlock {
  int sampleRate = 48000;
  int channels = 2;
  std::vector<float> samples;  // interleaved
};

// Called from the input's own thread.
class InputSink {
 public:
  virtual ~InputSink() = default;
  virtual void onVideo(VideoFrameIn frame) = 0;
  virtual void onAudio(AudioBlock audio) = 0;
  virtual void onState(SignalState state, std::optional<ErrorInfo> error) = 0;
};

// spec §4.1: every adapter runs its own thread; start()/stop() never block for long.
class Input {
 public:
  virtual ~Input() = default;
  virtual void start(InputSink& sink) = 0;
  virtual void stop() = 0;
};
}  // namespace bc
