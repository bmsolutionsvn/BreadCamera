#include "engine/matte.h"

#if BC_WITH_MAXINE
#include "engine/maxine_matte.h"
#endif

namespace bc {
MatteBackendChoice makeMatteBackend(GpuDevice& gpu, TexturePool& pool, Converter& conv) {
#if BC_WITH_MAXINE
  if (gpu.isNvidia()) {
    try {
      return {std::make_unique<MaxineMatte>(gpu, pool, conv, MaxineMatte::defaultModelDir()), ""};
    } catch (const std::exception& e) {
      return {nullptr, std::string("the NVIDIA background removal could not start: ") + e.what()};
    }
  }
#endif
  (void)pool;
  (void)conv;
  return {nullptr, "background removal needs an NVIDIA RTX GPU with the NVIDIA Video Effects SDK; this PC uses " +
                       gpu.info().adapterName};
}
}  // namespace bc
