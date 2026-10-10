#pragma once
#include <array>
#include <string_view>

#include <nlohmann/json.hpp>

#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc {
struct MatteRefine {
  float choke = 0;
  float feather = 1;
  float threshold = 0.5f;
};
MatteRefine refineFrom(const nlohmann::json& matteParams);

enum class BackgroundMode { None, Keyed, Blur, Replace };
struct BackgroundSettings {
  BackgroundMode mode = BackgroundMode::None;
  float blur = 12;                         // px at 1080p
  std::array<float, 3> colorLinear{0, 0, 0};
  TextureRef image;                        // linear RGBA16F, or null = use colorLinear
};
BackgroundSettings backgroundFrom(const nlohmann::json& backgroundParams);
std::array<float, 3> hexToLinear(std::string_view hex);

// spec §5.1 matte controls + §5.2 background modes, on the shared device.
class Compositor {
 public:
  Compositor(GpuDevice& gpu, TexturePool& pool);
  TextureRef refine(const GpuTexture& rawAlpha, uint32_t w, uint32_t h, const MatteRefine& r);
  TextureRef apply(const TextureRef& frame, const TextureRef& alpha, const BackgroundSettings& s);

 private:
  struct alignas(16) Params {
    float a[4];
    float b[4];
    uint32_t outW, outH, srcW, srcH;
  };
  void run(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs,
           ID3D11UnorderedAccessView* uav, const Params& p);
  TextureRef alphaTex(uint32_t w, uint32_t h);
  GpuDevice& gpu_;
  TexturePool& pool_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> threshold_, morph_, alphaBlur_, down_, bgBlur_, composite_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
};
}  // namespace bc
