#pragma once
#include <string_view>

#include "core/frame_sync.h"
#include "core/signal_state.h"

namespace bc {
// What the operator sees on the input chip (spec §6.1).
inline std::string_view inputHealth(SignalState input, FeedPhase phase, bool stutteredInLast10s) {
  if (phase == FeedPhase::Holding || phase == FeedPhase::Slate) return "down";
  if (phase == FeedPhase::NoSignal) return "offline";
  if (input != SignalState::Live || stutteredInLast10s) return "degraded";
  return "receiving";
}
}  // namespace bc
