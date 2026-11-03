#pragma once
#include <cstdint>

#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc {
// The one way to write a texture per frame: a DYNAMIC texture filled with Map(WRITE_DISCARD).
// UpdateSubresource leaks in this engine, which never presents. src rows are rowBytes apart.
// The caller holds GpuDevice::lock().
void writeTexture(GpuDevice& gpu, ID3D11Texture2D* dynamicTex, const uint8_t* src, uint32_t rowBytes,
                  uint32_t rows);

// Fills an R8 (w x h) and an R8G8 (w/2 x h/2) pooled texture from planes (chroma stride = width,
// luma rows are `yRowBytes` apart; 0 means tightly packed at width) and wraps the pair as one
// NV12-shaped texture whose srvLuma/srvChroma are the planes' SRVs.
// Throws std::invalid_argument for odd sizes.
TextureRef uploadNv12Planes(GpuDevice& gpu, TexturePool& pool, const uint8_t* y, const uint8_t* uv,
                            uint32_t w, uint32_t h, uint32_t yRowBytes = 0);

// UYVY (2 bytes a pixel) into an R8G8B8A8_UNORM w/2 x h texture, each texel U, Y0, V, Y1. The
// returned picture is w x h with the packed texture as its srv. Throws std::invalid_argument for
// an odd width.
TextureRef uploadUyvy(GpuDevice& gpu, TexturePool& pool, const uint8_t* uyvy, uint32_t rowBytes,
                      uint32_t w, uint32_t h);

// BGRA into a B8G8R8A8_UNORM w x h texture.
TextureRef uploadBgra(GpuDevice& gpu, TexturePool& pool, const uint8_t* bgra, uint32_t rowBytes,
                      uint32_t w, uint32_t h);
}  // namespace bc