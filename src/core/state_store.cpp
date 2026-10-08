#include "core/state_store.h"

namespace bc {
void StateStore::set(std::string_view pointer, nlohmann::json value) {
  nlohmann::json::json_pointer ptr{std::string(pointer)};
  std::lock_guard l(m_);
  if (state_.contains(ptr) && state_.at(ptr) == value) return;
  state_[ptr] = value;
  ops_.push_back({{"op", "replace"}, {"path", std::string(pointer)}, {"value", std::move(value)}});
}

void StateStore::erase(std::string_view pointer) {
  nlohmann::json::json_pointer ptr{std::string(pointer)};
  std::lock_guard l(m_);
  if (!state_.contains(ptr)) return;
  auto parent = ptr.parent_pointer();
  state_.at(parent).erase(ptr.back());
  ops_.push_back({{"op", "remove"}, {"path", std::string(pointer)}});
}

nlohmann::json StateStore::snapshot() const {
  std::lock_guard l(m_);
  return state_;
}

nlohmann::json StateStore::takePatch() {
  std::lock_guard l(m_);
  nlohmann::json out = std::move(ops_);
  ops_ = nlohmann::json::array();
  return out;
}
}  // namespace bc
