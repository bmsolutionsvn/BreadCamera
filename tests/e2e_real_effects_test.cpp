// Real end-to-end with an effect: a writeTestTs clip is pushed into a real SRT listener input,
// the real Engine (with the real adapter factory) applies a `color` stage, and the frames are
// read back from a real SRT listener output by a real FfmpegInput. The same pipeline runs with
// the stage disabled as the control, and the two mean-luma values are compared, so the assertion
// is about the output *pixels*, not a stage's state flag.
#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

#include "core/stage_params.h"
#include "core/state_store.h"
#include "engine/adapter_factory.h"
#include "engine/engine.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/converter.h"
#include "gpu/device.h"
#include "io/ffmpeg_input.h"
#include "io/srt_params.h"
#include "support/requires.h"
#include "support/temp_dir.h"
#include "support/test_media.h"

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
// Reads the frames from the SRT output and, once, converts one frame to BGRA8 and averages its
// BT.709 luma. FfmpegInput calls onVideo without the GPU lock held, so the sampled frame can be
// converted here; the frame is not kept by reference (the pool may recycle it after onVideo).
struct LumaSink : bc::InputSink {
  explicit LumaSink(bc::Converter& c) : conv(c) {}
  bc::Converter& conv;
  int sampleAt = 300;  // ~5 s at 60 fps: a settled clip frame after the output goes live
  std::atomic<int> video{0};
  std::atomic<double> luma{-1};

  void onVideo(bc::VideoFrameIn f) override {
    const int n = ++video;
    if (n != sampleAt) return;
    auto linear = conv.toLinear(*f.tex, f.layout, f.fullRange);
    auto rgba8 = conv.toBgra8(*linear, f.tex->width, f.tex->height);
    const bc::CpuImage img = conv.readback(*rgba8);
    double sum = 0;
    for (size_t i = 0; i + 3 < img.bgra.size(); i += 4)
      sum += 0.0722 * img.bgra[i] + 0.7152 * img.bgra[i + 1] + 0.2126 * img.bgra[i + 2];  // B G R
    luma = sum / (static_cast<double>(img.width) * img.height);
  }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
};

struct Result {
  int frames = 0;
  double meanLuma = -1;
  // The camera's state mid-run (the engine is stopped afterwards). `phase` is the authoritative
  // "frames are arriving at the tick" signal (frame_sync: live/holding/slate).
  std::string inputState, stageState, outputState, phase;
};

// One full real-transport run: real SRT input -> real Engine -> real SRT output, read back.
// `colorParams` is the camera's `color` stage params (null = a camera with no effect at all).
Result runPipeline(const std::filesystem::path& clip, int inPort, int outPort, const json* colorParams) {
  Result r;
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool(*gpu);
  bc::Converter conv(*gpu, pool);
  bc::Compositor comp(*gpu, pool);
  bc::ColorGrader grader(*gpu, pool);
  bc::StateStore state;
  bc::DefaultAdapterFactory factory(*gpu, pool);
  bc::Engine engine(bc::GpuTools{conv, comp, grader}, state, factory);

  bc::Show show;
  bc::CameraConfig c;
  c.id = "cam";
  c.name = "Cam";
  c.input.kind = "srt";
  c.input.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", inPort}};
  if (colorParams) c.stages["color"] = *colorParams;  // CameraConfig::stages is keyed by stage kind
  c.outputs.push_back({"srt-out", "srt", false, true, {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", outPort}}});
  show.cameras.push_back(c);
  engine.apply(show);
  engine.start();

  bc::test::TsPusher push(clip, "srt://127.0.0.1:" + std::to_string(inPort) + "?mode=caller", true);
  bc::FfmpegInput reader(*gpu, pool, bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "127.0.0.1"}, {"port", outPort}}));
  LumaSink sink(conv);
  reader.start(sink);
  std::this_thread::sleep_for(7s);
  {
    const auto s = state.snapshot();
    r.inputState = s["cameras"]["cam"]["input"]["state"];
    r.stageState = s["cameras"]["cam"]["stages"]["color"]["state"];
    r.outputState = s["cameras"]["cam"]["outputs"]["srt-out"]["state"];
    r.phase = s["cameras"]["cam"]["phase"];
  }
  std::this_thread::sleep_for(3s);
  reader.stop();
  push.stop();
  engine.stop();

  r.frames = sink.video.load();
  r.meanLuma = sink.luma.load();
  return r;
}
}  // namespace

TEST_CASE("real effect acceptance: SRT in -> colour effect -> SRT out, output pixels compare" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  // The `color` schema's luma control is `exposure` in EV over [-3, 3] (stage_params.cpp), applied
  // as a linear-light gain of 2^EV (color_grader.cpp: a[0] = pow(2, exposure)).
  const json color = bc::normalizeStageParams("color", {{"enabled", true}, {"exposure", 2.0}});
  REQUIRE(bc::findStageSchema("color") != nullptr);
  REQUIRE(color["enabled"].get<bool>());
  REQUIRE(color["exposure"].get<double>() == 2.0);

  bc::test::TempDir dir;
  bc::test::writeTestTs(dir.path / "in.ts", 1280, 720, 60, 6);

  const Result on = runPipeline(dir.path / "in.ts", 49261, 49262, &color);
  const Result off = runPipeline(dir.path / "in.ts", 49271, 49272, nullptr);

  MESSAGE("effect on:  " << on.frames << " frames read back, mean luma " << on.meanLuma);
  MESSAGE("effect off: " << off.frames << " frames read back, mean luma " << off.meanLuma);
  MESSAGE("on:   input=" << on.inputState << " stage=" << on.stageState << " output=" << on.outputState << " phase=" << on.phase);
  MESSAGE("off:  input=" << off.inputState << " stage=" << off.stageState << " output=" << off.outputState << " phase=" << off.phase);

  // Both runs are a real transport: frames flow from a live input to a live output, and the
  // camera's phase is `live` (frames are actually reaching the tick, not the slate).
  CHECK(on.frames > 300);
  CHECK(off.frames > 300);
  CHECK(on.inputState == "live");
  CHECK(off.inputState == "live");
  CHECK(on.outputState == "live");
  CHECK(off.outputState == "live");
  CHECK(on.phase == "live");
  CHECK(off.phase == "live");
  // The control run has no effect at all; the measured run really has the colour stage active.
  CHECK(on.stageState == "active");
  CHECK(off.stageState == "off");
  CHECK(on.meanLuma > 0);
  CHECK(off.meanLuma > 0);
  // The no-stage control must show the clip itself, not the slate: writeTestTs writes a Y ramp of
  // 16..235 (mean ~125.5), and the slate ({0.05,0.05,0.06} linear) reads ~63 -- well outside the band.
  CHECK(off.meanLuma >= 110.0);
  CHECK(off.meanLuma <= 140.0);
  // The clip is a mid-grey ramp, so +2 EV (a 4x linear-light gain) must be clearly brighter. The
  // no-stage control fixes the baseline, so this is a change in the output pixels, not a flag.
  // 30/255 is more than ten times the H.264 round-trip noise (a few counts) and well below the
  // measured lift, so the direction is asserted with a clear margin.
  CHECK(on.meanLuma - off.meanLuma > 30.0);
}
