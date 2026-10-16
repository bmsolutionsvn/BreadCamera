#include "engine/preview_service.h"

#include <chrono>

namespace bc {
PreviewService::PreviewService(StateStore& state, PreviewHub& hub, SendFn send)
    : state_(state), hub_(hub), send_(std::move(send)) {}

void PreviewService::requestKeyframe(const std::string& id, PreviewSize size) {
  std::lock_guard l(keyMutex_);
  keyRequests_.insert({id, size});
}

void PreviewService::onTick(uint64_t tick, const std::vector<FrameTap>& frames) {
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
    // The tap already carries the pixels the outputs produced: no preview render, no readback.
    // The shared image is <=1 tick behind (D8), so skip until the first copy lands.
    const FrameTap* tap = nullptr;
    for (const auto& f : frames) {
      if (f.id == id) tap = &f;
    }
    if (!tap || !tap->cpu) continue;
    const CpuImage& img = *tap->cpu;
    auto& slot = encoders_[key];
    // Encode at the frame size, not the wire size (D7). Reopen only if the frame size changed.
    if (slot.enc && (slot.w != img.width || slot.h != img.height)) {
      slot.enc.reset();
      slot.w = slot.h = 0;
    }
    if (!slot.enc) {
      try {
        slot.enc = std::make_unique<PreviewEncoder>(static_cast<int>(img.width), static_cast<int>(img.height), 30);
        slot.w = img.width;
        slot.h = img.height;
        state_.set("/engine/previewEncoder", slot.enc->encoderName());
      } catch (const std::exception& e) {
        encoders_.erase(key);
        broken_ = true;
        state_.set("/engine/warnings/preview", std::string("previews are unavailable: ") + e.what());
        return;
      }
    }
    if (keys.contains(key)) slot.enc->forceKeyframe();
    for (auto& pkt : slot.enc->encode(img, ptsUs)) {
      const auto msg = encodePreviewMessage(id, pkt);
      for (auto& client : hub_.clientsFor(id, size)) send_(client, msg);
    }
  }
}
}  // namespace bc
