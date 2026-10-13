#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include "core/vcam_shared.h"

using namespace std::chrono_literals;

TEST_CASE("vcam shared memory: writer to reader, and stale frames") {
  bc::vcam::Reader r;
  REQUIRE(r.create(9, false));  // Local\ for tests
  bc::vcam::Writer w;
  REQUIRE(w.open(9, false));
  std::vector<uint8_t> frame(bc::vcam::kFrameBytes, 0x40), got(bc::vcam::kFrameBytes, 0);
  CHECK_FALSE(r.readLatest(got.data(), 1000ms));  // nothing written yet
  CHECK(w.write(frame.data()));
  REQUIRE(r.readLatest(got.data(), 1000ms));
  CHECK(got[12345] == 0x40);
  std::this_thread::sleep_for(60ms);
  CHECK_FALSE(r.readLatest(got.data(), 50ms));  // too old
  w.close();
  r.close();
}

TEST_CASE("vcam shared memory: a reader never accepts a torn frame") {
  bc::vcam::Reader r;
  REQUIRE(r.create(10, false));
  bc::vcam::Writer w;
  REQUIRE(w.open(10, false));
  std::atomic<bool> run{true};
  std::thread writer([&] {
    std::vector<uint8_t> a(bc::vcam::kFrameBytes, 0x11), b(bc::vcam::kFrameBytes, 0x22);
    for (int i = 0; run; ++i) {
      w.write(i % 2 ? a.data() : b.data());
      // The engine publishes at frame cadence (~30 fps), so a completed (even seq) frame stays
      // readable for a whole frame interval. A back-to-back writer leaves that window at ~0 ns
      // (measured: 8-11 even seq samples in 2,000,000), which is not a real scenario.
      std::this_thread::sleep_for(2ms);
    }
  });
  std::vector<uint8_t> got(bc::vcam::kFrameBytes);
  int good = 0;
  for (int i = 0; i < 200; ++i) {
    if (!r.readLatest(got.data(), 1000ms)) {
      // Poll at the consumer cadence; without this the 200 iterations finish in microseconds,
      // inside a single write, and can never observe a complete frame.
      std::this_thread::sleep_for(1ms);
      continue;
    }
    ++good;
    const uint8_t first = got[0];
    CHECK((first == 0x11 || first == 0x22));
    CHECK(got[bc::vcam::kFrameBytes - 1] == first);  // whole frame from one write
  }
  run = false;
  writer.join();
  CHECK(good > 0);
}

TEST_CASE("vcam: the writer opens nothing when no reader exists") {
  bc::vcam::Writer w;
  CHECK_FALSE(w.open(11, false));
}