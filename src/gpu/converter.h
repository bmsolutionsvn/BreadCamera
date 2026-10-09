#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc {
enum class PixelLayout { NV12, P010, BGRA8 };

struct CpuImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> bgra;  // tightly packed, 4 bytes per pixel
};

// rgb' = rows * (y, cb, cr, 1) on unorm samples (spec §3.2).
struct YuvMatrix {
  std::array<std::array<float, 4>, 3> rows;
};
// BT.709 for height >= 720, else BT.601; limited range unless fullRange.
YuvMatrix yuvToRgbMatrix(uint32_t height, bool fullRange);

class Converter {
 public:
  static constexpr DXGI_FORMAT kLinearFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
  Converter(GpuDevice& gpu, TexturePool& pool);

  TextureRef toLinear(const GpuTexture& src, PixelLayout layout, bool fullRange);
  TextureRef toBgra8(const GpuTexture& linear, uint32_t outW, uint32_t outH);
  TextureRef fit(const GpuTexture& linear, uint32_t w, uint32_t h);  // letterbox to w x h, aspect kept
  CpuImage readback(const GpuTexture& rgba8);  // blocking Map
  TextureRef solid(uint32_t w, uint32_t h, std::array<float, 4> linearRgba);
  TextureRef uploadBgra8(const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride);

 private:
  struct alignas(16) Params {
    float row0[4];
    float row1[4];
    float row2[4];
    uint32_t outW, outH, srcW, srcH;
  };
  void dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srvs, UINT nSrv,
                ID3D11UnorderedAccessView* uav, const Params& p);

  GpuDevice& gpu_;
  TexturePool& pool_;
  Microsoft::WRL::ComPtr<ID3D11ComputeShader> yuvCs_, bgraCs_, outCs_, fitCs_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
};
}  // namespace bc
