#include "engine/surface_hub.h"

#include "gpu/converter.h"

namespace bc {
namespace {
constexpr auto kMinPresentGap = std::chrono::milliseconds(15);  // a box at 60 fps is wanted once a frame
}  // namespace

SurfaceHub::SurfaceHub(GpuDevice& gpu, Converter& conv) : gpu_(gpu), conv_(conv) {}

std::vector<SurfaceGrant> SurfaceHub::subscribe(const std::string& client, const std::vector<SurfaceRequest>& wanted) {
  std::lock_guard l(m_);
  auto gpuLock = gpu_.lock();  // presenters are created and destroyed under the GPU lock
  auto& current = byClient_[client];
  std::map<std::string, Entry> next;
  std::vector<SurfaceGrant> grants;
  grants.reserve(wanted.size());
  for (const auto& req : wanted) {
    if (next.contains(req.id)) continue;  // a duplicate id is one surface
    Entry entry;
    if (auto it = current.find(req.id); it != current.end()) {
      entry = std::move(it->second);  // kept, only resized below
    } else {
      try {
        entry.presenter = std::make_unique<SurfacePresenter>(gpu_, conv_, req.w, req.h);
      } catch (...) {
        continue;  // no picture for this one; the page shows an empty box
      }
    }
    entry.presenter->resize(req.w, req.h);
    grants.push_back({req.id, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry.presenter->handle())),
                      entry.presenter->width(), entry.presenter->height()});
    next.emplace(req.id, std::move(entry));
  }
  current = std::move(next);  // drops the presenters no longer listed
  return grants;
}

void SurfaceHub::dropClient(const std::string& client) {
  std::lock_guard l(m_);
  auto gpuLock = gpu_.lock();
  byClient_.erase(client);
}

std::set<std::string> SurfaceHub::wanted() const {
  std::lock_guard l(m_);
  std::set<std::string> ids;
  for (const auto& [client, entries] : byClient_) {
    for (const auto& [id, entry] : entries) ids.insert(id);
  }
  return ids;
}

void SurfaceHub::present(const std::string& id, const GpuTexture& linear) {
  std::lock_guard l(m_);
  const auto now = std::chrono::steady_clock::now();
  for (auto& [client, entries] : byClient_) {
    auto it = entries.find(id);
    if (it == entries.end() || now < it->second.nextDue) continue;
    it->second.nextDue = now + kMinPresentGap;
    it->second.presenter->present(linear);
  }
}

bool SurfaceHub::anyBroken() const {
  std::lock_guard l(m_);
  for (const auto& [client, entries] : byClient_) {
    for (const auto& [id, entry] : entries) {
      if (entry.presenter->broken()) return true;
    }
  }
  return false;
}

size_t SurfaceHub::presenterCount() const {
  std::lock_guard l(m_);
  size_t n = 0;
  for (const auto& [client, entries] : byClient_) n += entries.size();
  return n;
}
}  // namespace bc