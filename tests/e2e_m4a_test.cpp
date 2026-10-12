#include <doctest/doctest.h>

#include <thread>

#include "core/state_store.h"
#include "engine/adapter_factory.h"
#include "engine/engine.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "io/ffmpeg_input.h"
#include "io/srt_params.h"
#include "support/requires.h"
#include "support/temp_dir.h"
#include "support/test_media.h"

using namespace std::chrono_literals;
using nlohmann::json;

namespace {
struct Count : bc::InputSink {
  std::atomic<int> video{0};
  void onVideo(bc::VideoFrameIn) override { ++video; }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
};
}  // namespace

TEST_CASE("M4a acceptance: SRT in → engine → SRT out, read back" BC_REQUIRES(bc::test::Req::H264Encoder)) {
  bc::test::TempDir dir;
  bc::test::writeTestTs(dir.path / "in.ts", 1280, 720, 60, 6);
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
  c.input.params = {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 49161}};
  c.outputs.push_back({"srt-out", "srt", false, true, {{"mode", "listener"}, {"host", "127.0.0.1"}, {"port", 49162}}, 1280, 720});
  show.cameras.push_back(c);
  engine.apply(show);
  engine.start();
  bc::test::TsPusher push(dir.path / "in.ts", "srt://127.0.0.1:49161?mode=caller", true);
  bc::FfmpegInput reader(*gpu, pool, bc::srtOptionsFromParams({{"mode", "caller"}, {"host", "127.0.0.1"}, {"port", 49162}}));
  Count count;
  reader.start(count);
  std::this_thread::sleep_for(10s);
  reader.stop();
  push.stop();
  engine.stop();
  MESSAGE("frames read back from the SRT output: " << count.video.load());
  CHECK(count.video > 300);
  CHECK(state.snapshot()["cameras"]["cam"]["outputs"]["srt-out"]["state"] == "live");
}