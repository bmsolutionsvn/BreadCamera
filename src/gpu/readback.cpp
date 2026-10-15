#include "gpu/readback.h"

#include <cassert>

namespace bc {
void ReadbackSlot::begin(Converter& conv, const GpuTexture& rgba8) {
  assert(!pending_);  // a re-begin discards an unfinished frame; finish it first
  if (!staging_ || staging_->width != rgba8.width || staging_->height != rgba8.height ||
      staging_->format != rgba8.format) {
    staging_.reset();
    staging_ = conv.pool().acquire(rgba8.width, rgba8.height, rgba8.format, 0,
                                   D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
  }
  conv.beginReadback(staging_, rgba8);
  pending_ = true;
}

bool ReadbackSlot::tryFinish(Converter& conv, CpuImage& out) {
  if (!pending_) return false;
  if (!conv.tryFinishReadback(staging_, out)) return false;
  pending_ = false;
  return true;
}
}  // namespace bc
