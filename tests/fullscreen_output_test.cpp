// DOCTEST_CONFIG_DOUBLE_STRINGIFY: without it doctest's ADL toString picks
// bc::toString(SignalState) (string_view) and string_view + const char* fails to compile.
#define DOCTEST_CONFIG_DOUBLE_STRINGIFY
#include <doctest/doctest.h>

#include <thread>

#include "gpu/converter.h"
#include "io/fullscreen_output.h"
#include "io/monitors.h"

using namespace std::chrono_literals;

TEST_CASE("listMonitors: at least one, primary first, indexes in order") {
  auto mons = bc::listMonitors();
  REQUIRE_FALSE(mons.empty());
  CHECK(mons[0].primary);
  for (size_t i = 0; i < mons.size(); ++i) CHECK(mons[i].index == static_cast<int>(i));
}

TEST_CASE("FullscreenOutput: a missing monitor reports an error instead of crashing") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::FullscreenOutput out(*gpu, 99);
  out.start();
  std::this_thread::sleep_for(300ms);
  auto st = out.status();
  CHECK(st.state == bc::SignalState::Offline);
  REQUIRE(st.error);
  CHECK(st.error->code == "fullscreen-failed");
  CHECK(st.error->message.find("monitor 99") != std::string::npos);
  out.stop();
}

TEST_CASE("FullscreenOutput: presents frames on monitor 0") {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::FullscreenOutput out(*gpu, 0);
  out.start();
  for (uint64_t i = 0; i < 30; ++i) {
    bc::OutFrame f;
    f.tick = i;
    f.linear = conv.solid(1920, 1080, {0.2f, 0.02f, 0.02f, 1.0f});
    out.submit(std::move(f));
    std::this_thread::sleep_for(16ms);
  }
  auto st = out.status();
  CHECK(st.state == bc::SignalState::Live);
  CHECK_FALSE(st.error);
  CHECK(st.delivered > 0);
  out.stop();
}
