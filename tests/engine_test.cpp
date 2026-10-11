#include <doctest/doctest.h>

#include <set>
#include <thread>

#include "core/state_store.h"
#include "engine/command_router.h"
#include "engine/engine.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "support/fake_input.h"

using namespace std::chrono_literals;
using nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {
struct Rig {
  std::unique_ptr<bc::GpuDevice> gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};
  bc::StateStore state;
  bc::test::FakeFactory factory{conv};
  bc::Engine engine{conv, comp, state, factory};
  ~Rig() { engine.stop(); }
};

bc::CameraConfig cam(const std::string& id, std::vector<bc::OutputConfig> outs, json inParams = json::object()) {
  bc::CameraConfig c;
  c.id = id;
  c.name = "Cam " + id;
  c.input.kind = "fake";
  c.input.params = inParams;
  if (!c.input.params.contains("tag")) c.input.params["tag"] = id;
  c.outputs = std::move(outs);
  return c;
}

bc::OutputConfig out(const std::string& id, json params = json::object(), const std::string& kind = "fake") {
  return {id, kind, false, true, std::move(params)};
}

// Small show format so the tests stay cheap; the engine fits every frame to it.
bc::Show testShow() {
  bc::Show s;
  s.settings.width = 64;
  s.settings.height = 36;
  return s;
}

int countBetween(const std::vector<bc::test::FakeOutput::Seen>& seen, Clock::time_point a, Clock::time_point b) {
  int n = 0;
  for (auto& s : seen) if (s.at >= a && s.at < b) ++n;
  return n;
}

std::chrono::milliseconds maxGap(const std::vector<bc::test::FakeOutput::Seen>& seen) {
  std::chrono::milliseconds g{0};
  for (size_t i = 1; i < seen.size(); ++i) {
    g = std::max(g, std::chrono::duration_cast<std::chrono::milliseconds>(seen[i].at - seen[i - 1].at));
  }
  return g;
}

std::string phaseOf(bc::StateStore& s, const std::string& id) {
  auto snap = s.snapshot();
  auto p = json::json_pointer("/cameras/" + id + "/phase");
  return snap.contains(p) ? snap.at(p).get<std::string>() : "";
}
}  // namespace

TEST_CASE("Engine: a 30 fps input becomes a steady 60 fps output") {
  Rig r;
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("o1")}, {{"fps", 30}, {"bgra", {10, 20, 200, 255}}}));
  r.engine.apply(show);
  r.engine.start();
  auto t0 = Clock::now();
  std::this_thread::sleep_for(1600ms);
  auto seen = r.factory.output("o1")->seen();
  int n = countBetween(seen, t0 + 500ms, t0 + 1500ms);
  CHECK(n >= 55);
  CHECK(n <= 62);
  CHECK(maxGap(seen) < 50ms);
  auto& last = seen.back();
  CHECK(std::abs(last.firstPixel[0] - 10) <= 2);
  CHECK(std::abs(last.firstPixel[2] - 200) <= 2);
  CHECK(r.engine.stats().cameras.at("a").repeats > 0);
}

TEST_CASE("Engine: each tick carries exactly its share of audio") {
  Rig r;
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("o1")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  for (auto& s : r.factory.output("o1")->seen()) CHECK(s.audioSamples == 800 * 2);
}

TEST_CASE("Engine: a stalled input holds, then shows the slate; outputs never stop") {
  Rig r;
  bc::Show show = testShow();
  show.settings.holdMs = 500;
  show.cameras.push_back(cam("a", {out("o1")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  r.factory.input("a")->paused = true;
  std::set<std::string> phases;
  for (int i = 0; i < 75; ++i) {  // 1.5 s
    phases.insert(phaseOf(r.state, "a"));
    std::this_thread::sleep_for(20ms);
  }
  CHECK(phases.count("holding") == 1);
  CHECK(phaseOf(r.state, "a") == "slate");
  auto seen = r.factory.output("o1")->seen();
  CHECK(maxGap(seen) < 50ms);
  CHECK(std::abs(seen.back().firstPixel[0] - 79) <= 4);  // blue: 0.06 linear
  CHECK(std::abs(seen.back().firstPixel[2] - 73) <= 4);  // red: 0.05 linear
  r.factory.input("a")->paused = false;
  std::this_thread::sleep_for(500ms);
  CHECK(phaseOf(r.state, "a") == "live");
}

TEST_CASE("Engine: a slow output does not slow its neighbour") {
  Rig r;
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("slow", {{"delayMs", 200}}), out("fast")}));
  r.engine.apply(show);
  r.engine.start();
  auto t0 = Clock::now();
  std::this_thread::sleep_for(1600ms);
  CHECK(countBetween(r.factory.output("fast")->seen(), t0 + 500ms, t0 + 1500ms) >= 55);
  CHECK(r.factory.output("slow")->status().dropped > 0);
}

TEST_CASE("Engine: apply adds, reconfigures and removes cameras") {
  Rig r;
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("o1")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  auto* o1 = r.factory.output("o1");
  show.cameras[0].outputs = {out("o2")};
  show.cameras.push_back(cam("b", {out("o3")}));
  r.engine.apply(show);
  std::this_thread::sleep_for(300ms);
  CHECK(r.factory.output("o2")->delivered > 0);
  CHECK(r.factory.output("o3")->delivered > 0);
  auto snap = r.state.snapshot();
  CHECK_FALSE(snap["cameras"]["a"]["outputs"].contains("o1"));
  CHECK(snap["cameras"]["b"]["name"] == "Cam b");
  (void)o1;  // destroyed by the reconfigure; must not be touched
  r.engine.apply(bc::Show{});
  CHECK(r.state.snapshot()["cameras"].empty());
}

TEST_CASE("Engine: an input changing size mid-stream still leaves at the show size, without a gap") {
  Rig r;
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("o1")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  r.factory.input("a")->width = 32;
  r.factory.input("a")->height = 18;
  std::this_thread::sleep_for(500ms);
  auto seen = r.factory.output("o1")->seen();
  REQUIRE_FALSE(seen.empty());
  CHECK(seen.front().width == 64);
  CHECK(seen.back().width == 64);
  CHECK(maxGap(seen) < 50ms);
  // Review Focus #2: the switch itself must not crash the tick or wedge the frame.
  CHECK_FALSE(r.state.snapshot()["cameras"]["a"].contains("tickError"));
  CHECK(seen.back().firstPixel[2] > 100);  // still the picture, never an all-zero frame
}

TEST_CASE("Engine: tickError is set by a throwing tick and cleared by the next clean tick") {
  Rig r;
  bc::Show show = testShow();
  auto broken = out("o1");
  broken.width = 32768;  // over the D3D11 texture cap: every tick's variant() throws
  show.cameras.push_back(cam("a", {broken}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  auto tickError = [&] {
    auto snap = r.state.snapshot();
    auto p = json::json_pointer("/cameras/a/tickError");
    return snap.contains(p) ? snap.at(p).get<std::string>() : std::string{};
  };
  CHECK_FALSE(tickError().empty());  // the engine reports the throwing tick
  // Heal the same camera: a deliverable output size again, next tick is clean.
  show.cameras[0].outputs = {out("o1")};
  r.engine.apply(show);
  std::this_thread::sleep_for(300ms);
  CHECK(tickError().empty());  // a clean tick clears it; it must not stick forever
}

TEST_CASE("Engine: a missing slate image falls back to the built-in slate with a warning") {
  Rig r;
  bc::Show show = testShow();
  show.settings.slatePath = "C:/definitely/not/here.png";
  show.cameras.push_back(cam("a", {out("o1")}, {{"paused", true}}));  // never any frame
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  auto snap = r.state.snapshot();
  CHECK_FALSE(snap["engine"]["warnings"]["slate"].get<std::string>().empty());
  auto seen = r.factory.output("o1")->seen();
  REQUIRE_FALSE(seen.empty());
  CHECK(std::abs(seen.back().firstPixel[2] - 73) <= 4);
}

TEST_CASE("Engine: a bad output config is reported and the camera keeps running") {
  Rig r;
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("bad", {}, "nope"), out("good")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1200ms);  // at least one 1 Hz status publish
  auto snap = r.state.snapshot();
  CHECK(snap["cameras"]["a"]["outputs"]["bad"]["state"] == "offline");
  CHECK(snap["cameras"]["a"]["outputs"]["bad"]["error"]["code"] == "output-config");
  CHECK(snap["cameras"]["a"]["outputs"]["good"]["state"] == "live");
  CHECK(r.factory.output("good")->delivered > 30);
}

TEST_CASE("Engine: an output can send a smaller size, and a half-rate output gets every second tick with all the audio") {
  Rig r;
  bc::Show show = testShow();
  auto smallOut = out("small");
  smallOut.width = 32;
  smallOut.height = 18;
  auto half = out("half");
  half.halfRate = true;
  show.cameras.push_back(cam("a", {out("full"), smallOut, half}));
  r.engine.apply(show);
  r.engine.start();
  auto t0 = Clock::now();
  std::this_thread::sleep_for(1600ms);
  CHECK(r.factory.output("full")->seen().back().width == 64);
  CHECK(r.factory.output("small")->seen().back().width == 32);
  auto halfSeen = r.factory.output("half")->seen();
  const int n = countBetween(halfSeen, t0 + 500ms, t0 + 1500ms);
  CHECK(n >= 27);
  CHECK(n <= 31);
  for (size_t i = 1; i < halfSeen.size(); ++i) CHECK(halfSeen[i].audioSamples == 2 * 800 * 2);
}

TEST_CASE("Engine: input health is published") {
  Rig r;
  bc::Show show = testShow();
  show.settings.holdMs = 3000;
  show.cameras.push_back(cam("a", {out("o1")}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(1200ms);
  auto health = [&] { return r.state.snapshot()["cameras"]["a"]["input"]["health"].get<std::string>(); };
  CHECK(health() == "receiving");
  r.factory.input("a")->paused = true;
  std::this_thread::sleep_for(1200ms);
  CHECK(health() == "down");
  r.factory.input("a")->paused = false;
  std::this_thread::sleep_for(1200ms);
  CHECK(health() == "degraded");  // the pause was a stutter less than 10 s ago
  CHECK(r.state.snapshot()["engine"]["resolution"] == "64x36");
}

TEST_CASE("Factory: a non-string NDI name is rejected with the router's message") {
  Rig r;
  bc::DefaultAdapterFactory factory(*r.gpu, r.pool);
  bc::CameraConfig c;
  c.id = "a";
  c.name = "Cam a";
  bc::OutputConfig o;
  o.id = "ndi1";
  o.kind = "ndi";
  o.params["name"] = 7;  // hand-edited show file: name is not text
  // Both validation sites must reject the same value with the same words.
  CHECK_THROWS_WITH_AS(bc::validateOutput(o), "NDI name must be text", std::invalid_argument);
  CHECK_THROWS_WITH_AS(factory.makeOutput(o, c), "NDI name must be text", std::invalid_argument);
}
