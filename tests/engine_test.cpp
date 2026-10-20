#include <doctest/doctest.h>

#include <set>
#include <thread>

#include "core/state_store.h"
#include "engine/command_router.h"
#include "engine/engine.h"
#include "engine/gpu_tools.h"
#include "gpu/color_grader.h"
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
  bc::ColorGrader grader{*gpu, pool};
  bc::StateStore state;
  bc::test::FakeFactory factory{conv};
  bc::Engine engine{bc::GpuTools{conv, comp, grader}, state, factory};
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

// No show-wide frame size any more: every camera's frame is its input's size. The fake input
// is 64x36 by default, so the tests stay cheap.
bc::Show testShow() {
  bc::Show s;
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

TEST_CASE("Engine: a 30 fps input stays 30 fps at any Project rate") {
  for (const auto& project : {bc::Rate{15, 1}, bc::Rate{60, 1}}) {
    Rig r;
    bc::Show show = testShow();
    show.settings.rate = project;
    show.cameras.push_back(cam("a", {out("o1")}, {{"fps", 30}, {"bgra", {10, 20, 200, 255}}}));
    r.engine.apply(show);
    r.engine.start();
    auto t0 = Clock::now();
    std::this_thread::sleep_for(1500ms);
    auto seen = r.factory.output("o1")->seen();
    int n = countBetween(seen, t0 + 500ms, t0 + 1500ms);
    CHECK(n >= 27);
    CHECK(n <= 33);
    CHECK(maxGap(seen) < 60ms);
    CHECK(r.engine.stats().cameras.at("a").repeats == 0);
  }
}

TEST_CASE("Engine: no output frame before the first input frame") {
  Rig r;
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("o1")}, {{"paused", true}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(300ms);
  CHECK(r.factory.output("o1")->seen().empty());
  CHECK(phaseOf(r.state, "a") == "nosignal");
  r.factory.input("a")->paused = false;
  std::this_thread::sleep_for(300ms);
  CHECK_FALSE(r.factory.output("o1")->seen().empty());
}

TEST_CASE("Engine: each emitted frame carries one frame of audio at the input rate") {
  Rig r;
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("o1")}, {{"fps", 30}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  for (auto& s : r.factory.output("o1")->seen()) CHECK(s.audioSamples == 1600 * 2);
}

TEST_CASE("Engine: a stalled input holds, then shows the slate; outputs never stop") {
  Rig r;
  bc::Show show = testShow();
  show.settings.holdMs = 500;
  show.cameras.push_back(cam("a", {out("o1")}, {{"fps", 30}}));
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
  CHECK(maxGap(seen) < 300ms);  // live stops, then the first held repeat starts at stallAfter
  REQUIRE(seen.size() > 2);
  CHECK(seen.back().at - seen[seen.size() - 2].at < 60ms);  // repeats run at the camera rate
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
  CHECK(countBetween(r.factory.output("fast")->seen(), t0 + 500ms, t0 + 1500ms) >= 25);
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

TEST_CASE("Camera: the frame keeps the input's size") {
  Rig r;
  bc::Show show;  // no show-wide size: the frame is the input's size
  show.cameras.push_back(cam("a", {out("o1")}, {{"width", 640}, {"height", 360}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  auto seen = r.factory.output("o1")->seen();
  REQUIRE_FALSE(seen.empty());
  CHECK(seen.back().width == 640);  // the frame is the input's size, not a show size
  REQUIRE(r.factory.output("o1")->lastImage() != nullptr);
  CHECK(r.factory.output("o1")->lastImage()->height == 360);

  // A mid-show input size change moves the camera's frame size with it, with no gap.
  r.factory.input("a")->width = 320;
  r.factory.input("a")->height = 180;
  std::this_thread::sleep_for(500ms);
  seen = r.factory.output("o1")->seen();
  CHECK(seen.back().width == 320);
  CHECK(maxGap(seen) < 50ms);
  // Review Focus #2: the switch itself must not crash the tick or wedge the frame.
  CHECK_FALSE(r.state.snapshot()["cameras"]["a"].contains("tickError"));
}

namespace {
// An output that throws on submit while armed, to drive the engine's tickError path. (The old
// trigger — a per-output size over the D3D11 texture cap — is gone with the per-output size.)
class ThrowingOutput final : public bc::Output {
 public:
  std::atomic<bool> armed{true};
  bool needsCpu() const override { return false; }
  void start() override {}
  void submit(bc::OutFrame) override {
    if (armed) throw std::runtime_error("boom");
  }
  void stop() override {}
  bc::OutputStatus status() const override { return {}; }
};

class ThrowingFactory final : public bc::AdapterFactory {
 public:
  explicit ThrowingFactory(bc::test::FakeFactory& real) : real_(real) {}
  std::unique_ptr<bc::Input> makeInput(const bc::InputConfig& cfg) override { return real_.makeInput(cfg); }
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig&, const bc::CameraConfig&) override {
    auto o = std::make_unique<ThrowingOutput>();
    out = o.get();
    return o;
  }
  ThrowingOutput* out = nullptr;

 private:
  bc::test::FakeFactory& real_;
};
}  // namespace

TEST_CASE("Engine: tickError is set by a throwing tick and cleared by the next clean tick") {
  Rig r;
  ThrowingFactory factory{r.factory};
  bc::Engine engine{bc::GpuTools{r.conv, r.comp, r.grader}, r.state, factory};
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("o1")}));
  engine.apply(show);
  engine.start();
  std::this_thread::sleep_for(300ms);
  auto tickError = [&] {
    auto snap = r.state.snapshot();
    auto p = json::json_pointer("/cameras/a/tickError");
    return snap.contains(p) ? snap.at(p).get<std::string>() : std::string{};
  };
  CHECK_FALSE(tickError().empty());  // the engine reports the throwing tick
  // Heal the same camera: a clean tick clears the error.
  REQUIRE(factory.out != nullptr);
  factory.out->armed = false;
  std::this_thread::sleep_for(300ms);
  CHECK(tickError().empty());  // a clean tick clears it; it must not stick forever
  engine.stop();
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
  CHECK(seen.empty());  // D16: a camera with no input frame sends no picture, not a slate
  CHECK(snap["cameras"]["a"]["phase"] == "nosignal");
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

TEST_CASE("Engine: every output sends the one frame size, and a half-rate output gets every second tick with all the audio") {
  Rig r;
  bc::Show show = testShow();
  auto half = out("half");
  half.halfRate = true;
  show.cameras.push_back(cam("a", {out("full"), half}));
  r.engine.apply(show);
  r.engine.start();
  auto t0 = Clock::now();
  std::this_thread::sleep_for(1600ms);
  CHECK(r.factory.output("full")->seen().back().width == 64);  // one frame size: the input's size
  auto halfSeen = r.factory.output("half")->seen();
  const int n = countBetween(halfSeen, t0 + 500ms, t0 + 1500ms);
  CHECK(n >= 13);
  CHECK(n <= 17);
  for (size_t i = 1; i < halfSeen.size(); ++i) CHECK(halfSeen[i].audioSamples == 2 * 1600 * 2);
}

TEST_CASE("Camera: a coalesced frame advances the emitted index across the dropped frames") {
  CHECK(bc::Camera::advanceIndex(0, 0, 1, true) == 1);
  CHECK(bc::Camera::advanceIndex(1, 1, 5, true) == 5);
  CHECK(bc::Camera::advanceIndex(5, 5, 9, true) == 9);
  CHECK(bc::Camera::advanceIndex(9, 9, 9, false) == 10);
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
  CHECK_FALSE(r.state.snapshot()["engine"].contains("resolution"));  // no show-wide size
}

// bcam-readback Task 2: the frame readback happens off the tick. A CPU-consuming output is
// handed the newest *completed* readback (one tick behind at most), never a black/synthetic
// frame — on the first ticks it is simply handed none.
//
// A synchronous output records the frame the camera hands it *on the tick thread*, so a test
// can assert exactly what was submitted this tick (FakeOutput's worker thread is racy here).
namespace {
class RecordingOutput final : public bc::Output {
 public:
  explicit RecordingOutput(bool needsCpu) : needsCpu_(needsCpu) {}
  bool needsCpu() const override { return needsCpu_; }
  void start() override {}
  void submit(bc::OutFrame f) override {
    last = std::move(f.bgra);
    ++submitted;
  }
  void stop() override {}
  bc::OutputStatus status() const override { return {}; }
  std::shared_ptr<const bc::CpuImage> last;
  int submitted = 0;

 private:
  bool needsCpu_;
};

class NoInput final : public bc::Input {
 public:
  void start(bc::InputSink&) override {}
  void stop() override {}
};

class RecordingFactory final : public bc::AdapterFactory {
 public:
  explicit RecordingFactory(bool needsCpu) : needsCpu_(needsCpu) {}
  std::unique_ptr<bc::Input> makeInput(const bc::InputConfig&) override { return std::make_unique<NoInput>(); }
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig&, const bc::CameraConfig&) override {
    auto o = std::make_unique<RecordingOutput>(needsCpu_);
    made = o.get();
    all.push_back(o.get());
    return o;
  }
  RecordingOutput* made = nullptr;
  std::vector<RecordingOutput*> all;  // in creation (show output) order

 private:
  bool needsCpu_;
};

// A solid BGRA frame of one colour, to push straight into a Camera (no input thread).
bc::VideoFrameIn solidFrame(bc::Converter& conv, uint8_t red, uint32_t w = 64, uint32_t h = 36) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (size_t p = 0; p < px.size(); p += 4) {
    px[p + 0] = 0;
    px[p + 1] = 0;
    px[p + 2] = red;
    px[p + 3] = 255;
  }
  return bc::VideoFrameIn{conv.uploadBgra8(px.data(), w, h, w * 4), bc::PixelLayout::BGRA8, true, 0};
}
}  // namespace

TEST_CASE("Camera: the frame readback is off the tick, never black") {
  Rig r;
  RecordingFactory factory(/*needsCpu=*/true);
  bc::Camera camera(cam("a", {out("o1")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.start();
  REQUIRE(factory.made != nullptr);
  bc::TickSchedule sched{bc::Rate{60, 1}};
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});

  // Two colours: the first readback must deliver A, and later ticks must keep advancing to B
  // (the slot is not stuck on the first frame). A synchronous readback would hand tick 0 its
  // own frame, so the tick-0 check below is the one that fails before the async change.
  bool sawA = false, sawB = false;
  for (uint64_t i = 0; i < 160; ++i) {
    const uint8_t red = i < 40 ? 60 : 220;
    camera.onVideo(solidFrame(r.conv, red));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
    if (i == 0) CHECK(factory.made->last == nullptr);  // the copy begins this tick; nothing done yet
    if (factory.made->last) {
      const int got = factory.made->last->bgra[2];
      CHECK(got > 10);  // never a black/synthetic frame
      if (!sawA && std::abs(got - 60) <= 4) {
        CHECK(factory.made->last->width == 64);
        CHECK(factory.made->last->height == 36);
        CHECK(factory.made->last->bgra.size() == 64u * 36u * 4u);
        sawA = true;
      }
      if (std::abs(got - 220) <= 4) sawB = true;
    }
    std::this_thread::sleep_for(3ms);
  }
  CHECK(sawA);  // the first readback completed with the right pixels
  CHECK(sawB);  // and the readback keeps up: later frames are delivered too
  camera.stop();
}

TEST_CASE("Camera: no CPU output means no readback slot and no frame copy") {
  Rig r;
  RecordingFactory factory(/*needsCpu=*/false);
  bc::Camera camera(cam("a", {out("o1")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.start();
  REQUIRE(factory.made != nullptr);
  bc::TickSchedule sched{bc::Rate{60, 1}};
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  for (uint64_t i = 0; i < 10; ++i) {
    camera.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
  }
  CHECK(camera.readbackSlotCount() == 0);
  CHECK(factory.made->last == nullptr);  // a non-CPU output is never handed a CPU image
  camera.stop();
}

TEST_CASE("Camera: readback slots appear and are dropped with the output set") {
  Rig r;
  RecordingFactory factory(/*needsCpu=*/true);
  bc::CameraConfig cfg = cam("a", {out("o1")});
  bc::Camera camera(cfg, factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.start();
  bc::TickSchedule sched{bc::Rate{60, 1}};
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  auto tick = [&](uint64_t i) {
    camera.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
    std::this_thread::sleep_for(3ms);
  };
  tick(0);
  CHECK(camera.readbackSlotCount() == 1);

  // Reconfigure the output set: still one slot (one frame size per camera).
  cfg.outputs = {out("o2")};
  camera.reconfigure(cfg);
  tick(1);
  CHECK(camera.readbackSlotCount() == 1);

  // No CPU output left: the slot is dropped, so a GPU-only camera holds nothing.
  cfg.outputs = {};
  camera.reconfigure(cfg);
  tick(2);
  CHECK(camera.readbackSlotCount() == 0);
  camera.stop();
}

// Two CPU-consuming outputs at the same size must share ONE readback per tick (spec §4.3): the
// readback is finished/begun once and both outputs are handed the same CpuImage. Without the
// per-variant handled flag the second output re-enters the readback block, so the shared slot can
// advance mid-tick and the two outputs can be handed frames one tick apart.
TEST_CASE("Camera: two CPU outputs of one size share one readback a tick") {
  Rig r;
  RecordingFactory factory(/*needsCpu=*/true);
  bc::CameraConfig cfg = cam("a", {out("o1"), out("o2")});  // both CPU, both the input's size
  bc::Camera camera(cfg, factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.start();
  REQUIRE(factory.all.size() == 2);
  RecordingOutput* a = factory.all[0];
  RecordingOutput* b = factory.all[1];
  bc::TickSchedule sched{bc::Rate{60, 1}};
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});

  bool sawImage = false;
  for (uint64_t i = 0; i < 120; ++i) {
    camera.onVideo(solidFrame(r.conv, static_cast<uint8_t>(30 + (i % 5) * 40)));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
    CHECK(a->last == b->last);  // one readback per size per tick: the same image, never a tick apart
    if (a->last) sawImage = true;
    std::this_thread::sleep_for(3ms);
  }
  CHECK(sawImage);
  CHECK(camera.readbackSlotCount() == 1);  // still one slot, not one per output
  camera.stop();
}

// One frame size per camera: when the input changes size mid-show the one readback slot still holds
// the previous size's completed image. That stale image must not be published for a tick while the
// first new-size copy is in flight (spec §4.3).
TEST_CASE("Camera: an input size change never publishes the previous size's image") {
  Rig r;
  RecordingFactory factory(/*needsCpu=*/true);
  bc::Camera camera(cam("a", {out("o1")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.start();
  REQUIRE(factory.made != nullptr);
  bc::TickSchedule sched{bc::Rate{60, 1}};
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  auto tick = [&](uint64_t i, uint32_t w, uint32_t h) {
    camera.onVideo(solidFrame(r.conv, 120, w, h));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
  };

  // Run until the 64x36 image has been read back and handed to the output.
  bool sawOld = false;
  uint64_t i = 0;
  for (; i < 60 && !sawOld; ++i) {
    tick(i, 64, 36);
    if (factory.made->last && factory.made->last->width == 64 && factory.made->last->height == 36) sawOld = true;
    std::this_thread::sleep_for(3ms);
  }
  REQUIRE(sawOld);

  // The input changes size. The cached 64x36 image must not be published again: the first tick
  // after the change is handed nothing, not the old size.
  tick(i, 32, 18);
  CHECK(factory.made->last == nullptr);
  std::this_thread::sleep_for(3ms);

  // Later ticks may deliver the new-size image, never an old-size one.
  for (++i; i < 90; ++i) {
    tick(i, 32, 18);
    if (factory.made->last) {
      CHECK(factory.made->last->width == 32);
      CHECK(factory.made->last->height == 18);
    }
    std::this_thread::sleep_for(3ms);
  }
  camera.stop();
}

// The colour stage can be bypassed across an input renegotiation (guard backoff), so the graded
// texture left over from the previous size must not be reused once the input changes size: on a
// repeat tick the old-size graded frame would otherwise be published. The removed Camera::setFormat
// reset lastGraded_ on every size change; the reuse branch must now check the size itself.
TEST_CASE("Camera: a size change never reuses the previous size's graded frame") {
  Rig r;
  RecordingFactory factory(/*needsCpu=*/false);
  bc::CameraConfig cfg = cam("a", {out("o1")});
  cfg.stages = {{"color", bc::normalizeStageParams("color", {{"enabled", true}, {"saturation", 0.5}})}};
  // A long hold keeps the renegotiated frame in the sync's Holding window, so the repeat tick
  // below still outputs it (a Slate phase would drop it and hide the bug).
  bc::SyncPolicy policy;
  policy.hold = 60000ms;
  bc::Camera camera(cfg, factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, policy);
  camera.start();
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});

  // 1. A fresh 64x36 frame is graded: lastGraded_ is 64x36 and gradeDirty_ clears.
  camera.onVideo(solidFrame(r.conv, 120, 64, 36));
  const auto t0 = Clock::now();
  camera.beginTick(t0, slate);
  REQUIRE(camera.tickFrame()->width == 64);
  REQUIRE(camera.tickFrame()->height == 36);

  // 2. The colour stage bypasses (as on a failure) and will retry 5 s later.
  camera.colorGuard().failed("test: force colour bypass", t0);

  // 3. The input renegotiates to 32x18 while the colour stage is bypassed: the new frame is
  //    converted (lastSize_ moves to 32x18) but the 64x36 graded texture is left in place.
  camera.onVideo(solidFrame(r.conv, 120, 32, 18));
  camera.beginTick(t0 + 1s, slate);

  // 4. The guard retries on a repeat tick (no fresh frame). It must grade the 32x18 frame; before
  //    the fix the reuse branch serves the stale 64x36 texture, so the tick publishes the old size.
  camera.beginTick(t0 + 6s, slate);
  CHECK(camera.tickFrame()->width == 32);
  CHECK(camera.tickFrame()->height == 18);
  camera.stop();
}

// D2: with no picture the camera shows the slate at its own frame size, not a show size. Before any
// frame is seen it uses the fallback (1280x720); after a frame it uses that frame's size.
TEST_CASE("Camera: with no picture the slate is fitted to the camera's own size") {
  Rig r;
  RecordingFactory factory(/*needsCpu=*/true);
  bc::Camera camera(cam("a", {out("o1")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.start();
  // The slate source is a different size from the camera, so a fit must happen.
  auto slateSource = r.conv.solid(320, 180, {0.05f, 0.05f, 0.06f, 1.0f});

  // No frame yet: the slate is fitted to the fallback 1280x720.
  camera.beginTick(Clock::now(), slateSource);
  CHECK(camera.tickFrame()->width == 1280);
  CHECK(camera.tickFrame()->height == 720);

  // A frame at 640x360 moves the camera's size; the slate is then fitted to it.
  camera.onVideo(solidFrame(r.conv, 120, 640, 360));
  camera.beginTick(Clock::now(), slateSource);
  CHECK(camera.tickFrame()->width == 640);
  CHECK(camera.tickFrame()->height == 360);
  camera.stop();
}

// D5: a camera whose only output is GPU-only (never needsCpu) must still run one readback when a
// visible preview wants its image, so the frame tap can hand the preview a CpuImage.
TEST_CASE("GPU-only camera: a visible preview forces one readback") {
  Rig r;
  RecordingFactory factory(/*needsCpu=*/false);
  bc::Camera camera(cam("a", {out("o1")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.start();
  REQUIRE(factory.made != nullptr);
  bc::TickSchedule sched{bc::Rate{60, 1}};
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  auto tick = [&](uint64_t i, bool previewWanted) {
    camera.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr, previewWanted);
    std::this_thread::sleep_for(3ms);
  };

  tick(0, /*previewWanted=*/false);
  CHECK(camera.readbackSlotCount() == 0);  // no CPU output, no preview: no readback work

  int images = 0;
  for (uint64_t i = 1; i <= 20; ++i) {
    tick(i, /*previewWanted=*/true);
    if (camera.tickCpu()) ++images;
  }
  CHECK(camera.readbackSlotCount() == 1);  // the visible preview forces the one readback (D5)
  CHECK(images > 0);                        // and the image really reached the tap
  CHECK(factory.made->last == nullptr);     // the GPU-only output is never handed a CPU image

  tick(21, /*previewWanted=*/false);
  CHECK(camera.readbackSlotCount() == 0);  // preview gone, still no CPU output: released
  camera.stop();
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

TEST_CASE("Factory: a breadcall input with a missing or non-integer slot is rejected clearly") {
  Rig r;
  bc::DefaultAdapterFactory factory(*r.gpu, r.pool);
  bc::InputConfig in;
  in.kind = "breadcall";
  in.params = {{"url", "wss://livekit.example.com"}, {"token", "eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJ4In0.sig"}};
  // A hand-edited show file with no slot must not surface nlohmann::out_of_range.
  CHECK_THROWS_WITH_AS(factory.makeInput(in), "BreadCall slot is required (a whole number)", std::invalid_argument);
  in.params["slot"] = "2";  // a string slot is not a whole number
  CHECK_THROWS_WITH_AS(factory.makeInput(in), "BreadCall slot is required (a whole number)", std::invalid_argument);
}

TEST_CASE("Factory: an omt output is built from its mode and name") {
  Rig r;
  bc::DefaultAdapterFactory factory(*r.gpu, r.pool);
  bc::CameraConfig c;
  c.id = "a";
  c.name = "Cam a";
  bc::OutputConfig o;
  o.id = "omt1";
  o.kind = "omt";
  o.params = {{"mode", "full"}, {"name", "Stage"}};
#if BC_WITH_OMT
  CHECK(factory.makeOutput(o, c) != nullptr);
  o.params["name"] = 7;  // hand-edited show file: name is not text
  CHECK_THROWS_WITH_AS(bc::validateOutput(o), "OMT name must be text", std::invalid_argument);
  CHECK_THROWS_WITH_AS(factory.makeOutput(o, c), "OMT name must be text", std::invalid_argument);
  o.params["name"] = "Stage";
  o.params["mode"] = 5;  // hand-edited show file: mode is not text
  CHECK_THROWS_WITH_AS(bc::validateOutput(o), "OMT mode must be text", std::invalid_argument);
  CHECK_THROWS_WITH_AS(factory.makeOutput(o, c), "OMT mode must be text", std::invalid_argument);
#else
  CHECK_THROWS_WITH_AS(factory.makeOutput(o, c), "OMT support is not built (BC_WITH_OMT=OFF)", std::invalid_argument);
#endif
}
