#pragma once
#include <memory>
#include <string>
#include <vector>

#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc {
enum class MatteQuality { Quality, Performance };

struct MatteJob {
  std::string cameraId;
  TextureRef frame;  // linear RGBA16F, any size
  MatteQuality quality = MatteQuality::Quality;
  TextureRef alpha;  // out: R8 or R16F, any size
};

// spec §5.1: one batched call per tick for every camera that wants a matte.
class MatteBackend {
 public:
  virtual ~MatteBackend() = default;
  virtual std::string name() const = 0;
  virtual void process(std::vector<MatteJob*>& jobs) = 0;  // throws → the whole batch is bypassed
  virtual void forget(const std::string& cameraId) = 0;    // drop per-camera temporal state
  // Last per-camera inference time in ms, for /engine/matteMs; 0 when the backend reports none.
  virtual double lastInferenceMs() const { return 0; }
};

struct MatteBackendChoice {
  std::unique_ptr<MatteBackend> backend;
  std::string unavailableReason;  // set when backend is null
};
MatteBackendChoice makeMatteBackend(GpuDevice& gpu, TexturePool& pool, Converter& conv);
}  // namespace bc
