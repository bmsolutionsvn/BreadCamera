#pragma once
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

namespace bc {
struct CaptureDevice {
  std::string name;
  std::string id;  // MF symbolic link
};
std::vector<CaptureDevice> listCaptureDevices();

// USB/UVC (and most HDMI-to-USB) capture through Media Foundation (spec §4.2). Video only.
class MfCaptureInput final : public Input {
 public:
  MfCaptureInput(GpuDevice& gpu, TexturePool& pool, std::string device, int width = 0, int height = 0, int fps = 0)
      : gpu_(gpu), pool_(pool), device_(std::move(device)), width_(width), height_(height), fps_(fps) {}
  ~MfCaptureInput() override { stop(); }
  void start(InputSink& sink) override;
  void stop() override;

 private:
  void run();
  void runOnce();
  GpuDevice& gpu_;
  TexturePool& pool_;
  std::string device_;
  int width_, height_, fps_;
  InputSink* sink_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  std::vector<uint8_t> flip_;
  // The reader runs in MF_SOURCE_READER_ASYNC_CALLBACK mode: a synchronous ReadSample blocks
  // forever on a device that neither delivers a frame nor reports an error (a virtual or
  // stalled camera), so stop() could not bound its join(). The callback hands frames to the
  // run loop through a condition variable, so stop() is prompt. Owned; released in stop().
  struct MfReaderCallback;
  MfReaderCallback* cb_ = nullptr;
};
}  // namespace bc
