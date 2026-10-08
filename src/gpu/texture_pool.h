#pragma once
#include <d3d11.h>
#include <wrl/client.h>

#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "gpu/device.h"

namespace bc {
struct GpuTexture {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;        // non-planar formats
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvLuma;    // NV12 / P010
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srvChroma;  // NV12 / P010
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 0;
  uint32_t height = 0;
};
using TextureRef = std::shared_ptr<GpuTexture>;

// Creates the views a texture's bind flags allow (plane SRVs for NV12/P010).
TextureRef wrapTexture(ID3D11Device* device, Microsoft::WRL::ComPtr<ID3D11Texture2D> tex);

// Per-camera frame textures are recycled, not re-created each tick (spec §3.2).
class TexturePool {
 public:
  explicit TexturePool(GpuDevice& gpu);
  // Throws std::invalid_argument for odd NV12/P010 sizes, std::runtime_error on failure.
  TextureRef acquire(uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind,
                     D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpuAccess = 0);
  size_t freeCount() const;

 private:
  struct Key {
    uint32_t w, h;
    DXGI_FORMAT fmt;
    UINT bind;
    D3D11_USAGE usage;
    UINT cpu;
    auto operator<=>(const Key&) const = default;
  };
  struct State {
    mutable std::mutex m;
    std::map<Key, std::vector<std::unique_ptr<GpuTexture>>> free;
  };
  GpuDevice& gpu_;
  std::shared_ptr<State> state_ = std::make_shared<State>();
};
}  // namespace bc
