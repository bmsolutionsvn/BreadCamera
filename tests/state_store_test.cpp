#include <doctest/doctest.h>

#include "core/signal_state.h"
#include "core/state_store.h"

using nlohmann::json;

TEST_CASE("SignalState: wire names match the UX contract order") {
  CHECK(bc::toString(bc::SignalState::Ready) == "ready");
  CHECK(bc::toString(bc::SignalState::Connecting) == "connecting");
  CHECK(bc::toString(bc::SignalState::Live) == "live");
  CHECK(bc::toString(bc::SignalState::Offline) == "offline");
  CHECK(bc::errorToJson(std::nullopt).is_null());
  CHECK(bc::errorToJson(bc::ErrorInfo{"x", "y"}) == json{{"code", "x"}, {"message", "y"}});
}

TEST_CASE("StateStore: set creates parents and records one replace op") {
  bc::StateStore s;
  s.set("/cameras/abc/input/state", "live");
  CHECK(s.snapshot()["cameras"]["abc"]["input"]["state"] == "live");
  auto p = s.takePatch();
  REQUIRE(p.size() == 1);
  CHECK(p[0] == json{{"op", "replace"}, {"path", "/cameras/abc/input/state"}, {"value", "live"}});
  CHECK(s.takePatch().empty());
}

TEST_CASE("StateStore: setting the same value records nothing") {
  bc::StateStore s;
  s.set("/engine/rate", "60/1");
  s.takePatch();
  s.set("/engine/rate", "60/1");
  CHECK(s.takePatch().empty());
}

TEST_CASE("StateStore: erase removes the subtree and records a remove op") {
  bc::StateStore s;
  s.set("/cameras/abc/name", "Cam");
  s.takePatch();
  s.erase("/cameras/abc");
  CHECK_FALSE(s.snapshot()["cameras"].contains("abc"));
  auto p = s.takePatch();
  REQUIRE(p.size() == 1);
  CHECK(p[0] == json{{"op", "remove"}, {"path", "/cameras/abc"}});
  s.erase("/cameras/missing");
  CHECK(s.takePatch().empty());
}

TEST_CASE("StateStore: snapshot starts as an empty object") {
  bc::StateStore s;
  CHECK(s.snapshot() == json::object());
}
