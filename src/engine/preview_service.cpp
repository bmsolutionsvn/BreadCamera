#include "engine/preview_service.h"

#include <chrono>
#include <exception>
#include <iterator>
#include <utility>

namespace bc {
// Per-camera encoder plus its worker thread and one-slot queue. onTick writes the newest CpuImage
// and returns; the worker encodes and sends. w/h/enc/failed are touched only by the worker, the
// queue flags under m.
struct PreviewService::EncoderSlot {
  explicit EncoderSlot(std::string camera) : id(std::move(camera)) {}
  std::string id;
  std::thread worker;
  std::mutex m;
  std::condition_variable cv;
  bool hasPending = false;
  bool busy = false;
  bool stop = false;
  bool forceKey = false;
  std::atomic<bool> failed{false};
  std::shared_ptr<const CpuImage> pending;
  int64_t ptsUs = 0;
  uint32_t w = 0, h = 0;
  std::unique_ptr<PreviewEncoder> enc;
  std::atomic<bool> done{false};
};

PreviewService::PreviewService(StateStore& state, PreviewHub& hub, SendFn send)
    : state_(state), hub_(hub), send_(std::move(send)) {}

PreviewService::~PreviewService() {
  auto stop = [](EncoderSlot* s) {
    {
      std::lock_guard l(s->m);
      s->stop = true;
    }
    s->cv.notify_all();
  };
  for (auto& [id, s] : slots_) stop(s.get());
  for (auto& s : retiring_) stop(s.get());
  for (auto& [id, s] : slots_) {
    if (s->worker.joinable()) s->worker.join();
  }
  for (auto& s : retiring_) {
    if (s->worker.joinable()) s->worker.join();
  }
}

void PreviewService::requestKeyframe(const std::string& id, PreviewSize size) {
  std::lock_guard l(keyMutex_);
  keyRequests_.insert({id, size});
}

void PreviewService::reap() {
  for (auto it = retiring_.begin(); it != retiring_.end();) {
    if (!(*it)->done) {
      ++it;
      continue;
    }
    if ((*it)->worker.joinable()) (*it)->worker.join();
    it = retiring_.erase(it);
  }
}

// The worker: waits for a queued image (or a stop), opens/reopens the encoder at the frame size,
// encodes, and sends the packets to every client watching this camera. One slot means a slow encode
// drops intermediate frames rather than backing up the tick (D4/D8).
void PreviewService::run(EncoderSlot* s) {
  for (;;) {
    std::shared_ptr<const CpuImage> img;
    int64_t ptsUs = 0;
    bool forceKey = false;
    {
      std::unique_lock l(s->m);
      s->cv.wait(l, [&] { return s->hasPending || s->stop; });
      if (s->stop && !s->hasPending) break;
      img = std::move(s->pending);
      s->hasPending = false;
      s->busy = true;
      ptsUs = s->ptsUs;
      forceKey = s->forceKey;
      s->forceKey = false;
    }
    if (img) {
      try {
        // Encode at the frame size, not the wire size (D7). Reopen only if the size changed.
        if (!s->enc || s->w != img->width || s->h != img->height) {
          s->enc = std::make_unique<PreviewEncoder>(static_cast<int>(img->width), static_cast<int>(img->height), 30);
          s->w = img->width;
          s->h = img->height;
          state_.set("/engine/previewEncoder", s->enc->encoderName());
        }
        if (forceKey) s->enc->forceKeyframe();
        for (auto& pkt : s->enc->encode(*img, ptsUs)) {
          const auto msg = encodePreviewMessage(s->id, pkt);
          // The wire size is ignored (D7), so send to every client watching this camera.
          std::vector<std::string> clients = hub_.clientsFor(s->id, PreviewSize::Small);
          for (auto& c : hub_.clientsFor(s->id, PreviewSize::Large)) clients.push_back(c);
          for (auto& c : clients) send_(c, msg);
        }
      } catch (const std::exception&) {
        // One tile's encoder failed to open: skip it, do not blank every preview (D9).
        std::lock_guard l(s->m);
        s->failed = true;
        s->enc.reset();
      }
    }
    {
      std::lock_guard l(s->m);
      s->busy = false;
    }
    s->cv.notify_all();
  }
  s->done = true;
}

void PreviewService::onTick(uint64_t tick, const std::vector<FrameTap>& frames) {
  if (tick % 2 != 0) return;  // half-rate (D6): previews encode every 2nd tick
  reap();
  const auto wanted = hub_.wanted();
  std::set<std::string> cams;
  for (const auto& [id, size] : wanted) cams.insert(id);

  // Drop encoders for cameras nobody watches any more, and forget their failed flag.
  for (auto it = slots_.begin(); it != slots_.end();) {
    if (cams.contains(it->first)) {
      ++it;
      continue;
    }
    EncoderSlot* s = it->second.get();
    {
      std::lock_guard l(s->m);
      s->stop = true;
    }
    s->cv.notify_all();
    retiring_.push_back(std::move(it->second));
    it = slots_.erase(it);
  }
  for (auto it = dead_.begin(); it != dead_.end();) {
    it = cams.contains(*it) ? std::next(it) : dead_.erase(it);
  }

  std::set<PreviewHub::Key> keys;
  {
    std::lock_guard l(keyMutex_);
    keys.swap(keyRequests_);
  }
  std::set<std::string> forceCams;
  for (const auto& [id, size] : keys) forceCams.insert(id);

  const int64_t ptsUs = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
  size_t skipped = 0;
  for (const auto& cam : cams) {
    auto it = slots_.find(cam);
    if (it != slots_.end() && it->second->failed) {
      EncoderSlot* s = it->second.get();
      {
        std::lock_guard l(s->m);
        s->stop = true;
      }
      s->cv.notify_all();
      retiring_.push_back(std::move(it->second));
      slots_.erase(it);
      dead_.insert(cam);
      continue;
    }
    if (dead_.contains(cam)) continue;
    // The tap already carries the pixels the outputs produced (Task 2): no preview render, no
    // readback. The shared image is <=1 tick behind, so skip until the first copy lands (D8).
    const FrameTap* tap = nullptr;
    for (const auto& f : frames) {
      if (f.id == cam) {
        tap = &f;
        break;
      }
    }
    if (!tap || !tap->cpu) continue;
    it = slots_.find(cam);
    if (it == slots_.end()) {
      if (slots_.size() >= kMaxPreviewEncoders) {
        ++skipped;  // past the cap: skip this tile, keep the others running (D9)
        continue;
      }
      auto slot = std::make_unique<EncoderSlot>(cam);
      EncoderSlot* raw = slot.get();
      raw->worker = std::thread(&PreviewService::run, this, raw);
      it = slots_.emplace(cam, std::move(slot)).first;
    }
    EncoderSlot* s = it->second.get();
    {
      std::lock_guard l(s->m);
      s->pending = tap->cpu;
      s->hasPending = true;
      s->ptsUs = ptsUs;
      if (forceCams.contains(cam)) s->forceKey = true;
    }
    s->cv.notify_one();
  }

  bool deadWanted = false;
  for (const auto& cam : cams) {
    if (dead_.contains(cam)) deadWanted = true;
  }
  if (skipped > 0 || deadWanted) {
    state_.set("/engine/warnings/preview", "preview limit: showing " + std::to_string(slots_.size()) +
                                               " of " + std::to_string(cams.size()) + " cameras");
  } else {
    state_.erase("/engine/warnings/preview");
  }
}

void PreviewService::drain() {
  for (auto& [id, s] : slots_) {
    std::unique_lock l(s->m);
    s->cv.wait(l, [&] { return !s->hasPending && !s->busy; });
  }
}
}  // namespace bc
