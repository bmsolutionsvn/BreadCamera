#include "engine/matte.h"

namespace bc {
MatteBackendChoice makeMatteBackend(GpuDevice& gpu, TexturePool& pool, Converter& conv) {
  (void)pool;
  (void)conv;
  // Task 5 adds the Maxine branch here.
  return {nullptr, "background removal needs an NVIDIA RTX GPU with the NVIDIA Video Effects SDK; this PC uses " +
                       gpu.info().adapterName};
}
}  // namespace bc
