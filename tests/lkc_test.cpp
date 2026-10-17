// C1 Task 1 — the shared LiveKit client. These cases need no server; the live
// case is gated behind Req::LiveCall ("no LiveKit server").
#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>

#include "io/lkc/lkc.h"
#include "support/requires.h"

namespace {
// A structurally valid LiveKit token (three base64url segments). It is not a
// real credential; anything that needs the server is skipped.
constexpr const char* kTokenShape = "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ4In0.c2lnbmF0dXJl";

std::optional<std::string> envVar(const char* name) {
  char* buf = nullptr;
  size_t len = 0;
  if (_dupenv_s(&buf, &len, name) != 0 || buf == nullptr) return std::nullopt;
  std::string v(buf);
  std::free(buf);
  return v;
}

struct StateWatch {
  std::mutex m;
  std::condition_variable cv;
  bool have = false;
  bc::SignalState state = bc::SignalState::Ready;
  std::optional<bc::ErrorInfo> err;

  void on(bc::SignalState s, std::optional<bc::ErrorInfo> e) {
    std::lock_guard lk(m);
    have = true;
    state = s;
    err = e;
    cv.notify_all();
  }
  bool wait(int ms) {
    std::unique_lock lk(m);
    return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return have; });
  }
  // Waits for a settled outcome: Live (media flowing) or Offline (failed).
  bool waitSettled(int ms) {
    std::unique_lock lk(m);
    return cv.wait_for(lk, std::chrono::milliseconds(ms), [&] {
      return (have && state == bc::SignalState::Live) || state == bc::SignalState::Offline;
    });
  }
  bool isLive() {
    std::lock_guard lk(m);
    return state == bc::SignalState::Live;
  }
};
}  // namespace

TEST_CASE("lkc::Client reports a config error for a bad token") {
  bc::lkc::Client client("ws://127.0.0.1:7880", "not-a-jwt");
  StateWatch w;
  client.onState([&](bc::SignalState s, std::optional<bc::ErrorInfo> e) { w.on(s, e); });
  REQUIRE(w.wait(4000));
  CHECK(static_cast<int>(w.state) == static_cast<int>(bc::SignalState::Offline));
  REQUIRE(w.err.has_value());
  CHECK(w.err->code == "lkc-config");
}

TEST_CASE("lkc::Client reports unreachable for a bad url") {
  bc::lkc::Client client("this is not a url", kTokenShape);
  StateWatch w;
  client.onState([&](bc::SignalState s, std::optional<bc::ErrorInfo> e) { w.on(s, e); });
  REQUIRE(w.wait(4000));
  CHECK(static_cast<int>(w.state) == static_cast<int>(bc::SignalState::Offline));
  REQUIRE(w.err.has_value());
  CHECK(w.err->code == "lkc-unreachable");
}

TEST_CASE("lkc::Client survives a valid token aimed at a dead endpoint") {
  // ws://127.0.0.1:1 is syntactically valid and refuses the connection; the
  // client must not crash on the way in or out.
  bc::lkc::Client client("ws://127.0.0.1:1", kTokenShape);
  StateWatch w;
  client.onState([&](bc::SignalState s, std::optional<bc::ErrorInfo> e) { w.on(s, e); });
  // It may report unreachable, or still be connecting on a slow machine; either
  // way construction and destruction are clean.
  w.wait(1500);
  CHECK(true);
}

TEST_CASE("lkc::Client joins, subscribes and receives a video frame"
          BC_REQUIRES(bc::test::Req::LiveCall)) {
  const std::string url = envVar("BC_LIVECALL_URL").value_or("");
  const std::string token = envVar("BC_LIVECALL_TOKEN").value_or("");
  const std::string identity = envVar("BC_LIVECALL_IDENTITY").value_or("");

  const auto started = std::chrono::steady_clock::now();
  bc::lkc::Client client(url, token);
  if (!identity.empty()) client.subscribe(identity);
  StateWatch w;
  client.onState([&](bc::SignalState s, std::optional<bc::ErrorInfo> e) { w.on(s, e); });
  std::mutex fm;
  std::condition_variable fcv;
  bool gotVideo = false;
  size_t videoBytes = 0;
  client.onVideo([&](bc::lkc::VideoSample s) {
    std::lock_guard lk(fm);
    gotVideo = true;
    videoBytes = s.annexB.size();
    fcv.notify_all();
  });
  REQUIRE(w.waitSettled(20000));
  const bool live = w.isLive();
  if (!live && w.err)
    std::fprintf(stderr, "[lkc live] settled offline after %lld ms: %s: %s\n",
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - started)
                                             .count()),
                 w.err->code.c_str(), w.err->message.c_str());
  CHECK(live);
  if (live) {
    std::unique_lock lk(fm);
    const bool got = fcv.wait_for(lk, std::chrono::seconds(15), [&] { return gotVideo; });
    CHECK(got);
    CHECK(videoBytes > 0);
  }
}