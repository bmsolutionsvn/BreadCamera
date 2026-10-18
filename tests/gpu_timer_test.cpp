#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <thread>
#include <vector>

#include "gpu/converter.h"
#include "gpu/device.h"
#include "gpu/gpu_timer.h"
#include "gpu/texture_pool.h"

using namespace std::chrono_literals;

TEST_CASE("GpuTimer: measures a dispatch and reports it once") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::GpuTimer t{*gpu};
  t.begin();
  auto f = conv.solid(1920, 1080, {0.2f, 0.2f, 0.2f, 1});
  conv.toBgra8(*f, 1920, 1080);
  t.end();
  std::optional<std::chrono::nanoseconds> got;
  for (int i = 0; i < 200 && !got; ++i) {
    gpu->context()->Flush();
    got = t.poll();
    std::this_thread::sleep_for(5ms);
  }
  REQUIRE(got);
  CHECK(got->count() > 0);
  CHECK_FALSE(t.poll());  // reported once
}

TEST_CASE("GpuTimer: five begins without a poll never block and the fifth records nothing") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::GpuTimer t{*gpu};
  // The ring holds four sets; the fifth begin/end has nowhere to go and records nothing.
  for (int i = 0; i < 5; ++i) {
    t.begin();
    t.end();
  }
  std::vector<std::chrono::nanoseconds> got;
  for (int i = 0; i < 200 && got.size() < 4; ++i) {
    gpu->context()->Flush();
    if (auto r = t.poll()) got.push_back(*r);
    std::this_thread::sleep_for(2ms);
  }
  CHECK(got.size() <= 4);  // the fifth set was never recorded
  CHECK(got.size() >= 1);
  CHECK_FALSE(t.poll());  // all finished measurements drained
}