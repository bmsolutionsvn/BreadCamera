#pragma once
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/rtx_video.h"
#include "gpu/upscaler.h"

namespace bc {
class FaceDetector;

// The GPU helpers every camera stage uses, passed as one bundle.
struct GpuTools {
  Converter& conv;
  Compositor& comp;
  ColorGrader& grader;
  // The FSR 1 upscaler (spec D6). Null bypasses Enhance's resize to a plain fit.
  Upscaler* up = nullptr;
  // The NVIDIA RTX Video SDK path (spec D4): Enhance's one VSR pass. Null passes a same-size frame
  // through unchanged and falls a resize back to FSR 1.
  RtxVideo* rtx = nullptr;
  // The shared device, for per-stage GPU timing (spec D14). Null keeps the CPU timing.
  GpuDevice* gpu = nullptr;
  // AI face detection for Auto frame. Null makes the stage report why it cannot run.
  FaceDetector* faces = nullptr;
};
}  // namespace bc
