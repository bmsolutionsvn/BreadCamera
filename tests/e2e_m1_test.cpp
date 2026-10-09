#include <doctest/doctest.h>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <atomic>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

#include "core/show_store.h"
#include "core/state_store.h"
#include "engine/engine.h"
#include "gpu/device.h"
#include "shell/process_launcher.h"
#include "shell/supervisor.h"
#include "support/fake_input.h"
#include "support/requires.h"
#include "support/temp_dir.h"
#include "support/test_media.h"

using namespace std::chrono_literals;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {
// Real SRT inputs, recording fake outputs.
struct HybridFactory : bc::AdapterFactory {
  bc::DefaultAdapterFactory real;
  bc::test::FakeFactory fake;
  HybridFactory(bc::GpuDevice& g, bc::TexturePool& p, bc::Converter& c) : real(g, p), fake(c) {}
  std::unique_ptr<bc::Input> makeInput(const bc::InputConfig& cfg) override { return real.makeInput(cfg); }
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig& cfg, const bc::CameraConfig& cam) override {
    return fake.makeOutput(cfg, cam);
  }
};

std::string phase(bc::StateStore& s, const std::string& id) {
  auto p = json::json_pointer("/cameras/" + id + "/phase");
  auto snap = s.snapshot();
  return snap.contains(p) ? snap.at(p).get<std::string>() : "";
}

int countSince(bc::test::FakeOutput* o, Clock::time_point a, Clock::time_point b) {
  int n = 0;
  for (auto& s : o->seen()) n += (s.at >= a && s.at < b) ? 1 : 0;
  return n;
}

std::filesystem::path exeDir() {
  wchar_t buf[MAX_PATH];
  GetModuleFileNameW(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path();
}

// RAII teardown for the M1 acceptance cases (review batch 6, finding 1): a REQUIRE-thrown
// abort must still run the case's tail cleanup — otherwise a joinable poller destroyed while
// unwinding std::terminates the whole bc_tests binary and an engine process is orphaned.
// Declare it AFTER the resources it tears down (so its destructor runs while they are still
// alive); steps run once, in registration order. The cases' former tail-cleanup statements
// are folded into the steps, so the normal path behaves as before.
struct CaseGuard {
  void defer(std::function<void()> step) { steps_.push_back(std::move(step)); }
  ~CaseGuard() {
    for (auto& s : steps_) s();
  }

 private:
  std::vector<std::function<void()>> steps_;
};
}  // namespace

TEST_CASE("M1 acceptance: 4x 1080p60 SRT passthrough, then hold and slate on one camera" BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  bc::test::TempDir dir;
  const auto clip = dir.path / "1080p60.ts";
  bc::test::writeTestTs(clip, 1920, 1080, 60, 10);

  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::StateStore state;
  HybridFactory factory(*gpu, pool, conv);
  bc::Engine engine(conv, state, factory);

  bc::Show show;
  show.settings.holdMs = 2000;
  for (int i = 0; i < 4; ++i) {
    bc::CameraConfig c;
    c.id = "cam" + std::to_string(i);
    c.name = "Cam " + std::to_string(i);
    c.input.kind = "srt";
    c.input.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 47911 + i}, {"latencyMs", 120}};
    c.outputs.push_back({"out" + std::to_string(i), "fake", false, true, json::object()});
    show.cameras.push_back(c);
  }
  engine.apply(show);
  engine.start();
  std::vector<std::unique_ptr<bc::test::TsPusher>> pushers;
  for (int i = 0; i < 4; ++i) {
    pushers.push_back(std::make_unique<bc::test::TsPusher>(clip, "srt://127.0.0.1:" + std::to_string(47911 + i) + "?mode=caller", true));
  }
  CaseGuard guard;  // an abort anywhere below (REQUIRE(allLive())) still stops the pushers and the engine
  guard.defer([&] {
    for (auto& p : pushers) p->stop();
  });
  guard.defer([&] { engine.stop(); });

  // Warm up: every camera live.
  auto warm = Clock::now() + 20s;
  auto allLive = [&] {
    for (int i = 0; i < 4; ++i) if (phase(state, "cam" + std::to_string(i)) != "live") return false;
    return true;
  };
  while (!allLive() && Clock::now() < warm) std::this_thread::sleep_for(100ms);
  REQUIRE(allLive());
  std::this_thread::sleep_for(3s);  // let latency/audio settle

  // 20 s under full load.
  const uint64_t overruns0 = engine.stats().overruns;
  const auto t0 = Clock::now();
  std::this_thread::sleep_for(20s);
  const auto t1 = Clock::now();
  const auto st = engine.stats();
  CHECK(st.overruns - overruns0 == 0);
  for (int i = 0; i < 4; ++i) {
    const auto id = "cam" + std::to_string(i);
    auto* out = factory.fake.output("out" + std::to_string(i));
    const int n = countSince(out, t0, t1);
    MESSAGE(id << ": " << n << " frames in 20 s, latency " << st.cameras.at(id).latencyMs << " ms, fps " << st.cameras.at(id).fps);
    CHECK(n >= 1180);  // 60 fps × 20 s, ≤ 20 lost
    CHECK(st.cameras.at(id).latencyMs <= 50.0);
    CHECK(phase(state, id) == "live");
  }

  // Pull camera 0's source: hold, then slate; the others keep going.
  pushers[0]->stop();
  const auto cut = Clock::now();
  bool sawHolding = false;
  while (Clock::now() < cut + 5s) {
    const auto p = phase(state, "cam0");
    sawHolding = sawHolding || p == "holding";
    if (p == "slate") break;
    std::this_thread::sleep_for(20ms);
  }
  CHECK(sawHolding);
  CHECK(phase(state, "cam0") == "slate");
  const auto t2 = Clock::now();
  CHECK(countSince(factory.fake.output("out0"), cut, t2) >= static_cast<int>((t2 - cut) / 1ms / 17) - 10);  // never stopped
  CHECK(countSince(factory.fake.output("out1"), cut, t2) >= static_cast<int>((t2 - cut) / 1ms / 17) - 10);

  // The source comes back: live again.
  pushers[0] = std::make_unique<bc::test::TsPusher>(clip, "srt://127.0.0.1:47911?mode=caller", true);
  const auto back = Clock::now() + 15s;
  while (phase(state, "cam0") != "live" && Clock::now() < back) std::this_thread::sleep_for(100ms);
  CHECK(phase(state, "cam0") == "live");
}  // pushers and engine stopped by the guard above

TEST_CASE("M1 acceptance: a killed engine is back with its show in under 3 s") {
  bc::test::TempDir dir;
  {
    bc::ShowStore store(dir.path / "shows");
    bc::Show show;
    bc::CameraConfig c;
    c.id = "keepme01";
    c.name = "Kept";
    c.input.params = {{"mode", "listener"}, {"port", 47920}};
    show.cameras.push_back(c);
    store.saveNow(show);
  }
  const std::string token = "e2etoken0123456789ab";
  const int port = 47921;
  bc::ProcessLauncher launcher(exeDir() / "breadcamera-engine.exe",
                               L"--port " + std::to_wstring(port) + L" --token " + std::wstring(token.begin(), token.end()) +
                                   L" --show-dir \"" + (dir.path / "shows").wstring() + L"\" --discovery \"" +
                                   (dir.path / "engine.json").wstring() + L"\"");
  bc::Supervisor sup(launcher);
  std::atomic<bool> run{true};
  std::thread poller([&] {
    while (run) {
      sup.poll(Clock::now());
      std::this_thread::sleep_for(100ms);
    }
  });
  // Guard after the resources it tears down: an abort below (REQUIRE(first)/REQUIRE(again))
  // still stops the poller, the supervisor and the engine process — never a joinable thread
  // destroyed while unwinding, never an orphaned engine.
  CaseGuard guard;
  guard.defer([&] { sup.stopSupervising(); });
  guard.defer([&] {
    run = false;
    if (poller.joinable()) poller.join();
  });
  guard.defer([&] { launcher.kill(); });

  ix::initNetSystem();
  auto askShow = [&]() -> std::optional<json> {  // connects, asks getShow, returns the result or nothing
    ix::WebSocket ws;
    ws.setUrl("ws://127.0.0.1:" + std::to_string(port) + "/?token=" + token);
    ws.disableAutomaticReconnection();
    std::mutex m;
    std::optional<json> result;
    std::atomic<bool> open{false};
    ws.setOnMessageCallback([&](const ix::WebSocketMessagePtr& msg) {
      if (msg->type == ix::WebSocketMessageType::Open) open = true;
      if (msg->type != ix::WebSocketMessageType::Message) return;
      auto j = json::parse(msg->str, nullptr, false);
      if (j.is_object() && j.value("id", json()) == 1 && j.value("ok", false)) {
        std::lock_guard l(m);
        result = j["result"];
      }
    });
    ws.start();
    for (int i = 0; i < 20 && !open; ++i) std::this_thread::sleep_for(25ms);
    if (open) ws.send(R"({"id":1,"cmd":"getShow"})");
    for (int i = 0; i < 20; ++i) {
      {
        std::lock_guard l(m);
        if (result) break;
      }
      std::this_thread::sleep_for(25ms);
    }
    ws.stop();
    std::lock_guard l(m);
    return result;
  };

  auto until = Clock::now() + 10s;
  std::optional<json> first;
  while (!(first = askShow()) && Clock::now() < until) {}
  REQUIRE(first);
  CHECK((*first)["cameras"][0]["id"] == "keepme01");

  launcher.kill();  // simulated crash
  const auto killed = Clock::now();
  std::optional<json> again;
  while (!(again = askShow()) && Clock::now() < killed + 10s) {}
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - killed);
  MESSAGE("engine back after " << took.count() << " ms");
  REQUIRE(again);
  CHECK(took < 3000ms);
  CHECK((*again)["cameras"][0]["id"] == "keepme01");
}  // poller, supervisor and engine process stopped by the guard above

TEST_CASE("M1 case guard tears down every resource exactly once when a case aborts") {
  // Seam for the two acceptance cases above (review batch 6, finding 1): a REQUIRE-thrown
  // abort must still stop the poller thread, the supervisor and the launcher (case 2), and
  // the pushers and the engine (case 1) — a joinable thread destroyed while unwinding would
  // std::terminate the whole suite. Force an early exit through the guard and assert every
  // teardown step ran, exactly once.
  struct CountingLauncher : bc::EngineLauncher {
    std::atomic<int> launches{0};
    std::atomic<int> kills{0};
    std::atomic<bool> running{false};
    bool isRunning() override { return running; }
    void launch() override {
      ++launches;
      running = true;
    }
    void kill() override {
      ++kills;
      running = false;
    }
  } launcher;
  bc::Supervisor sup(launcher);
  std::atomic<bool> run{true};
  std::thread poller([&] {
    while (run) {
      sup.poll(Clock::now());
      std::this_thread::sleep_for(10ms);
    }
  });
  int pushersStopped = 0;
  int engineStopped = 0;

  bool aborted = false;
  try {
    {  // the case body: the guard is declared after the resources it must tear down
      CaseGuard guard;
      guard.defer([&] { sup.stopSupervising(); });
      guard.defer([&] {
        run = false;
        if (poller.joinable()) poller.join();
      });
      guard.defer([&] { launcher.kill(); });
      guard.defer([&] { ++pushersStopped; });  // stands in for: for (auto& p : pushers) p->stop();
      guard.defer([&] { ++engineStopped; });   // stands in for: engine.stop();
      throw std::runtime_error("simulated REQUIRE failure");  // the early exit through the guard
    }
  } catch (const std::runtime_error&) {
    aborted = true;
  }
  CHECK(aborted);
  CHECK(launcher.kills == 1);        // kill() ran, exactly once
  CHECK(pushersStopped == 1);        // pusher teardown ran, exactly once
  CHECK(engineStopped == 1);         // engine teardown ran, exactly once
  const int launchesAfter = sup.launches();
  sup.poll(Clock::now() + 1h);       // stopSupervising ran: a late poll must launch nothing
  CHECK(sup.launches() == launchesAfter);
  const bool joinedByGuard = !poller.joinable();
  if (poller.joinable()) {           // never leave a joinable thread behind, even on failure
    run = false;
    poller.join();
  }
  CHECK(joinedByGuard);
}
