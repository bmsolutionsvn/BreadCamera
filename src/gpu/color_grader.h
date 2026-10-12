#pragma once
#include <memory>

#include <nlohmann/json.hpp>

#include "core/lut.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc {
struct GpuLut {
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  int size = 0;
};

struct ColorGrade {
  float exposure = 0, temperature = 0, tint = 0, lift = 0, gamma = 1, gain = 1, saturation = 1;
  std::shared_ptr<const GpuLut> lut;
};
ColorGrade gradeFrom(const nlohmann::json& colorParams);

// The colour stage (spec §5.3): matched LUT, then trims.
class ColorGrader {
 public:
  ColorGrader(GpuDevice& gpu, TexturePool& pool);
  std::shared_ptr<const GpuLut> upload(const Lut3D& lut);
  TextureRef apply(const TextureRef& frame, const ColorGrade& g);
  static bool isIdentity(const ColorGrade& g);

 private:
  struct alignas(16) Params {
    float a[4];
    float b[4];
    float c[4];
    uint32_t outW, outH, srcW, srcH;
  };
  GpuDevice& gpu_;
  TexturePool& pool_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
  std::shared_ptr<const GpuLut> identity_;  // bound when no LUT is set (a slot must hold a valid view)
};
}  // namespace bc
