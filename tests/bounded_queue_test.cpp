#include <doctest/doctest.h>

#include <thread>

#include "core/backoff.h"
#include "core/bounded_queue.h"

using namespace std::chrono_literals;

TEST_CASE("BoundedQueue: drops the oldest item when full and never blocks") {
  bc::BoundedQueue<int> q(2);
  CHECK_FALSE(q.push(1));
  CHECK_FALSE(q.push(2));
  CHECK(q.push(3));  // drops 1
  CHECK(q.dropped() == 1);
  CHECK(q.pop(0ms) == 2);
  CHECK(q.pop(0ms) == 3);
  CHECK_FALSE(q.pop(0ms));
}

TEST_CASE("BoundedQueue: pop wakes on push and returns nullopt after close") {
  bc::BoundedQueue<int> q(2);
  std::thread t([&] {
    std::this_thread::sleep_for(20ms);
    q.push(5);
  });
  CHECK(q.pop(1000ms) == 5);
  t.join();
  std::thread c([&] {
    std::this_thread::sleep_for(20ms);
    q.close();
  });
  auto start = std::chrono::steady_clock::now();
  CHECK_FALSE(q.pop(5000ms));
  CHECK(std::chrono::steady_clock::now() - start < 1000ms);
  c.join();
}

TEST_CASE("Backoff: doubles up to the cap and resets") {
  bc::Backoff b(500ms, 5000ms);
  CHECK(b.next() == 500ms);
  CHECK(b.next() == 1000ms);
  CHECK(b.next() == 2000ms);
  CHECK(b.next() == 4000ms);
  CHECK(b.next() == 5000ms);
  CHECK(b.next() == 5000ms);
  b.reset();
  CHECK(b.next() == 500ms);
}
