#pragma once
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace bc {
// UX-contract signal states, in their fixed order. Errors are separate (spec §6.1).
enum class SignalState { Ready, Connecting, Live, Offline };

std::string_view toString(SignalState s);

struct ErrorInfo {
  std::string code;
  std::string message;
  bool operator==(const ErrorInfo&) const = default;
};

nlohmann::json errorToJson(const std::optional<ErrorInfo>& e);
}  // namespace bc
