#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/state_store.h"
#include "engine/engine.h"
#include "engine/preview_encoder.h"
#include "engine/preview_hub.h"

namespace bc {
// Runs on the tick thread via Engine::setFrameTap. Encodes only what someone watches, using the
// CpuImage the tap already carries (no converter, no readback, no per-wire-size downscale).
// ponytail: encodes on the tick thread (~1-2 ms per small preview); move to a worker if
// the tick budget gets tight with many large previews.
class PreviewService {
 public:
  using SendFn = std::function<void(const std::string& client, std::vector<uint8_t> message)>;
  PreviewService(StateStore& state, PreviewHub& hub, SendFn send);
  void onTick(uint64_t tick, const std::vector<FrameTap>& frames);
  void requestKeyframe(const std::string& cameraId, PreviewSize size);

 private:
  // One H.264 encoder per watched (camera, wire size) pair, always opened at the frame size.
  // w/h record that size so a show-resolution change reopens the encoder (D7 ignores the wire
  // size for dimensions).
  struct EncoderSlot {
    std::unique_ptr<PreviewEncoder> enc;
    uint32_t w = 0, h = 0;
  };
  StateStore& state_;
  PreviewHub& hub_;
  SendFn send_;
  bool broken_ = false;
  std::map<PreviewHub::Key, EncoderSlot> encoders_;
  std::mutex keyMutex_;
  std::set<PreviewHub::Key> keyRequests_;
};
}  // namespace bc
