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
#include "engine/preview_encoder.h"
#include "engine/preview_hub.h"
#include "gpu/converter.h"

namespace bc {
// Runs on the tick thread via Engine::setFrameTap. Encodes only what someone watches.
// ponytail: encodes on the tick thread (~1-2 ms per small preview); move to a worker if
// the tick budget gets tight with many large previews.
class PreviewService {
 public:
  using SendFn = std::function<void(const std::string& client, std::vector<uint8_t> message)>;
  PreviewService(Converter& conv, StateStore& state, PreviewHub& hub, SendFn send);
  void onTick(uint64_t tick, const std::vector<std::pair<std::string, TextureRef>>& frames);
  void requestKeyframe(const std::string& cameraId, PreviewSize size);

 private:
  Converter& conv_;
  StateStore& state_;
  PreviewHub& hub_;
  SendFn send_;
  bool broken_ = false;
  std::map<PreviewHub::Key, std::unique_ptr<PreviewEncoder>> encoders_;
  std::mutex keyMutex_;
  std::set<PreviewHub::Key> keyRequests_;
};
}  // namespace bc
