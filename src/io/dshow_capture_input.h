#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

namespace bc {
// A DirectShow video capture device (CLSID_VideoInputDeviceCategory), addressed by moniker
// display name. The path MF cannot take: OBS Virtual Camera and other DirectShow-only virtual
// cameras. Video only, RGB32 -> BGRA8. Same contract as MfCaptureInput (spec §4.1/§4.2).
class DShowCaptureInput final : public Input {
 public:
  DShowCaptureInput(GpuDevice& gpu, TexturePool& pool, std::string displayName);
  ~DShowCaptureInput() override;
  void start(InputSink& sink) override;
  void stop() override;

 private:
  struct GrabberCB;
  void run();
  void runOnce();

  GpuDevice& gpu_;
  TexturePool& pool_;
  std::string displayName_;
  InputSink* sink_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::vector<uint8_t> flip_;
  GrabberCB* cb_ = nullptr;
  // Set once per runOnce from the connected media type.
  int width_ = 0;
  int height_ = 0;
  bool bottomUp_ = false;
};
}  // namespace bc
