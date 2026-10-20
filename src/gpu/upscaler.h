#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc {
// Upscale on any GPU: AMD FidelityFX Super Resolution 1 (EASU then RCAS, MIT) over the pipeline's
// linear RGBA16F frames (spec D6). EASU and RCAS run on gamma-encoded values — the pipeline's
// 1/2.4 encode on load and 2.4 decode on store — as FSR expects.
class Upscaler {
 public:
  Upscaler(GpuDevice& gpu, TexturePool& pool);
  // linear RGBA16F in and out, at exactly w x h.
  TextureRef apply(const GpuTexture& linear, uint32_t w, uint32_t h);

 private:
  struct EasuParams {
    uint32_t con0[4], con1[4], con2[4], con3[4];
    uint32_t outW, outH;
  };
  struct RcasParams {
    uint32_t con[4];
    uint32_t outW, outH;
  };
  // The GPU lock must already be held (Vulkan-free D3D11 state is not thread-safe).
  void dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* srv, ID3D11UnorderedAccessView* uav,
                const void* params, size_t bytes, uint32_t w, uint32_t h);

  GpuDevice& gpu_;
  TexturePool& pool_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> easuCs_, rcasCs_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
};
}  // namespace bc