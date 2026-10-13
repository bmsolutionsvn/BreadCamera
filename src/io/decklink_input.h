#pragma once
#include <atomic>
#include <mutex>
#include <thread>

#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "io/input.h"

struct IDeckLink;
struct IDeckLinkInput;

namespace bc {
// DeckLink SDI/HDMI capture with input format detection (spec §4.2).
class DeckLinkInput final : public Input {
 public:
  DeckLinkInput(GpuDevice& gpu, TexturePool& pool, int deviceIndex) : gpu_(gpu), pool_(pool), index_(deviceIndex) {}
  ~DeckLinkInput() override { stop(); }
  void start(InputSink& sink) override;
  void stop() override;

  // Called by the SDK callback object.
  void onFormatChanged(uint32_t mode, bool rgb);
  void onFrame(const uint8_t* bytes, int width, int height, int rowBytes, bool noSignal, const int32_t* audio, int audioFrames);

 private:
  void run();
  GpuDevice& gpu_;
  TexturePool& pool_;
  int index_;
  InputSink* sink_ = nullptr;
  std::atomic<bool> stopping_{false};
  std::thread thread_;
  IDeckLink* device_ = nullptr;
  IDeckLinkInput* input_ = nullptr;
  std::mutex m_;
  bool live_ = false;
};
}  // namespace bc