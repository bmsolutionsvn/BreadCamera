#include <doctest/doctest.h>

#include "core/input_health.h"

using bc::FeedPhase;
using bc::SignalState;

TEST_CASE("inputHealth: phases and stutter map to the four operator states") {
  CHECK(bc::inputHealth(SignalState::Live, FeedPhase::Live, false) == "receiving");
  CHECK(bc::inputHealth(SignalState::Live, FeedPhase::Live, true) == "degraded");
  CHECK(bc::inputHealth(SignalState::Connecting, FeedPhase::Live, false) == "degraded");
  CHECK(bc::inputHealth(SignalState::Live, FeedPhase::Holding, false) == "down");
  CHECK(bc::inputHealth(SignalState::Offline, FeedPhase::Slate, false) == "down");
  CHECK(bc::inputHealth(SignalState::Connecting, FeedPhase::NoSignal, false) == "offline");
}
