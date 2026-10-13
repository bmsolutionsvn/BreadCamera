#pragma once
#include <memory>

#include "core/show.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"
#include "io/output.h"

namespace bc {
// Builds adapters from show config. Both methods throw std::invalid_argument with a
// user-readable message for unknown kinds or bad params.
class AdapterFactory {
 public:
  virtual ~AdapterFactory() = default;
  // The SDI and virtual-camera outputs need the show rate to pick their fixed format. The
  // engine sets it in apply() before cameras are (re)built.
  virtual void setShowRate(Rate) {}
  virtual std::unique_ptr<Input> makeInput(const InputConfig& cfg) = 0;
  virtual std::unique_ptr<Output> makeOutput(const OutputConfig& cfg, const CameraConfig& cam) = 0;
};

class DefaultAdapterFactory final : public AdapterFactory {
 public:
  DefaultAdapterFactory(GpuDevice& gpu, TexturePool& pool) : gpu_(gpu), pool_(pool) {}
  void setShowRate(Rate r) override { rate_ = r; }
  std::unique_ptr<Input> makeInput(const InputConfig& cfg) override;
  std::unique_ptr<Output> makeOutput(const OutputConfig& cfg, const CameraConfig& cam) override;

 private:
  GpuDevice& gpu_;
  TexturePool& pool_;
  Rate rate_{60, 1};
};
}  // namespace bc
