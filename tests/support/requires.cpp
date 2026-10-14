#include "support/requires.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>

#include "engine/chart_detector.h"
#include "engine/preview_encoder.h"
#include "gpu/device.h"
#include "io/aja.h"
#include "io/decklink.h"
#include "io/mf_capture_input.h"
#include "io/vcam_output.h"

namespace bc::test {
namespace {
bool envSet(const char* name) {
  char* buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) return false;
  const bool set = buf[0] != '\0';
  std::free(buf);
  return set;
}
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
    case Req::CaptureDevice:
      reason = "no USB/UVC capture device on this PC";
      return bc::listCaptureDevices().empty();
    case Req::Omt:
      reason = "OMT-HX checkout not built (BC_WITH_OMT=OFF)";
      return BC_WITH_OMT == 0;
    case Req::DeckLink:
      reason = "no DeckLink SDK or card";
#if BC_WITH_DECKLINK
      return bc::listDeckLinkDevices().empty();
#else
      return true;
#endif
    case Req::Aja:
      reason = "no AJA SDK or card";
#if BC_WITH_AJA
      return bc::listAjaDevices().empty();
#else
      return true;
#endif
    case Req::VcamInstalled:
      reason = "virtual camera not registered (run regsvr32 breadcamera-vcam.dll as admin on Windows 11)";
#if BC_WITH_VCAM
      return !bc::VirtualCameraOutput::installed();
#else
      return true;
#endif
    case Req::Onnx:
      reason = "ONNX Runtime/model not fetched (scripts/fetch-sdks.ps1)";
#if BC_WITH_ONNX
      {
        wchar_t buf[MAX_PATH];
        if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0) return true;
        return !std::filesystem::exists(std::filesystem::path(buf).parent_path() / "models" /
                                        "modnet.onnx");
      }
#else
      return true;
#endif
    case Req::LiveCall:
      reason = "no LiveKit server (set BC_LIVECALL_URL and BC_LIVECALL_TOKEN)";
#if BC_WITH_LIVECALL
      return !envSet("BC_LIVECALL_URL") || !envSet("BC_LIVECALL_TOKEN");
#else
      return true;
#endif
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
