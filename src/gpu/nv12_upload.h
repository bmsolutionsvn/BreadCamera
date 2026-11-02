#pragma once
#include <cstdint>

#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc {
// Fills an R8 (w x h) and an R8G8 (w/2 x h/2) pooled texture from tightly packed planes (stride =
// width) and wraps the pair as one NV12-shaped texture whose srvLuma/srvChroma are the planes' SRVs.
// The planes are DYNAMIC and written with Map(WRITE_DISCARD): UpdateSubresource leaks in this
// engine, which never presents. Throws std::invalid_argument for odd sizes. The caller holds
// GpuDevice::lock().
TextureRef uploadNv12Planes(GpuDevice& gpu, TexturePool& pool, const uint8_t* y, const uint8_t* uv,
                            uint32_t w, uint32_t h);
}  // namespace bc
