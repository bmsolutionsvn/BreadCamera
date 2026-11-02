#include "gpu/nv12_upload.h"

#include <cstring>
#include <stdexcept>

namespace bc {
namespace {
// src rows are `rowBytes` apart, as OMT-HX documents for both planes.
void fillPlane(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const uint8_t* src, uint32_t rowBytes,
               uint32_t rows) {
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) throw std::runtime_error("Map failed");
  for (uint32_t y = 0; y < rows; ++y) {
    std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                src + static_cast<size_t>(y) * rowBytes, rowBytes);
  }
  ctx->Unmap(tex, 0);
}
}  // namespace

TextureRef uploadNv12Planes(GpuDevice& gpu, TexturePool& pool, const uint8_t* y, const uint8_t* uv,
                            uint32_t w, uint32_t h) {
  if ((w | h) & 1) throw std::invalid_argument("NV12 planes need even width and height");
  auto luma = pool.acquire(w, h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                           D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  auto chroma = pool.acquire(w / 2, h / 2, DXGI_FORMAT_R8G8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                             D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  auto* ctx = gpu.context();
  fillPlane(ctx, luma->tex.Get(), y, w, h);
  fillPlane(ctx, chroma->tex.Get(), uv, w, h / 2);  // both planes' rows are width bytes

  auto out = std::make_shared<GpuTexture>();
  out->width = w;
  out->height = h;
  out->srvLuma = luma->srv;
  out->srvChroma = chroma->srv;
  out->planeLuma = std::move(luma);
  out->planeChroma = std::move(chroma);
  return out;
}
}  // namespace bc
