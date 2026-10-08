#include <doctest/doctest.h>

#include <numeric>

#include "core/audio_delay.h"

using namespace std::chrono_literals;

TEST_CASE("AudioDelay: releases samples after the delay, silence before") {
  bc::AudioDelay d(1000, 1);  // 1 kHz mono: 10 ms = 10 frames
  d.setDelay(10ms);
  std::vector<float> in(20);
  std::iota(in.begin(), in.end(), 1.0f);
  d.push(in);
  CHECK(d.pull(10) == std::vector<float>(10, 0.0f));
  CHECK(d.pull(5) == std::vector<float>{1, 2, 3, 4, 5});
}

TEST_CASE("AudioDelay: underrun pads silence and is counted") {
  bc::AudioDelay d(1000, 2);
  d.setDelay(0ms);
  d.push(std::vector<float>{1, 1, 2, 2});
  CHECK(d.pull(4) == std::vector<float>{1, 1, 2, 2, 0, 0, 0, 0});
  CHECK(d.underruns() == 1);
}

TEST_CASE("AudioDelay: a buffer more than 100 ms over the delay is trimmed back to the delay") {
  bc::AudioDelay d(1000, 1);
  d.setDelay(10ms);
  d.push(std::vector<float>(500, 1.0f));
  CHECK(d.bufferedFrames() == 10);
}
