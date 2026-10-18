#pragma once
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/upscaler.h"

namespace bc {
// The GPU helpers every camera stage uses, passed as one bundle.
struct GpuTools {
  Converter& conv;
  Compositor& comp;
  ColorGrader& grader;
  // The FSR 1 upscaler (spec D6). Null bypasses Upscale to a plain fit.
  Upscaler* up = nullptr;
  // The shared device, for per-stage GPU timing (spec D14). Null keeps the CPU timing.
  GpuDevice* gpu = nullptr;
};
}  // namespace bc
