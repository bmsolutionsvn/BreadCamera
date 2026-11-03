#include "gpu/upload.h"

#include <cstring>
#include <stdexcept>

namespace bc {
void writeTexture(GpuDevice& gpu, ID3D11Texture2D* tex, const uint8_t* src, uint32_t rowBytes,
                  uint32_t rows) {
  auto* ctx = gpu.context();
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) throw std::runtime_error("Map failed");
  for (uint32_t y = 0; y < rows; ++y) {
    std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                src + static_cast<size_t>(y) * rowBytes, rowBytes);
  }
  ctx->Unmap(tex, 0);
}

TextureRef uploadNv12Planes(GpuDevice& gpu, TexturePool& pool, const uint8_t* y, const uint8_t* uv,
                            uint32_t w, uint32_t h) {
  if ((w | h) & 1) throw std::invalid_argument("NV12 planes need even width and height");
  auto luma = pool.acquire(w, h, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                           D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  auto chroma = pool.acquire(w / 2, h / 2, DXGI_FORMAT_R8G8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                             D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  writeTexture(gpu, luma->tex.Get(), y, w, h);
  writeTexture(gpu, chroma->tex.Get(), uv, w, h / 2);  // both planes' rows are width bytes

  auto out = std::make_shared<GpuTexture>();
  out->width = w;
  out->height = h;
  out->srvLuma = luma->srv;
  out->srvChroma = chroma->srv;
  out->planeLuma = std::move(luma);
  out->planeChroma = std::move(chroma);
  return out;
}

TextureRef uploadUyvy(GpuDevice& gpu, TexturePool& pool, const uint8_t* uyvy, uint32_t rowBytes,
                      uint32_t w, uint32_t h) {
  if (w % 2 != 0) throw std::invalid_argument("UYVY needs an even width");
  auto packed = pool.acquire(w / 2, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                             D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  writeTexture(gpu, packed->tex.Get(), uyvy, rowBytes, h);
  auto out = std::make_shared<GpuTexture>();
  out->width = w;
  out->height = h;
  out->format = packed->format;
  out->srv = packed->srv;
  out->planeLuma = std::move(packed);  // keeps the packed texture out of the pool while in use
  return out;
}

TextureRef uploadBgra(GpuDevice& gpu, TexturePool& pool, const uint8_t* bgra, uint32_t rowBytes,
                      uint32_t w, uint32_t h) {
  auto out = pool.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                          D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
  writeTexture(gpu, out->tex.Get(), bgra, rowBytes, h);
  return out;
}
}  // namespace bc