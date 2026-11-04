#include <doctest/doctest.h>

#include <atomic>
#include <set>
#include <thread>

#include "core/state_store.h"
#include "engine/command_router.h"
#include "engine/engine.h"
#include "engine/gpu_tools.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "omthx/omthx.h"
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

TEST_CASE("Engine: the output frame carries the camera rate, not the Project rate") {
  Rig r;
  bc::Show show = testShow();
  show.settings.rate = bc::Rate{15, 1};
  show.cameras.push_back(cam("a", {out("o1")}, {{"rateNum", 30000}, {"rateDen", 1001}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(700ms);
  auto seen = r.factory.output("o1")->seen();
  REQUIRE_FALSE(seen.empty());
  CHECK(r.factory.output("o1")->lastRate() == bc::Rate{30000, 1001});
}

TEST_CASE("Engine: a renegotiated input rate reaches the output") {
  Rig r;
  bc::Show show = testShow();
  show.cameras.push_back(cam("a", {out("o1")}, {{"rateNum", 30000}, {"rateDen", 1001}}));
  r.engine.apply(show);
  r.engine.start();
  std::this_thread::sleep_for(500ms);
  REQUIRE(r.factory.output("o1")->lastRate() == bc::Rate{30000, 1001});
  show.cameras[0].input.params["rateNum"] = 60000;
  show.cameras[0].input.params["rateDen"] = 1001;
  r.engine.apply(show);
  std::this_thread::sleep_for(500ms);
  CHECK(r.factory.output("o1")->lastRate() == bc::Rate{60000, 1001});
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
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig&, const bc::CameraConfig&, bc::Rate) override {
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
  for (size_t i = 1; i < halfSeen.size(); ++i) {
    CHECK(halfSeen[i].tick == halfSeen[i - 1].tick + 1);  // the half-rate stream's own clock
    CHECK(halfSeen[i].audioSamples == 2 * 1600 * 2);
  }
}

TEST_CASE("Camera: a coalesced frame stamps its wall-clock tick and covers the gaps") {
  CHECK(bc::Camera::stampTick(0, 0, 1, true) == 0);
  CHECK(bc::Camera::stampTick(1, 1, 5, true) == 4);
  CHECK(bc::Camera::stampTick(5, 5, 9, true) == 8);
  CHECK(bc::Camera::stampTick(9, 9, 9, false) == 9);
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
// What an output asks of its camera: a CPU image, or the frame as an R8G8B8A8 texture.
struct RecSpec {
  bool needsCpu;
  bool wantsGpu;
  bool wantsUyvy = false;
  int codedMode = -1;  // an omthx::Mode value this output takes coded frames for, or -1 for none
};

class RecordingOutput final : public bc::Output {
 public:
  explicit RecordingOutput(RecSpec spec) : spec_(spec) {}
  bool needsCpu() const override { return spec_.needsCpu; }
  bool wantsGpuRgba8() const override { return spec_.wantsGpu; }
  bool wantsUyvy(uint32_t, uint32_t) const override { return spec_.wantsUyvy; }
  bool takesCodedOmt(omthx::Mode mode) const override { return spec_.codedMode == static_cast<int>(mode); }
  bool takeKeyframeRequest() override { return keyframe.exchange(false); }
  void start() override {}
  void submit(bc::OutFrame f) override {
    if (f.coded) {
      ++coded;
      lastCoded = std::move(f.coded);
      codedSamples = f.audio ? f.audio->size() : 0;
      return;
    }
    last = std::move(f.bgra);
    lastUyvy = std::move(f.uyvy);
    lastRgba8 = std::move(f.rgba8);
    ++submitted;
  }
  void stop() override {}
  bc::OutputStatus status() const override { return {}; }
  std::atomic<bool> keyframe{false};
  std::shared_ptr<const bc::CpuImage> last;
  std::shared_ptr<const bc::CpuImage> lastUyvy;
  bc::TextureRef lastRgba8;
  int submitted = 0;
  int coded = 0;
  std::shared_ptr<const omthx::CodedFrame> lastCoded;
  size_t codedSamples = 0;

 private:
  RecSpec spec_;
};

class NoInput final : public bc::Input {
 public:
  void start(bc::InputSink&) override {}
  void stop() override {}
};

class RecordingFactory final : public bc::AdapterFactory {
 public:
  // Every output gets this spec.
  explicit RecordingFactory(bool needsCpu) : spec_{needsCpu, false} {}
  // One spec per output, in creation order (the last one repeats).
  explicit RecordingFactory(std::vector<RecSpec> specs) : specs_(std::move(specs)) {}
  std::unique_ptr<bc::Input> makeInput(const bc::InputConfig&) override { return std::make_unique<NoInput>(); }
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig&, const bc::CameraConfig&, bc::Rate) override {
    const RecSpec spec = all.size() < specs_.size() ? specs_[all.size()] : spec_;
    auto o = std::make_unique<RecordingOutput>(spec);
    made = o.get();
    all.push_back(o.get());
    return o;
  }
  RecordingOutput* made = nullptr;
  std::vector<RecordingOutput*> all;  // in creation (show output) order

 private:
  RecSpec spec_{true, false};
  std::vector<RecSpec> specs_;
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
  return bc::VideoFrameIn{conv.uploadBgra8(px.data(), w, h, w * 4), bc::PixelLayout::BGRA8, true, false, 0};
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

namespace {
std::shared_ptr<omthx::CodedFrame> dummyCoded(uint64_t seq = 0, omthx::Mode mode = omthx::Mode::Full) {
  auto c = std::make_shared<omthx::CodedFrame>();
  c->mode = mode;
  c->format = omthx::VideoFormat{64, 36, 30, 1, false};
  c->timestamp = static_cast<int64_t>(seq) * 10'000'000 / 30;
  return c;
}
}  // namespace

TEST_CASE("Camera: pass-through sends coded frames, not tick pictures") {
  Rig r;
  RecordingFactory factory(std::vector<RecSpec>{RecSpec{false, false, false, static_cast<int>(omthx::Mode::Full)}});
  bc::Camera camera(cam("a", {out("o1")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state,
                    bc::SyncPolicy{});
  camera.setPreviewQuery([] { return false; });
  camera.start();
  REQUIRE(factory.made != nullptr);
  camera.onState(bc::SignalState::Live, std::nullopt);
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  auto tick = [&] {
    camera.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
  };
  camera.onCodedOmt(dummyCoded(0));  // the source's mode is known from its first coded frame
  tick();
  tick();  // the first rebuild happens at the end of the previous tick
  CHECK(camera.wantsCodedOmt());
  CHECK_FALSE(camera.wantsGpuVideo());
  const int tickFrames = factory.made->submitted;
  auto c = dummyCoded(1);
  CHECK_FALSE(camera.onCodedOmt(c));
  CHECK(factory.made->coded == 1);
  CHECK(factory.made->lastCoded == c);
  CHECK(factory.made->submitted == tickFrames);  // the tick path sent nothing more
  camera.setPreviewQuery([] { return true; });
  CHECK(camera.wantsGpuVideo());  // a visible preview decodes
  camera.stop();
}

TEST_CASE("Camera: pass-through leaves a tick output beside it") {
  Rig r;
  RecordingFactory factory(
      std::vector<RecSpec>{RecSpec{false, false, false, static_cast<int>(omthx::Mode::Full)}, RecSpec{true, false}});
  bc::Camera camera(cam("a", {out("o1"), out("o2")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state,
                    bc::SyncPolicy{});
  camera.setPreviewQuery([] { return false; });
  camera.start();
  REQUIRE(factory.all.size() == 2);
  camera.onState(bc::SignalState::Live, std::nullopt);
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  camera.onCodedOmt(dummyCoded(0));  // the source's mode is known from its first coded frame
  for (int i = 0; i < 3; ++i) {
    camera.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
  }
  CHECK(camera.wantsCodedOmt());
  CHECK(camera.wantsGpuVideo());  // the CPU output still needs a picture
  const int codedTickFrames = factory.all[0]->submitted;
  CHECK(factory.all[1]->submitted > 0);
  camera.onCodedOmt(dummyCoded(1));
  CHECK(factory.all[0]->coded == 1);
  CHECK(factory.all[1]->coded == 0);
  CHECK(factory.all[0]->submitted == codedTickFrames);  // the coded output takes no tick frame
  camera.stop();
}

TEST_CASE("Camera: enabling a stage leaves pass-through") {
  Rig r;
  RecordingFactory factory(std::vector<RecSpec>{RecSpec{false, false, false, static_cast<int>(omthx::Mode::Full)}});
  bc::CameraConfig cfg = cam("a", {out("o1")});
  bc::Camera camera(cfg, factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.setPreviewQuery([] { return false; });
  camera.start();
  REQUIRE(factory.made != nullptr);
  camera.onState(bc::SignalState::Live, std::nullopt);
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  auto tick = [&] {
    camera.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
  };
  camera.onCodedOmt(dummyCoded(0));  // the source's mode is known from its first coded frame
  tick();
  tick();
  REQUIRE(camera.wantsCodedOmt());

  cfg.stages["color"] = {{"enabled", true}};
  camera.reconfigure(cfg);
  CHECK_FALSE(camera.wantsCodedOmt());
  const int coded = factory.made->coded;
  camera.onCodedOmt(dummyCoded(2));
  CHECK(factory.made->coded == coded);  // no longer a pass target

  const int submitted = factory.made->submitted;
  tick();
  CHECK(factory.made->submitted == submitted + 1);  // back on the tick path
  camera.stop();
}

TEST_CASE("Camera: a pass-through target asks for a keyframe once") {
  Rig r;
  RecordingFactory factory(std::vector<RecSpec>{RecSpec{false, false, false, static_cast<int>(omthx::Mode::Full)}});
  bc::Camera camera(cam("a", {out("o1")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state,
                    bc::SyncPolicy{});
  camera.setPreviewQuery([] { return false; });
  camera.start();
  REQUIRE(factory.made != nullptr);
  camera.onState(bc::SignalState::Live, std::nullopt);
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  for (int i = 0; i < 2; ++i) {
    if (i == 0) camera.onCodedOmt(dummyCoded(0));  // learn the source's mode
    camera.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
  }
  REQUIRE(camera.wantsCodedOmt());
  factory.made->keyframe = true;
  CHECK(camera.onCodedOmt(dummyCoded(1)));
  CHECK_FALSE(camera.onCodedOmt(dummyCoded(2)));  // taken once
  camera.stop();
}

TEST_CASE("Camera: a Full coded source with only an HX output keeps decoding") {
  Rig r;
  RecordingFactory factory(std::vector<RecSpec>{RecSpec{false, false, false, static_cast<int>(omthx::Mode::Hx)}});
  bc::Camera camera(cam("a", {out("o1")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state,
                    bc::SyncPolicy{});
  camera.setPreviewQuery([] { return false; });
  camera.start();
  REQUIRE(factory.made != nullptr);
  camera.onState(bc::SignalState::Live, std::nullopt);
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  auto tick = [&] {
    camera.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
  };
  tick();
  CHECK(camera.wantsGpuVideo());  // the source's mode is not known yet
  const int before = factory.made->submitted;
  CHECK_FALSE(camera.onCodedOmt(dummyCoded(1, omthx::Mode::Full)));
  CHECK(camera.wantsGpuVideo());  // a Full frame and an HX-only output: no pass-through
  tick();
  tick();
  CHECK(factory.made->submitted > before);  // the HX output gets tick pictures instead
  CHECK(factory.made->coded == 0);
  camera.stop();
}

TEST_CASE("Camera: an HX pass-through falls back to decoding when the source turns to Full") {
  Rig r;
  RecordingFactory factory(std::vector<RecSpec>{RecSpec{false, false, false, static_cast<int>(omthx::Mode::Hx)}});
  bc::Camera camera(cam("a", {out("o1")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state,
                    bc::SyncPolicy{});
  camera.setPreviewQuery([] { return false; });
  camera.start();
  REQUIRE(factory.made != nullptr);
  camera.onState(bc::SignalState::Live, std::nullopt);
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  auto tick = [&] {
    camera.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
  };
  for (int i = 0; i < 3; ++i) {
    camera.onCodedOmt(dummyCoded(static_cast<uint64_t>(i), omthx::Mode::Hx));
    tick();
  }
  CHECK(camera.wantsCodedOmt());
  REQUIRE_FALSE(camera.wantsGpuVideo());  // HX source, HX output: pass-through
  REQUIRE(factory.made->coded == 2);

  // The source turns to Full: the camera must decode again within a frame or two.
  camera.onCodedOmt(dummyCoded(9, omthx::Mode::Full));
  CHECK(camera.wantsGpuVideo());  // known on the input thread: no tick is needed to decide
  const int before = factory.made->submitted;
  tick();
  tick();
  CHECK(factory.made->submitted > before);  // pictures resume on the tick path
  CHECK(factory.made->coded == 2);          // and the Full frames were not passed on
  camera.stop();
}

TEST_CASE("Camera: reconfiguring pass-through outputs while coded frames flow is safe") {
  Rig r;
  RecordingFactory factory(std::vector<RecSpec>{RecSpec{false, false, false, static_cast<int>(omthx::Mode::Full)}});
  bc::CameraConfig cfg = cam("a", {out("o1")});
  bc::Camera camera(cfg, factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.setPreviewQuery([] { return false; });
  camera.start();
  camera.onState(bc::SignalState::Live, std::nullopt);
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  camera.onCodedOmt(dummyCoded(0));  // learn the source's mode, so this output becomes a target
  camera.onVideo(solidFrame(r.conv, 120));
  const auto now = Clock::now();
  camera.beginTick(now, slate);
  camera.endTick(now, nullptr);
  REQUIRE(camera.wantsCodedOmt());

  std::atomic<bool> run{true};
  std::thread input([&] {
    auto c = dummyCoded(3);
    while (run.load()) camera.onCodedOmt(c);
  });
  for (int i = 0; i < 100; ++i) {
    cfg.outputs = {out(i % 2 ? "o1" : "o2")};
    camera.reconfigure(cfg);
  }
  run = false;
  input.join();
  camera.stop();
  CHECK(true);  // no use after free: the targets are removed under the mutex before a slot goes
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

// Task 3 (OMT copy paths): an HX output sends from the GPU, so a camera whose only consumer is
// such an output does no readback at all; it gets the frame as an R8G8B8A8 texture instead. A CPU
// output beside it still forces the one readback, and both get their frame (acceptance 3).
TEST_CASE("Camera: an HX GPU output alone does not read back") {
  Rig r;
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  auto tick = [&](bc::Camera& c) {
    c.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    c.beginTick(now, slate);
    c.endTick(now, nullptr);
    std::this_thread::sleep_for(3ms);
  };

  RecordingFactory gpuOnly(std::vector<RecSpec>{{/*needsCpu=*/false, /*wantsGpu=*/true}});
  bc::Camera camera(cam("a", {out("o1")}), gpuOnly, bc::GpuTools{r.conv, r.comp, r.grader}, r.state,
                    bc::SyncPolicy{});
  camera.start();
  REQUIRE(gpuOnly.all.size() == 1);
  for (int i = 0; i < 30; ++i) tick(camera);
  CHECK(camera.tickCpu() == nullptr);  // no readback for a GPU-only consumer
  CHECK(camera.readbackSlotCount() == 0);
  CHECK(gpuOnly.all[0]->last == nullptr);
  REQUIRE(gpuOnly.all[0]->lastRgba8 != nullptr);  // the frame arrives as a GPU texture instead
  CHECK(gpuOnly.all[0]->lastRgba8->format == DXGI_FORMAT_R8G8B8A8_UNORM);
  CHECK(gpuOnly.all[0]->lastRgba8->width == 64);
  CHECK(gpuOnly.all[0]->lastRgba8->height == 36);
  camera.stop();

  RecordingFactory mixed(std::vector<RecSpec>{{false, true}, {true, false}});
  bc::Camera both(cam("a", {out("o1"), out("o2")}), mixed, bc::GpuTools{r.conv, r.comp, r.grader}, r.state,
                  bc::SyncPolicy{});
  both.start();
  REQUIRE(mixed.all.size() == 2);
  for (int i = 0; i < 30; ++i) tick(both);
  CHECK(both.tickCpu() != nullptr);  // the CPU output forces the one readback
  CHECK(both.readbackSlotCount() == 1);
  CHECK(mixed.all[0]->lastRgba8 != nullptr);   // the HX output still sends from the GPU
  CHECK(mixed.all[1]->last != nullptr);        // the CPU output gets the shared image
  CHECK(mixed.all[1]->lastRgba8 == nullptr);   // and is not handed the GPU frame it never asked for
  both.stop();
}

// The D3D11 path takes no alpha, so a keyed frame must still be read back for an HX output:
// otherwise the alpha picture would be dropped. It goes through memory, as every output did before.
TEST_CASE("Camera: an alpha frame stays on the memory path for an HX output") {
  Rig r;
  RecordingFactory factory(std::vector<RecSpec>{{/*needsCpu=*/false, /*wantsGpu=*/true}});
  bc::CameraConfig cfg = cam("a", {out("o1")});
  cfg.stages = {{"background", bc::normalizeStageParams("background", {{"mode", "keyed"}})}};
  bc::Camera camera(cfg, factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.start();
  REQUIRE(factory.all.size() == 1);
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  auto alpha = r.conv.solid(64, 36, {1, 1, 1, 1});  // a full matte: the person everywhere
  for (int i = 0; i < 30; ++i) {
    camera.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, alpha);
    std::this_thread::sleep_for(3ms);
  }
  CHECK(camera.readbackSlotCount() == 1);
  CHECK(camera.tickCpu() != nullptr);
  REQUIRE(factory.all[0]->last != nullptr);     // the alpha frame reaches the output through memory
  CHECK(factory.all[0]->lastRgba8 == nullptr);  // not as a texture, which cannot carry alpha
  camera.stop();
}

// Plan review focus 2: a keyer turned on mid-show. Opaque ticks travel as UYVY; from the tick
// the matte arrives they fall back to BGRA, and an alpha frame is never sent as UYVY.
TEST_CASE("Camera: a UYVY output sends BGRA, not UYVY, when the frame gains alpha") {
  Rig r;
  RecordingFactory factory(std::vector<RecSpec>{{/*needsCpu=*/true, /*wantsGpu=*/false, /*wantsUyvy=*/true}});
  bc::CameraConfig cfg = cam("a", {out("o1")});
  cfg.stages = {{"background", bc::normalizeStageParams("background", {{"mode", "keyed"}})}};
  bc::Camera camera(cfg, factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.start();
  REQUIRE(factory.all.size() == 1);
  RecordingOutput* o = factory.all[0];
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  auto alpha = r.conv.solid(64, 36, {1, 1, 1, 1});
  bool sawUyvy = false, alphaOn = false, sawBgraAfter = false;
  for (int i = 0; i < 40; ++i) {
    camera.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, alphaOn ? alpha : nullptr);
    if (!alphaOn) {
      if (o->lastUyvy) sawUyvy = true;
      if (sawUyvy) alphaOn = true;  // the keyer comes on once the UYVY path is live
    } else {
      CHECK(o->lastUyvy == nullptr);  // an alpha frame never travels as UYVY
      if (o->last) sawBgraAfter = true;
    }
    std::this_thread::sleep_for(3ms);
  }
  CHECK(sawUyvy);
  CHECK(sawBgraAfter);
  if (o->last) CHECK(o->last->layout == bc::CpuLayout::Bgra);
  camera.stop();
}

// Plan review focus 3: mixed outputs on one camera each get their own layout, and there is one
// readback slot per layout that a consumer needs.
TEST_CASE("Camera: a UYVY output and a BGRA output each get their own layout") {
  Rig r;
  RecordingFactory factory(
      std::vector<RecSpec>{{/*needsCpu=*/true, /*wantsGpu=*/false, /*wantsUyvy=*/true}, {true, false, false}});
  bc::Camera camera(cam("a", {out("o1"), out("o2")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state,
                    bc::SyncPolicy{});
  camera.start();
  REQUIRE(factory.all.size() == 2);
  RecordingOutput* uyvy = factory.all[0];
  RecordingOutput* bgra = factory.all[1];
  auto slate = r.conv.solid(64, 36, {0, 0, 0, 1});
  bool sawUyvy = false, sawBgra = false;
  for (int i = 0; i < 30; ++i) {
    camera.onVideo(solidFrame(r.conv, 120));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
    if (uyvy->lastUyvy) {
      sawUyvy = true;
      CHECK(uyvy->lastUyvy->layout == bc::CpuLayout::Uyvy);
      CHECK(uyvy->lastUyvy->bgra.size() == 64u * 36u * 2u);
      CHECK(uyvy->last == nullptr);  // never both layouts at once
    }
    if (bgra->last) {
      sawBgra = true;
      CHECK(bgra->last->layout == bc::CpuLayout::Bgra);
      CHECK(bgra->lastUyvy == nullptr);
    }
    CHECK(camera.readbackSlotCount() == 2);
    std::this_thread::sleep_for(3ms);
  }
  CHECK(sawUyvy);
  CHECK(sawBgra);
  camera.stop();
}

// The plan's headline case: a camera whose only CPU consumer wants UYVY runs one readback slot,
// and that slot hands out the picture's width and 2 bytes a pixel.
TEST_CASE("Camera: a UYVY-only output runs one readback slot of 2 bytes a pixel") {
  Rig r;
  RecordingFactory factory(std::vector<RecSpec>{{/*needsCpu=*/true, /*wantsGpu=*/false, /*wantsUyvy=*/true}});
  bc::Camera camera(cam("a", {out("o1")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.start();
  REQUIRE(factory.all.size() == 1);
  RecordingOutput* o = factory.all[0];
  auto slate = r.conv.solid(1920, 1080, {0, 0, 0, 1});
  bool saw = false;
  for (int i = 0; i < 40 && !saw; ++i) {
    camera.onVideo(solidFrame(r.conv, 120, 1920, 1080));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
    CHECK(camera.readbackSlotCount() == 1);
    if (o->lastUyvy) {
      saw = true;
      CHECK(o->last == nullptr);
      CHECK(o->lastUyvy->layout == bc::CpuLayout::Uyvy);
      CHECK(o->lastUyvy->width == 1920);
      CHECK(o->lastUyvy->height == 1080);
      CHECK(o->lastUyvy->bgra.size() == 1920u * 1080u * 2u);
    }
    std::this_thread::sleep_for(3ms);
  }
  CHECK(saw);
  camera.stop();
}

// Plan review focus 4: a size change must not publish the old size's completed UYVY image.
TEST_CASE("Camera: a size change never sends a UYVY frame of the old size") {
  Rig r;
  RecordingFactory factory(std::vector<RecSpec>{{/*needsCpu=*/true, /*wantsGpu=*/false, /*wantsUyvy=*/true}});
  bc::Camera camera(cam("a", {out("o1")}), factory, bc::GpuTools{r.conv, r.comp, r.grader}, r.state, bc::SyncPolicy{});
  camera.start();
  REQUIRE(factory.all.size() == 1);
  RecordingOutput* o = factory.all[0];
  auto slate = r.conv.solid(1920, 1080, {0, 0, 0, 1});
  auto tick = [&](uint32_t w, uint32_t h) {
    camera.onVideo(solidFrame(r.conv, 120, w, h));
    const auto now = Clock::now();
    camera.beginTick(now, slate);
    camera.endTick(now, nullptr);
    std::this_thread::sleep_for(3ms);
    if (o->lastUyvy) CHECK(o->lastUyvy->width == w);  // ever only the frame the camera is on
  };
  for (int i = 0; i < 20 && !o->lastUyvy; ++i) tick(1920, 1080);
  REQUIRE(o->lastUyvy);
  bool saw720 = false;
  for (int i = 0; i < 30; ++i) {
    tick(1280, 720);
    if (o->lastUyvy && o->lastUyvy->height == 720) saw720 = true;
  }
  CHECK(saw720);
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
  CHECK_THROWS_WITH_AS(factory.makeOutput(o, c, {60, 1}), "NDI name must be text", std::invalid_argument);
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
  CHECK(factory.makeOutput(o, c, {60, 1}) != nullptr);
  o.params["name"] = 7;  // hand-edited show file: name is not text
  CHECK_THROWS_WITH_AS(bc::validateOutput(o), "OMT name must be text", std::invalid_argument);
  CHECK_THROWS_WITH_AS(factory.makeOutput(o, c, {60, 1}), "OMT name must be text", std::invalid_argument);
  o.params["name"] = "Stage";
  o.params["mode"] = 5;  // hand-edited show file: mode is not text
  CHECK_THROWS_WITH_AS(bc::validateOutput(o), "OMT mode must be text", std::invalid_argument);
  CHECK_THROWS_WITH_AS(factory.makeOutput(o, c, {60, 1}), "OMT mode must be text", std::invalid_argument);
#else
  CHECK_THROWS_WITH_AS(factory.makeOutput(o, c, {60, 1}), "OMT support is not built (BC_WITH_OMT=OFF)", std::invalid_argument);
#endif
}
