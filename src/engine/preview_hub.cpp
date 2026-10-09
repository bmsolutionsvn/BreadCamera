#include "engine/preview_hub.h"

namespace bc {
std::optional<PreviewSize> parsePreviewSize(std::string_view s) {
  if (s == "small") return PreviewSize::Small;
  if (s == "large") return PreviewSize::Large;
  return std::nullopt;
}

std::vector<PreviewHub::Key> PreviewHub::subscribe(const std::string& client, std::vector<std::string> ids, PreviewSize size) {
  std::lock_guard l(m_);
  std::set<Key> next;
  for (auto& id : ids) next.insert({id, size});
  std::vector<Key> fresh;
  auto& cur = byClient_[client];
  for (auto& k : next) {
    if (!cur.contains(k)) fresh.push_back(k);
  }
  cur = std::move(next);
  if (cur.empty()) byClient_.erase(client);
  return fresh;
}

void PreviewHub::dropClient(const std::string& client) {
  std::lock_guard l(m_);
  byClient_.erase(client);
}

std::set<PreviewHub::Key> PreviewHub::wanted() const {
  std::lock_guard l(m_);
  std::set<Key> all;
  for (auto& [c, keys] : byClient_) all.insert(keys.begin(), keys.end());
  return all;
}

std::vector<std::string> PreviewHub::clientsFor(const std::string& id, PreviewSize size) const {
  std::lock_guard l(m_);
  std::vector<std::string> out;
  for (auto& [c, keys] : byClient_) {
    if (keys.contains({id, size})) out.push_back(c);
  }
  return out;
}
}  // namespace bc
