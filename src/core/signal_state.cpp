#include "core/signal_state.h"

namespace bc {
std::string_view toString(SignalState s) {
  switch (s) {
    case SignalState::Ready: return "ready";
    case SignalState::Connecting: return "connecting";
    case SignalState::Live: return "live";
    case SignalState::Offline: return "offline";
  }
  return "offline";
}

nlohmann::json errorToJson(const std::optional<ErrorInfo>& e) {
  if (!e) return nullptr;
  return {{"code", e->code}, {"message", e->message}};
}
}  // namespace bc
