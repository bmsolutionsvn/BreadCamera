#include "engine/preview_service.h"

#include <chrono>

namespace bc {
PreviewService::PreviewService(Converter& conv, StateStore& state, PreviewHub& hub, SendFn send)
    : conv_(conv), state_(state), hub_(hub), send_(std::move(send)) {}

void PreviewService::requestKeyframe(const std::string& id, PreviewSize size) {
  std::lock_guard l(keyMutex_);
  keyRequests_.insert({id, size});
}

void PreviewService::onTick(uint64_t tick, const std::vector<std::pair<std::string, TextureRef>>& frames) {
  if (broken_ || tick % 2 != 0) return;
  const auto wanted = hub_.wanted();
  for (auto it = encoders_.begin(); it != encoders_.end();) {
    it = wanted.contains(it->first) ? std::next(it) : encoders_.erase(it);
  }
  std::set<PreviewHub::Key> keys;
  {
    std::lock_guard l(keyMutex_);
    keys.swap(keyRequests_);
  }
  const int64_t ptsUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
  for (const auto& key : wanted) {
    const auto& [id, size] = key;
    TextureRef frame;
    for (auto& [fid, tex] : frames) {
      if (fid == id) frame = tex;
    }
    if (!frame) continue;
    auto& enc = encoders_[key];
    if (!enc) {
      try {
        const auto d = dims(size);
        enc = std::make_unique<PreviewEncoder>(d.w, d.h, 30);
        state_.set("/engine/previewEncoder", enc->encoderName());
      } catch (const std::exception& e) {
        encoders_.erase(key);
        broken_ = true;
        state_.set("/engine/warnings/preview", std::string("previews are unavailable: ") + e.what());
        return;
      }
    }
    if (keys.contains(key)) enc->forceKeyframe();
    const auto d = dims(size);
    const auto img = conv_.readback(*conv_.toBgra8(*frame, static_cast<uint32_t>(d.w), static_cast<uint32_t>(d.h)));
    for (auto& pkt : enc->encode(img, ptsUs)) {
      const auto msg = encodePreviewMessage(id, pkt);
      for (auto& client : hub_.clientsFor(id, size)) send_(client, msg);
    }
  }
}
}  // namespace bc
