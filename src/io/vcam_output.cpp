#include "io/vcam_output.h"

#include <mfapi.h>
#include <mfvirtualcamera.h>

extern "C" {
#include <libswscale/swscale.h>
}

#include <stdexcept>

#include "core/utf8.h"

namespace bc {
bool VirtualCameraOutput::installed() {
  OSVERSIONINFOEXW v{sizeof(v)};
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
  auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  if (!fn || fn(&v) != 0 || v.dwBuildNumber < 22000) return false;
  HKEY key = nullptr;
  const std::wstring path = std::wstring(L"Software\\Classes\\CLSID\\") + vcam::kClsids[0] + L"\\InprocServer32";
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
  RegCloseKey(key);
  return true;
}

void VirtualCameraOutput::open() {
  if (index_ < 1 || index_ > 4) throw std::runtime_error("virtual camera index must be 1 to 4");
  if (!installed()) throw std::runtime_error("the BreadCamera virtual camera is not installed (Windows 11 and the installer are needed)");
  MFStartup(MF_VERSION);
  const std::wstring friendly = toWide(name_);
  const HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_Session,
                                           MFVirtualCameraAccess_CurrentUser, friendly.c_str(), vcam::kClsids[index_ - 1],
                                           nullptr, 0, &cam_);
  if (FAILED(hr) || FAILED(cam_->Start(nullptr))) throw std::runtime_error("Windows refused to start the virtual camera");
}

void VirtualCameraOutput::deliver(const OutFrame& f) {
  if (!f.bgra || f.bgra->bgra.empty()) return;
  if (++frames_ % 2) return;  // ~30 fps from a 60 fps tick
  if (!writer_.isOpen() && (frames_ % 30 == 0 || frames_ == 2)) writer_.open(index_);
  if (!writer_.isOpen()) return;  // no app has the camera open: nothing to do
  const uint8_t* src = f.bgra->bgra.data();
  if (f.bgra->width != vcam::kWidth || f.bgra->height != vcam::kHeight) {
    scaled_.resize(vcam::kFrameBytes);
    sws_ = sws_getCachedContext(sws_, static_cast<int>(f.bgra->width), static_cast<int>(f.bgra->height), AV_PIX_FMT_BGRA,
                                vcam::kWidth, vcam::kHeight, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) throw std::runtime_error("cannot create a pixel converter for the virtual camera");
    const uint8_t* s[1] = {src};
    const int ss[1] = {static_cast<int>(f.bgra->width * 4)};
    uint8_t* d[1] = {scaled_.data()};
    const int ds[1] = {static_cast<int>(vcam::kWidth * 4)};
    sws_scale(sws_, s, ss, 0, static_cast<int>(f.bgra->height), d, ds);
    src = scaled_.data();
  }
  writer_.write(src);
}

void VirtualCameraOutput::close() noexcept {
  writer_.close();
  if (cam_) {
    cam_->Shutdown();
    cam_->Release();
    cam_ = nullptr;
  }
  sws_freeContext(sws_);
  sws_ = nullptr;
}
}  // namespace bc