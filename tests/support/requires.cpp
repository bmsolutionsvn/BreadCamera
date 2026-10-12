#include "support/requires.h"

#include <cstdio>
#include <mutex>
#include <set>

#include "engine/chart_detector.h"
#include "engine/preview_encoder.h"
#include "gpu/device.h"

namespace bc::test {
namespace {
bool check(Req r, const char*& reason) {
  switch (r) {
    case Req::Ndi:
      reason = "NDI SDK not built (BC_WITH_NDI=OFF)";
      return BC_WITH_NDI == 0;
    case Req::HardwareGpu: {
      reason = "needs a hardware GPU (WARP or BC_FORCE_WARP=1 in use)";
      try {
        auto dev = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
        return dev->info().warp;
      } catch (...) {
        return true;
      }
    }
    case Req::H264Encoder: {
      reason = "no H.264 encoder opens on this machine (h264_nvenc/amf/qsv/mf)";
      try {
        bc::PreviewEncoder probe(512, 288, 30);
        return false;
      } catch (...) {
        return true;
      }
    }
    case Req::Maxine: {
      reason = "needs BC_WITH_MAXINE (NVIDIA Video Effects SDK) and an NVIDIA GPU";
      if (BC_WITH_MAXINE == 0) return true;
      try {
        return !bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware)->isNvidia();
      } catch (...) {
        return true;
      }
    }
    case Req::ChartDetect:
      reason = "chart detection is not in this build (OpenCV mcc missing)";
      return !bc::chartDetectionAvailable();
  }
  return true;
}
}  // namespace

bool missing(Req r) {
  static std::mutex m;
  static std::set<int> printed;
  const char* reason = "";
  bool miss = check(r, reason);
  std::lock_guard lock(m);
  if (miss && printed.insert(static_cast<int>(r)).second) {
    std::fprintf(stderr, "[skip] requirement %d missing: %s\n", static_cast<int>(r), reason);
  }
  return miss;
}
}  // namespace bc::test
