#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/state_store.h"
#include "engine/engine.h"
#include "engine/preview_encoder.h"
#include "engine/preview_hub.h"

namespace bc {
// Consumes the CpuImage the frame tap already carries: no converter, no readback, no per-wire-size
// downscale. The tick thread only drops the newest image into a per-camera one-slot queue (D4); a
// worker thread H.264-encodes it, so the tick never blocks on the encoder. The number of live
// encoders is capped (D9): past the cap a tile is skipped and a state warning is set, instead of one
// open failure disabling every preview.
class PreviewService {
 public:
  using SendFn = std::function<void(const std::string& client, std::vector<uint8_t> message)>;
  // Consumer GPUs limit concurrent encoder sessions, and a full-size preview per visible tile can
  // exhaust them; beyond this many watched cameras the excess tiles are skipped (D9).
  static constexpr size_t kMaxPreviewEncoders = 4;
  // A preview never encodes larger than this (D8): the shared image is the camera's input size, so a
  // 4K camera would otherwise H.264-encode 4K per visible tile. A larger frame is downscaled to fit
  // this box (aspect kept) on the encoder worker, never on the tick.
  static constexpr int kPreviewCapWidth = 1280;
  static constexpr int kPreviewCapHeight = 720;
  PreviewService(StateStore& state, PreviewHub& hub, SendFn send);
  ~PreviewService();
  PreviewService(const PreviewService&) = delete;
  PreviewService& operator=(const PreviewService&) = delete;
  // Runs on the tick thread and never blocks on the encoder.
  void onTick(uint64_t tick, const std::vector<FrameTap>& frames);
  void requestKeyframe(const std::string& cameraId, PreviewSize size);
  // Waits until every worker has drained the frames queued so far (tests and shutdown).
  void drain();

 private:
  struct EncoderSlot;
  void run(EncoderSlot* slot);
  void reap();

  StateStore& state_;
  PreviewHub& hub_;
  SendFn send_;
  // One H.264 encoder per watched camera, always opened at the frame size (D7 ignores the wire size).
  std::map<std::string, std::unique_ptr<EncoderSlot>> slots_;
  // Cameras whose encoder failed to open: their tile is skipped and not retried until unwatched.
  std::set<std::string> dead_;
  // Stopped workers awaiting join; reaped once their thread has finished, never on the hot path.
  std::vector<std::unique_ptr<EncoderSlot>> retiring_;
  std::mutex keyMutex_;
  std::set<PreviewHub::Key> keyRequests_;
};
}  // namespace bc
