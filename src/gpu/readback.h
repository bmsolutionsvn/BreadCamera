#pragma once
#include "gpu/converter.h"
#include "gpu/texture_pool.h"

namespace bc {
// A non-blocking frame readback: begin() issues the cheap GPU-side staging copy; tryFinish()
// maps without waiting, so a tick never stalls on the GPU. Contrast Converter::readback, the
// blocking one-shot kept for callers that genuinely need this tick's pixels.
class ReadbackSlot {
 public:
  bool pending() const { return pending_; }

  // Issues the staging CopyResource for rgba8, reusing the slot's staging texture when the
  // size and format match. Does not block.
  void begin(Converter& conv, const GpuTexture& rgba8);

  // If the GPU has finished the copy, fills out, clears pending and returns true. If it is
  // still drawing, returns false and changes nothing — never blocks.
  bool tryFinish(Converter& conv, CpuImage& out);

 private:
  TextureRef staging_;
  bool pending_ = false;
};
}  // namespace bc