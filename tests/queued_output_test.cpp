// DOCTEST_CONFIG_DOUBLE_STRINGIFY: without it doctest's ADL toString picks
// bc::toString(SignalState) (string_view) and string_view + const char* fails to compile.
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

#include "support/fake_output.h"

using namespace std::chrono_literals;

namespace {
bc::OutFrame frame(uint64_t tick) {
  bc::OutFrame f;
  f.tick = tick;
  return f;
}
template <class P>
bool waitUntil(P pred, std::chrono::milliseconds t) {
  auto end = std::chrono::steady_clock::now() + t;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(5ms);
  }
  return pred();
}
}  // namespace

TEST_CASE("QueuedOutput: Ready, then Live after the first delivered frame") {
  bc::test::FakeOutput out;
  CHECK(out.status().state == bc::SignalState::Ready);
  out.start();
  out.submit(frame(1));
  CHECK(waitUntil([&] { return out.status().state == bc::SignalState::Live; }, 1000ms));
  out.stop();
  CHECK(out.status().state == bc::SignalState::Offline);
  CHECK_FALSE(out.status().error);
}

TEST_CASE("QueuedOutput: a slow output never blocks submit and drops its own oldest frames") {
  bc::test::FakeOutput out;
  out.delayMs = 100;
  out.start();
  auto t0 = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < 10; ++i) out.submit(frame(i));
  CHECK(std::chrono::steady_clock::now() - t0 < 20ms);
  CHECK(waitUntil([&] { return out.status().dropped >= 7; }, 500ms));
  out.stop();
}

TEST_CASE("QueuedOutput: a delivery exception goes Offline with an error, then recovers") {
  bc::test::FakeOutput out;
  out.failOnDelivery = 2;
  out.start();
  out.submit(frame(1));
  CHECK(waitUntil([&] { return out.delivered >= 1; }, 1000ms));
  out.submit(frame(2));
  CHECK(waitUntil([&] { return out.status().state == bc::SignalState::Offline; }, 1000ms));
  auto st = out.status();
  REQUIRE(st.error);
  CHECK(st.error->code == "output-failed");
  CHECK(st.error->message == "boom");
  CHECK(waitUntil([&] { return out.opens >= 2; }, 2000ms));  // reopened after the 500 ms backoff
  out.submit(frame(3));
  CHECK(waitUntil([&] { return out.status().state == bc::SignalState::Live; }, 1000ms));
  CHECK_FALSE(out.status().error);
  out.stop();
}

TEST_CASE("QueuedOutput: stop joins quickly even while idle") {
  bc::test::FakeOutput out;
  out.start();
  auto t0 = std::chrono::steady_clock::now();
  out.stop();
  CHECK(std::chrono::steady_clock::now() - t0 < 300ms);
}
