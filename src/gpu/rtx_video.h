#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc {
// NVIDIA RTX Video SDK (NGX VSR, spec D4): Clean up at 1× and AI Upscale, on D3D11 textures with
// no CPU round-trip. RTX 20+ only, and only when the SDK is present (BC_WITH_RTXVIDEO); a missing
// SDK never fails the build (spec §7.2). This wrapper is written from the SDK headers and
// programming guide, not from the SDK's samples (spec "RTX Video SDK licence obligations").
class RtxVideo {
 public:
  // Null and a plain reason when this PC cannot run it: no NVIDIA card (or WARP), no SDK in this
  // build, or NGX refusing to start (a driver older than R550). `whyNot` is one of the exact
  // strings in the .cpp and is what Add effect shows on the Clean up row (spec D7).
  static std::unique_ptr<RtxVideo> create(GpuDevice& gpu, TexturePool& pool, Converter& conv, std::string& whyNot);
  ~RtxVideo();
  RtxVideo(const RtxVideo&) = delete;
  RtxVideo& operator=(const RtxVideo&) = delete;
  // linear RGBA16F in and out, at exactly w × h. w × h equal to the input runs VSR at 1×
  // (Clean up); larger runs super resolution (Upscale). Throws std::runtime_error on NGX failure.
  TextureRef run(const GpuTexture& linear, uint32_t w, uint32_t h);

 private:
  RtxVideo(GpuDevice& gpu, TexturePool& pool, Converter& conv);
  struct Impl;
  std::unique_ptr<Impl> impl_;
  GpuDevice& gpu_;
  TexturePool& pool_;
  Converter& conv_;
};
}  // namespace bc