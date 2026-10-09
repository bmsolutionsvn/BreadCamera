#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "core/rate.h"
#include "core/signal_state.h"
#include "gpu/converter.h"
#include "gpu/texture_pool.h"

namespace bc {
// One tick's frame for one camera, shared by all of its outputs (no copies, spec §3.2).
struct OutFrame {
  uint64_t tick = 0;
  Rate rate;
  TextureRef linear;                            // RGBA16F linear
  std::shared_ptr<const CpuImage> bgra;         // set when any output needsCpu()
  std::shared_ptr<const std::vector<float>> audio;  // interleaved, this tick's samples
  int sampleRate = 48000;
  int channels = 2;
  bool hasAlpha = false;
};

struct OutputStatus {
  SignalState state = SignalState::Ready;
  std::optional<ErrorInfo> error;
  uint64_t dropped = 0;
  uint64_t delivered = 0;
};

class Output {
 public:
  virtual ~Output() = default;
  virtual bool needsCpu() const = 0;
  virtual void start() = 0;
  virtual void submit(OutFrame frame) = 0;  // never blocks
  virtual void stop() = 0;
  virtual OutputStatus status() const = 0;
};
}  // namespace bc
