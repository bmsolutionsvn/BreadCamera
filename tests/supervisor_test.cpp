#include <doctest/doctest.h>

#include <fstream>
#include <thread>

#include "shell/process_launcher.h"
#include "shell/supervisor.h"
#include "support/temp_dir.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {
struct FakeLauncher : bc::EngineLauncher {
  bool running = false;
  int launchCount = 0, killCount = 0;
  bool isRunning() override { return running; }
  void launch() override {
    ++launchCount;
    running = true;
  }
  void kill() override {
    ++killCount;
    running = false;
  }
};
}  // namespace

TEST_CASE("Supervisor: launches when nothing runs and relaunches after an exit") {
  FakeLauncher l;
  bc::Supervisor s(l);
  auto t = Clock::now();
  s.poll(t);
  CHECK(l.launchCount == 1);
  l.running = false;  // engine exited
  s.poll(t + 250ms);
  CHECK(l.launchCount == 2);
}

TEST_CASE("Supervisor: missing heartbeats kill and relaunch, but not during the startup grace") {
  FakeLauncher l;
  bc::Supervisor s(l);
  auto t = Clock::now();
  s.poll(t);
  s.poll(t + 5s);  // inside the 8 s grace, no heartbeat yet
  CHECK(l.killCount == 0);
  s.heartbeat(t + 9s);
  s.poll(t + 11s);
  CHECK(l.killCount == 0);
  s.poll(t + 12500ms);  // 3.5 s since the last heartbeat
  CHECK(l.killCount == 1);
  CHECK(l.launchCount == 2);
}

TEST_CASE("Supervisor: a crash loop slows down to one restart every 10 s") {
  FakeLauncher l;
  bc::Supervisor s(l);
  auto t = Clock::now();
  for (int i = 0; i < 5; ++i) {
    s.poll(t + std::chrono::seconds(i));
    l.running = false;
  }
  CHECK(l.launchCount == 5);
  CHECK(s.crashLooping(t + 5s));
  s.poll(t + 6s);
  CHECK(l.launchCount == 5);  // waits
  s.poll(t + 14s);            // 10 s after the last launch
  CHECK(l.launchCount == 6);
}

TEST_CASE("Supervisor: stopSupervising means no more launches") {
  FakeLauncher l;
  bc::Supervisor s(l);
  s.stopSupervising();
  s.poll(Clock::now());
  CHECK(l.launchCount == 0);
}

TEST_CASE("ProcessLauncher: launches, sees, kills and adopts a real process") {
  bc::ProcessLauncher l("C:\\Windows\\System32\\PING.EXE", L"-n 30 127.0.0.1");
  CHECK_FALSE(l.isRunning());
  l.launch();
  CHECK(l.isRunning());
  bc::ProcessLauncher other("C:\\Windows\\System32\\PING.EXE", L"");
  other.adopt(l.pid());
  CHECK(other.isRunning());
  l.kill();
  CHECK_FALSE(l.isRunning());
  std::this_thread::sleep_for(100ms);
  CHECK_FALSE(other.isRunning());
}

TEST_CASE("readDiscovery: parses the engine file and rejects junk") {
  bc::test::TempDir dir;
  auto f = dir.path / "engine.json";
  std::ofstream(f) << R"({"pid":1234,"port":47810,"token":"abcdef0123456789"})";
  auto d = bc::readDiscovery(f);
  REQUIRE(d);
  CHECK(d->pid == 1234);
  CHECK(d->port == 47810);
  CHECK(d->token == "abcdef0123456789");
  std::ofstream(f, std::ios::trunc) << "junk";
  CHECK_FALSE(bc::readDiscovery(f));
  CHECK_FALSE(bc::readDiscovery(dir.path / "missing.json"));
}

TEST_CASE("isEngineProcess: refuses a live process that is not the engine") {
  bc::ProcessLauncher l("C:\\Windows\\System32\\PING.EXE", L"-n 30 127.0.0.1");
  l.launch();
  REQUIRE(l.isRunning());
  // engine.json survives an unclean exit, so the pid it names can already have
  // reused: adopting it would end with TerminateProcess on that process.
  CHECK_FALSE(bc::isEngineProcess(l.pid()));
  CHECK_FALSE(bc::isEngineProcess(GetCurrentProcessId()));
  CHECK_FALSE(bc::isEngineProcess(0));
  l.kill();
}

TEST_CASE("waitForDiscovery: waits for a file that appears and gives up on one that does not") {
  bc::test::TempDir dir;
  const auto f = dir.path / "engine.json";
  std::thread writer([&] {
    std::this_thread::sleep_for(150ms);
    std::ofstream(f) << R"({"pid":7,"port":47810,"token":"0123456789abcdef"})";
  });
  auto d = bc::waitForDiscovery(f, std::chrono::seconds(2));
  writer.join();
  REQUIRE(d);
  CHECK(d->pid == 7);
  CHECK(d->token == "0123456789abcdef");
  CHECK_FALSE(bc::waitForDiscovery(dir.path / "never.json", std::chrono::milliseconds(300)));
}
