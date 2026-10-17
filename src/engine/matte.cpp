#include "engine/matte.h"

#if BC_WITH_MAXINE
#include "engine/maxine_matte.h"
#endif
#if BC_WITH_ONNX
#include <windows.h>

#include <filesystem>

#include "engine/onnx_matte.h"
#endif

namespace bc {
MatteBackendChoice makeMatteBackend(GpuDevice& gpu, TexturePool& pool, Converter& conv) {
  std::string maxineError;
#if BC_WITH_MAXINE
  if (gpu.isNvidia()) {
    try {
      return {std::make_unique<MaxineMatte>(gpu, pool, conv, MaxineMatte::defaultModelDir()), ""};
    } catch (const std::exception& e) {
      // The SDK can be present but fail to start (no VFX runtime, missing models). Do not give
      // up: fall through to ONNX, which the plan also runs on NVIDIA when Maxine is missing.
      maxineError = std::string("the NVIDIA background removal could not start: ") + e.what();
    }
  }
#endif
  // M6 Task 2: MODNet through ONNX Runtime + DirectML is the fallback when Maxine is absent
  // (or the GPU is not NVIDIA). It runs on this machine's adapter and names its own backend.
#if BC_WITH_ONNX
  {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const auto model = std::filesystem::path(exe).parent_path() / "models" / "modnet.onnx";
    try {
      return {std::make_unique<OnnxMatte>(gpu, pool, conv, model), ""};
    } catch (const std::exception& e) {
      std::string reason = std::string("background removal is unavailable: ") + e.what();
      if (!maxineError.empty()) reason = maxineError + "; " + reason;
      return {nullptr, std::move(reason)};
    }
  }
#else
  (void)pool;
  (void)conv;
  std::string reason = "background removal needs an NVIDIA RTX GPU with the NVIDIA Video Effects SDK; this PC uses " +
                       gpu.info().adapterName;
  if (!maxineError.empty()) reason = maxineError + "; " + reason;
  return {nullptr, std::move(reason)};
#endif
}
}  // namespace bc
