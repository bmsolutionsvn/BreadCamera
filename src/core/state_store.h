#pragma once
#include <mutex>
#include <string_view>

#include <nlohmann/json.hpp>

namespace bc {
// Engine-side live state pushed to the UI: one snapshot on connect, then patches
// (spec §6.1). Thread-safe; inputs and outputs report from their own threads.
class StateStore {
 public:
  void set(std::string_view pointer, nlohmann::json value);
  void erase(std::string_view pointer);
  nlohmann::json snapshot() const;
  nlohmann::json takePatch();

 private:
  mutable std::mutex m_;
  nlohmann::json state_ = nlohmann::json::object();
  nlohmann::json ops_ = nlohmann::json::array();
};
}  // namespace bc
