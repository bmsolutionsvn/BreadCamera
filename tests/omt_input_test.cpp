#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "core/state_store.h"
#include "engine/adapter_factory.h"
#include "engine/engine.h"
#include "gpu/color_grader.h"
#include "gpu/compositor.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"
#include "support/requires.h"

#if BC_WITH_OMT
#include <windows.h>

#include <psapi.h>

#include "io/omt_input.h"
#include "io/omt_output.h"
#include "omthx/omthx.h"

using namespace std::chrono_literals;

namespace {
struct CountingSink : bc::InputSink {
  std::atomic<int> videos{0};
  std::atomic<int> audios{0};
  std::atomic<uint32_t> width{0};
  std::atomic<uint32_t> height{0};
  std::atomic<bool> live{false};

  void onVideo(bc::VideoFrameIn f) override {
    width = f.tex ? f.tex->width : 0;
    height = f.tex ? f.tex->height : 0;
    ++videos;
  }
  void onAudio(bc::AudioBlock) override { ++audios; }
  void onState(bc::SignalState state, std::optional<bc::ErrorInfo>) override {
    if (state == bc::SignalState::Live) live = true;
  }
};

// Converts every picture it gets, so the layout, range and matrix the input declared are the ones
// the real pipeline would use.
struct PictureSink : bc::InputSink {
  explicit PictureSink(bc::Converter& c) : conv(c) {}
  bc::Converter& conv;
  std::atomic<int> videos{0};
  std::atomic<int> layout{-1};
  std::atomic<bool> fullRange{false};
  std::atomic<bool> bt709{false};
  std::atomic<bool> colourOk{true};

  void onVideo(bc::VideoFrameIn f) override {
    if (!f.tex) return;
    layout = static_cast<int>(f.layout);
    fullRange = f.fullRange;
    bt709 = f.bt709;
    try {
      auto img = conv.readback(*conv.toBgra8(*conv.toLinear(*f.tex, f.layout, f.fullRange, f.bt709),
                                             f.tex->width, f.tex->height));
      const size_t c = (static_cast<size_t>(img.height / 2) * img.width + img.width / 2) * 4;
      if (std::abs(static_cast<int>(img.bgra[c]) - 100) > 8 ||
          std::abs(static_cast<int>(img.bgra[c + 1]) - 50) > 8 ||
          std::abs(static_cast<int>(img.bgra[c + 2]) - 200) > 8) {
        colourOk = false;
      }
    } catch (const std::exception&) {
      colourOk = false;
    }
    ++videos;
  }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
};

// BreadReplay's recorder tap: CPU frames only, and it asks for coded VMX first. An HX source
// falls back to decoded pictures, so this is the sink the NV12 receiver must not starve.
struct CpuOnlySink : bc::InputSink {
  std::atomic<int> cpuFrames{0};
  std::atomic<int> pictures{0};
  std::atomic<int> format{-1};
  std::atomic<bool> colourOk{true};

  void onVideo(bc::VideoFrameIn) override { ++pictures; }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
  bool wantsCpuVideo() const override { return true; }
  bool wantsGpuVideo() const override { return false; }
  bool wantsCodedVmx() const override { return true; }

  void onCpuVideo(const bc::CpuVideoFrame& f) override {
    format = static_cast<int>(f.format);
    if (f.format != bc::CpuPixel::BGRA || f.width == 0 || f.height == 0) {
      colourOk = false;
    } else {
      const size_t c = static_cast<size_t>(f.height / 2) * static_cast<size_t>(f.stride) +
                       static_cast<size_t>(f.width / 2) * 4;
      if (std::abs(static_cast<int>(f.data[c]) - 100) > 8 ||
          std::abs(static_cast<int>(f.data[c + 1]) - 50) > 8 ||
          std::abs(static_cast<int>(f.data[c + 2]) - 200) > 8) {
        colourOk = false;
      }
    }
    ++cpuFrames;
  }
};
}  // namespace

TEST_CASE("OmtInput: receives an OMT HX source as pictures and audio" BC_REQUIRES(bc::test::Req::Omt)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  if (!gpu) return;
  bc::TexturePool pool{*gpu};

  const std::string name = "BreadCamera OMT input " + std::to_string(GetCurrentProcessId());
  omthx::Sender sender(name, omthx::Mode::Hx, omthx::Quality::Medium);

  omthx::VideoFormat format;
  format.width = 320;
  format.height = 180;
  format.fpsN = 30;
  format.fpsD = 1;
  const std::vector<uint8_t> bgra(static_cast<size_t>(320) * 180 * 4, 100);
  const std::vector<float> tone(1600 * 2, 0.01f);

  std::atomic<bool> run{true};
  std::thread feeder([&] {
    int64_t timestamp = 0;
    while (run) {
      sender.sendVideo(format, bgra, false, timestamp);
      sender.sendAudio(tone, 48000, 2, timestamp);
      timestamp += 333333;  // 30 fps in 100 ns units
      std::this_thread::sleep_for(33ms);
    }
  });

  const std::string address = sender.address();
  REQUIRE_FALSE(address.empty());

  CountingSink sink;
  bc::OmtInput input(*gpu, pool, address);
  input.start(sink);
  for (int i = 0; i < 100 && sink.videos.load() == 0; ++i) std::this_thread::sleep_for(50ms);
  input.stop();
  run = false;
  if (feeder.joinable()) feeder.join();

  CHECK(sink.videos.load() > 0);
  CHECK(sink.width.load() == 320);
  CHECK(sink.height.load() == 180);
  CHECK(sink.live.load());
}

namespace {
// Sends a solid BGRA picture (B 100, G 50, R 200) from an in-process OMT sender and checks what
// OmtInput makes of it: layout, range, matrix and colour through the real converter.
void checkPicture(omthx::Mode mode, bc::PixelLayout want, const std::string& senderName) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};

  omthx::Sender sender(senderName, mode, omthx::Quality::Medium);
  omthx::VideoFormat format;
  format.width = 640;
  format.height = 360;
  format.fpsN = 30;
  format.fpsD = 1;
  std::vector<uint8_t> bgra(static_cast<size_t>(640) * 360 * 4);
  for (size_t i = 0; i < bgra.size(); i += 4) {
    bgra[i] = 100;      // B
    bgra[i + 1] = 50;   // G
    bgra[i + 2] = 200;  // R
    bgra[i + 3] = 255;
  }

  std::atomic<bool> run{true};
  std::thread feeder([&] {
    int64_t timestamp = 0;
    while (run) {
      sender.sendVideo(format, bgra, false, timestamp);
      timestamp += 333333;  // 30 fps in 100 ns units
      std::this_thread::sleep_for(33ms);
    }
  });

  PictureSink sink(conv);
  bc::OmtInput input(*gpu, pool, sender.address());
  input.start(sink);
  for (int i = 0; i < 200 && sink.videos.load() == 0; ++i) std::this_thread::sleep_for(50ms);
  input.stop();
  run = false;
  if (feeder.joinable()) feeder.join();

  REQUIRE(sink.videos.load() > 0);
  CHECK(sink.layout.load() == static_cast<int>(want));
  CHECK(sink.fullRange.load());
  CHECK(sink.bt709.load() == (want == bc::PixelLayout::NV12));
  CHECK(sink.colourOk.load());
}
}  // namespace

TEST_CASE("OmtInput: an HX source arrives as NV12" BC_REQUIRES(bc::test::Req::Omt)
          BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  checkPicture(omthx::Mode::Hx, bc::PixelLayout::NV12,
               "BreadCamera OMT input NV12 " + std::to_string(GetCurrentProcessId()));
}

TEST_CASE("OmtInput: a plain OMT source stays BGRA" BC_REQUIRES(bc::test::Req::Omt)
          BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  checkPicture(omthx::Mode::Full, bc::PixelLayout::BGRA8,
               "BreadCamera OMT input BGRA " + std::to_string(GetCurrentProcessId()));
}

TEST_CASE("OmtInput: a CPU-only sink gets HX pictures as BGRA" BC_REQUIRES(bc::test::Req::Omt)
          BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool{*gpu};

  omthx::Sender sender("BreadCamera OMT input CPU " + std::to_string(GetCurrentProcessId()), omthx::Mode::Hx,
                       omthx::Quality::Medium);
  omthx::VideoFormat format;
  format.width = 640;
  format.height = 360;
  format.fpsN = 30;
  format.fpsD = 1;
  std::vector<uint8_t> bgra(static_cast<size_t>(640) * 360 * 4);
  for (size_t i = 0; i < bgra.size(); i += 4) {
    bgra[i] = 100;      // B
    bgra[i + 1] = 50;   // G
    bgra[i + 2] = 200;  // R
    bgra[i + 3] = 255;
  }

  std::atomic<bool> run{true};
  std::thread feeder([&] {
    int64_t timestamp = 0;
    while (run) {
      sender.sendVideo(format, bgra, false, timestamp);
      timestamp += 333333;  // 30 fps in 100 ns units
      std::this_thread::sleep_for(33ms);
    }
  });

  CpuOnlySink sink;
  bc::OmtInput input(*gpu, pool, sender.address());
  input.start(sink);
  for (int i = 0; i < 200 && sink.cpuFrames.load() == 0; ++i) std::this_thread::sleep_for(50ms);
  input.stop();
  run = false;
  if (feeder.joinable()) feeder.join();

  CHECK(sink.cpuFrames.load() > 0);
  CHECK(sink.pictures.load() == 0);
  CHECK(sink.format.load() == static_cast<int>(bc::CpuPixel::BGRA));
  CHECK(sink.colourOk.load());
}

namespace {
struct CodedSink : bc::InputSink {
  std::atomic<int> coded{0};
  std::atomic<int> wrong{0};
  std::atomic<int> pictures{0};

  void onVideo(bc::VideoFrameIn) override { ++pictures; }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
  bool wantsCodedVmx() const override { return true; }
  bool wantsGpuVideo() const override { return false; }
  void onCodedVideo(const bc::CodedVideoFrame& f) override {
    ++coded;
    if (f.codec != "vmx" || f.data.empty() || f.width != 320 || f.height != 180) ++wrong;
  }
};
}  // namespace

TEST_CASE("OmtInput: a coded sink gets VMX bytes" BC_REQUIRES(bc::test::Req::Omt)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  if (!gpu) return;
  bc::TexturePool pool{*gpu};

  const std::string name = "BreadCamera OMT coded " + std::to_string(GetCurrentProcessId());
  omthx::Sender sender(name, omthx::Mode::Full, omthx::Quality::Medium);

  omthx::VideoFormat format;
  format.width = 320;
  format.height = 180;
  format.fpsN = 30;
  format.fpsD = 1;
  const std::vector<uint8_t> bgra(static_cast<size_t>(320) * 180 * 4, 100);

  std::atomic<bool> run{true};
  std::thread feeder([&] {
    int64_t timestamp = 0;
    while (run) {
      sender.sendVideo(format, bgra, false, timestamp);
      timestamp += 333333;  // 30 fps in 100 ns units
      std::this_thread::sleep_for(33ms);
    }
  });

  CodedSink sink;
  bc::OmtInput input(*gpu, pool, sender.address());
  input.start(sink);
  for (int i = 0; i < 140 && sink.coded.load() < 20; ++i) std::this_thread::sleep_for(50ms);
  input.stop();
  run = false;
  if (feeder.joinable()) feeder.join();

  CHECK(sink.coded.load() >= 20);
  CHECK(sink.wrong.load() == 0);
  CHECK(sink.pictures.load() == 0);
}

namespace {
// A recorder tap that takes coded OMT-HX access units as they arrive, and VMX frames too.
struct CodedHxSink : bc::InputSink {
  std::atomic<int> coded{0};
  std::atomic<int> wrong{0};
  std::atomic<int> pictures{0};
  std::mutex m;
  std::vector<int64_t> keyTimes;

  void onVideo(bc::VideoFrameIn) override { ++pictures; }
  void onAudio(bc::AudioBlock) override {}
  void onState(bc::SignalState, std::optional<bc::ErrorInfo>) override {}
  bool wantsCodedVmx() const override { return true; }
  bool wantsCodedHx() const override { return true; }
  bool wantsGpuVideo() const override { return false; }
  bool wantsCpuVideo() const override { return false; }
  void onCodedVideo(const bc::CodedVideoFrame& f) override {
    const int n = ++coded;
    if (f.codec != "hevc" || f.data.empty() || f.width != 320 || f.height != 180) ++wrong;
    if (n == 1 && !f.keyframe) ++wrong;
    if (f.keyframe) {
      std::lock_guard lock(m);
      keyTimes.push_back(f.ptsUs);
    }
  }
};
}  // namespace

TEST_CASE("OmtInput: a coded HX sink gets HEVC access units and keyframes" BC_REQUIRES(bc::test::Req::Omt)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  if (!gpu) return;
  bc::TexturePool pool{*gpu};

  const std::string name = "BreadCamera OMT coded HX " + std::to_string(GetCurrentProcessId());
  omthx::Sender sender(name, omthx::Mode::Hx, omthx::Quality::Medium, omthx::Codec::Hevc);

  omthx::VideoFormat format;
  format.width = 320;
  format.height = 180;
  format.fpsN = 30;
  format.fpsD = 1;
  const std::vector<uint8_t> bgra(static_cast<size_t>(320) * 180 * 4, 100);

  std::atomic<bool> run{true};
  std::thread feeder([&] {
    int64_t timestamp = 0;
    while (run) {
      sender.sendVideo(format, bgra, false, timestamp);
      timestamp += 333333;  // 30 fps in 100 ns units
      std::this_thread::sleep_for(33ms);
    }
  });

  CodedHxSink sink;
  bc::OmtInput input(*gpu, pool, sender.address());
  input.start(sink);
  for (int i = 0; i < 100 && sink.coded.load() < 30; ++i) std::this_thread::sleep_for(50ms);
  input.stop();
  run = false;
  if (feeder.joinable()) feeder.join();

  CHECK(sink.coded.load() >= 30);
  CHECK(sink.wrong.load() == 0);
  CHECK(sink.pictures.load() == 0);

  std::vector<int64_t> keys;
  {
    std::lock_guard lock(sink.m);
    keys = sink.keyTimes;
  }
  REQUIRE(keys.size() >= 2);
  const int64_t span = keys.back() - keys.front();
  CHECK(span <= static_cast<int64_t>(keys.size() - 1) * 600'000);  // a keyframe at least every 600 ms
}

namespace {
uint64_t privateBytes() {
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                            sizeof counters)) {
    return 0;
  }
  return counters.PrivateUsage;
}
}  // namespace

// The audit's critical finding: every OMT input kept about one BGRA picture per received frame.
// 300 frames of 1080p through the real Camera path, and the process must not grow per frame.
// The HX sender decodes to NV12, so the same window covers the plane upload path too.
static void leakCase(omthx::Mode mode, const char* sourceName) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::Hardware);
  bc::TexturePool pool{*gpu};
  bc::Converter conv{*gpu, pool};
  bc::Compositor comp{*gpu, pool};
  bc::ColorGrader grader{*gpu, pool};
  bc::StateStore state;
  bc::DefaultAdapterFactory factory{*gpu, pool};
  bc::Engine engine{bc::GpuTools{conv, comp, grader}, state, factory};

  std::atomic<int> frames{0};
  engine.setFrameTap([&](uint64_t, const std::vector<bc::FrameTap>& tapped) {
    for (const auto& f : tapped) {
      if (f.id == "a") ++frames;
    }
  });

  omthx::Sender sender(sourceName, mode, omthx::Quality::Medium);
  omthx::VideoFormat format;
  format.width = 1920;
  format.height = 1080;
  format.fpsN = 30;
  format.fpsD = 1;
  const std::vector<uint8_t> bgra(static_cast<size_t>(1920) * 1080 * 4, 100);

  std::atomic<bool> run{true};
  std::thread feeder([&] {
    int64_t timestamp = 0;
    while (run) {
      sender.sendVideo(format, bgra, false, timestamp);
      timestamp += 333333;  // 30 fps in 100 ns units
      std::this_thread::sleep_for(33ms);
    }
  });

  bc::Show show;
  bc::CameraConfig camera;
  camera.id = "a";
  camera.name = "Cam a";
  camera.input.kind = "omt";
  camera.input.params = {{"source", sender.address()}};
  // An OMT camera with no outputs and no preview only receives now, so a stage keeps the frames
  // (and their decode) flowing: the leak this measures is one per decoded picture.
  camera.stages["color"] = {{"enabled", true}};
  show.cameras.push_back(camera);
  engine.apply(show);
  engine.start();

  auto waitFrames = [&](int want) {
    for (int i = 0; i < 2000 && frames.load() < want; ++i) std::this_thread::sleep_for(20ms);  // 40 s
  };
  waitFrames(60);
  const int64_t start = static_cast<int64_t>(privateBytes());
  waitFrames(300);
  const int64_t end = static_cast<int64_t>(privateBytes());
  const int64_t grown = end - start;
  const int taken = frames.load();

  // The fix moves every picture from VRAM to system memory (sampled through an SRV): the frame the
  // real pipeline captures must still be the colour the source sent. Captured before stop(), while
  // the camera is live.
  const auto captured = engine.capture({"a"}, 1, 1, 64, 36, std::chrono::milliseconds(5000));

  engine.stop();
  run = false;
  if (feeder.joinable()) feeder.join();

  INFO("frames " << taken << ", start " << start << " bytes, grown " << grown << " bytes");
  REQUIRE(taken >= 300);
  REQUIRE(start > 0);
  REQUIRE(end > 0);
  CHECK(grown < 150LL * 1024 * 1024);

  REQUIRE(captured.count("a") == 1);
  REQUIRE(captured.at("a").size() == 1);
  const bc::CpuImage& pic = captured.at("a").front();
  REQUIRE(pic.width == 64);
  REQUIRE(pic.height == 36);
  const size_t centre = (static_cast<size_t>(pic.height / 2) * pic.width + pic.width / 2) * 4;
  // Colour channels: the source was sent as 100 on every channel. Plain OMT carries no alpha
  // (VideoFormat::alpha is false), so libomt returns the picture opaque; the sender's 100 alpha
  // never reaches the wire.
  for (size_t c = 0; c < 3; ++c) CHECK(std::abs(static_cast<int>(pic.bgra[centre + c]) - 100) <= 8);
  CHECK(pic.bgra[centre + 3] == 255);
}

TEST_CASE("OmtInput: memory stays flat while a camera receives 300 frames" BC_REQUIRES(bc::test::Req::Omt)
          BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  leakCase(omthx::Mode::Full, "bc-leak-test");
}

TEST_CASE("OmtInput: memory stays flat while a camera receives 300 HX frames" BC_REQUIRES(bc::test::Req::Omt)
          BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  leakCase(omthx::Mode::Hx, "bc-leak-test-hx");
}

// ---------------------------------------------------------------- pass-through

namespace {
// The real default adapters, with the OMT output and input kept for the test: the output's address
// is only reachable through the object, and the input reports its decoder's work.
class PassThroughFactory final : public bc::AdapterFactory {
 public:
  PassThroughFactory(bc::AdapterFactory& inner, omthx::Mode outputMode) : inner_(inner), mode_(outputMode) {}
  std::unique_ptr<bc::Input> makeInput(const bc::InputConfig& cfg) override {
    auto in = inner_.makeInput(cfg);
    input = dynamic_cast<bc::OmtInput*>(in.get());
    return in;
  }
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig& cfg, const bc::CameraConfig& cam,
                                         bc::Rate) override {
    const std::string name = cfg.params.value("name", std::string("BreadCamera - ") + cam.name);
    auto out = std::make_unique<bc::OmthOutput>(name, mode_, omthx::Codec::Hevc, /*keyed=*/false);
    output = out.get();
    return out;
  }
  bc::OmtInput* input = nullptr;    // valid until the camera is stopped
  bc::OmthOutput* output = nullptr;  // ditto

 private:
  bc::AdapterFactory& inner_;
  omthx::Mode mode_;
};

struct PassRig {
  explicit PassRig(omthx::Mode outputMode)
      : gpu(bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp)),
        pool(*gpu),
        conv(*gpu, pool),
        comp(*gpu, pool),
        grader(*gpu, pool),
        inner(*gpu, pool),
        factory(inner, outputMode),
        engine(bc::GpuTools{conv, comp, grader}, state, factory) {}
  std::unique_ptr<bc::GpuDevice> gpu;
  bc::TexturePool pool;
  bc::Converter conv;
  bc::Compositor comp;
  bc::ColorGrader grader;
  bc::StateStore state;
  bc::DefaultAdapterFactory inner;
  PassThroughFactory factory;
  bc::Engine engine;
};

// One camera, its OMT input fed by `source`, and one OMT output.
bc::Show passShow(const std::string& source) {
  bc::Show show;
  bc::CameraConfig c;
  c.id = "a";
  c.name = "Cam a";
  c.input.kind = "omt";
  c.input.params = {{"source", source}};
  c.outputs.push_back(bc::OutputConfig{"o1", "omt", false, true, nlohmann::json::object()});
  show.cameras.push_back(c);
  return show;
}

std::string waitForPassAddress(const bc::OmthOutput& out) {
  std::string address;
  for (int i = 0; i < 50 && address.empty(); ++i) {
    address = out.address();
    if (address.empty()) std::this_thread::sleep_for(100ms);
  }
  return address;
}

// A bright picture that changes each frame, as the test source.
void brightPattern(std::vector<uint8_t>& bgra, int i, uint32_t w, uint32_t h) {
  bgra.resize(static_cast<size_t>(w) * h * 4);
  const uint8_t k = static_cast<uint8_t>(180 + (i % 3) * 20);
  for (size_t p = 0; p < bgra.size(); p += 4) {
    bgra[p + 0] = 30;
    bgra[p + 1] = k;
    bgra[p + 2] = k;
    bgra[p + 3] = 255;
  }
}

int centreLuma(const omthx::VideoFrame& vf) {
  const size_t c = (static_cast<size_t>(vf.format.height / 2) * vf.format.width + vf.format.width / 2) * 4;
  if (vf.bgra.size() < c + 4) return -1;
  return (299 * vf.bgra[c + 2] + 587 * vf.bgra[c + 1] + 114 * vf.bgra[c + 0]) / 1000;
}

// Sends a bright pattern at 30 fps on its own thread until stopped.
class PatternFeeder {
 public:
  PatternFeeder(omthx::Sender& sender, omthx::VideoFormat fmt) : thread_([this, &sender, fmt] {
    std::vector<uint8_t> bgra;
    const std::vector<float> tone(2 * 1600, 0.2f);
    auto next = std::chrono::steady_clock::now();
    for (int i = 0; !stop_; ++i) {
      brightPattern(bgra, i, static_cast<uint32_t>(fmt.width), static_cast<uint32_t>(fmt.height));
      const int64_t ts = static_cast<int64_t>(i) * 10'000'000 / fmt.fpsN;
      sender.sendVideo(fmt, bgra, false, ts);
      sender.sendAudio(tone, 48000, 2, ts);
      next += std::chrono::microseconds(1'000'000 / fmt.fpsN);
      std::this_thread::sleep_until(next);
    }
  }) {}
  ~PatternFeeder() {
    stop_ = true;
    thread_.join();
  }

 private:
  std::atomic<bool> stop_{false};
  std::thread thread_;
};
}  // namespace

TEST_CASE("OMT pass-through: an idle preview decodes nothing" BC_REQUIRES(bc::test::Req::Omt)) {
  const std::string name = "bc-passthrough-idle-" + std::to_string(GetCurrentProcessId());
  omthx::Sender source(name, omthx::Mode::Full, omthx::Quality::Medium);
  PatternFeeder feeder(source, omthx::VideoFormat{640, 360, 30, 1, false});

  PassRig rig(omthx::Mode::Full);
  rig.engine.apply(passShow(source.address()));
  rig.engine.start();
  REQUIRE(rig.factory.output != nullptr);
  REQUIRE(rig.factory.input != nullptr);
  const std::string address = waitForPassAddress(*rig.factory.output);
  REQUIRE_FALSE(address.empty());

  omthx::Receiver rx(address);
  omthx::VideoFrame vf;
  bool got = false;
  for (int i = 0; i < 100 && !got; ++i) got = rx.receiveVideo(vf, 100);
  CHECK(got);  // the pass-through output delivers the source's frames
  std::this_thread::sleep_for(600ms);  // let the camera settle into pass-through
  const uint64_t decoded = rig.factory.input->framesDecoded();
  std::this_thread::sleep_for(1000ms);
  CHECK(rig.factory.input->framesDecoded() == decoded);  // nothing needs a picture, so nothing decodes
  rig.engine.stop();
}

TEST_CASE("OMT pass-through: a late receiver gets a picture" BC_REQUIRES(bc::test::Req::Omt)) {
  const std::string name = "bc-passthrough-late-" + std::to_string(GetCurrentProcessId());
  omthx::Sender source(name, omthx::Mode::Full, omthx::Quality::Medium);
  PatternFeeder feeder(source, omthx::VideoFormat{640, 360, 30, 1, false});

  PassRig rig(omthx::Mode::Full);
  rig.engine.apply(passShow(source.address()));
  rig.engine.start();
  REQUIRE(rig.factory.output != nullptr);
  const std::string address = waitForPassAddress(*rig.factory.output);
  REQUIRE_FALSE(address.empty());

  std::this_thread::sleep_for(2s);  // the stream runs with nobody watching
  omthx::Receiver rx(address);
  omthx::VideoFrame vf;
  bool got = false;
  for (int i = 0; i < 10 && !got; ++i) got = rx.receiveVideo(vf, 100);  // within one second
  CHECK(got);
  rig.engine.stop();
}

TEST_CASE("OMT pass-through: a late receiver of an HX source gets a keyframe relayed"
          BC_REQUIRES(bc::test::Req::Omt) BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  const std::string name = "bc-passthrough-late-hx-" + std::to_string(GetCurrentProcessId());
  omthx::Sender source(name, omthx::Mode::Hx, omthx::Quality::Medium, omthx::Codec::Hevc);
  PatternFeeder feeder(source, omthx::VideoFormat{640, 360, 30, 1, false});

  PassRig rig(omthx::Mode::Hx);
  rig.engine.apply(passShow(source.address()));
  rig.engine.start();
  REQUIRE(rig.factory.output != nullptr);
  REQUIRE(rig.factory.input != nullptr);
  const std::string address = waitForPassAddress(*rig.factory.output);
  REQUIRE_FALSE(address.empty());

  std::this_thread::sleep_for(2s);  // mid-GOP: the source's next keyframe is 10 s away on its own
  const uint64_t decoded = rig.factory.input->framesDecoded();
  omthx::Receiver rx(address);
  omthx::VideoFrame vf;
  bool got = false;
  for (int i = 0; i < 10 && !got; ++i) got = rx.receiveVideo(vf, 100);  // within one second
  CHECK(got);                                          // the keyframe request was relayed back
  CHECK(rig.factory.input->framesDecoded() == decoded);  // still relayed, not decoded locally
  rig.engine.stop();
}

TEST_CASE("OMT pass-through: audio flows" BC_REQUIRES(bc::test::Req::Omt)) {
  const std::string name = "bc-passthrough-audio-" + std::to_string(GetCurrentProcessId());
  omthx::Sender source(name, omthx::Mode::Full, omthx::Quality::Medium);
  PatternFeeder feeder(source, omthx::VideoFormat{640, 360, 30, 1, false});

  PassRig rig(omthx::Mode::Full);
  rig.engine.apply(passShow(source.address()));
  rig.engine.start();
  REQUIRE(rig.factory.output != nullptr);
  const std::string address = waitForPassAddress(*rig.factory.output);
  REQUIRE_FALSE(address.empty());

  omthx::Receiver rx(address);
  int64_t videoTs = -1;
  int64_t audioTs = -1;
  std::vector<int64_t> videoStamps;
  for (int i = 0; i < 100 && (videoTs < 0 || audioTs < 0); ++i) {
    omthx::VideoFrame vf;
    if (rx.receiveVideo(vf, 20)) {
      videoTs = vf.timestamp;
      videoStamps.push_back(videoTs);
    }
    omthx::AudioFrame af;
    if (rx.receiveAudio(af, 0)) audioTs = af.timestamp;
  }
  REQUIRE(videoTs >= 0);
  REQUIRE(audioTs >= 0);  // the tone keeps flowing on the pass-through output
  int64_t nearest = videoStamps.front();
  for (int64_t t : videoStamps) {
    if (std::abs(t - audioTs) < std::abs(nearest - audioTs)) nearest = t;
  }
  CHECK(std::abs(nearest - audioTs) <= 10'000'000 / 30);  // one frame period
  rig.engine.stop();
}

TEST_CASE("OMT pass-through: an effect turned on mid-stream sends no black"
          BC_REQUIRES(bc::test::Req::Omt) BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  const std::string name = "bc-passthrough-effect-" + std::to_string(GetCurrentProcessId());
  omthx::Sender source(name, omthx::Mode::Hx, omthx::Quality::Medium, omthx::Codec::Hevc);
  PatternFeeder feeder(source, omthx::VideoFormat{640, 360, 30, 1, false});

  PassRig rig(omthx::Mode::Hx);
  bc::Show show = passShow(source.address());
  rig.engine.apply(show);
  rig.engine.start();
  REQUIRE(rig.factory.output != nullptr);
  const std::string address = waitForPassAddress(*rig.factory.output);
  REQUIRE_FALSE(address.empty());

  std::vector<int> lumas;
  std::string encoder;
  uint64_t decodedAfterSwitch = 0;
  {
    omthx::Receiver rx(address);
    const auto start = std::chrono::steady_clock::now();
    bool switched = false;
    while (std::chrono::steady_clock::now() - start < 3s) {
      omthx::VideoFrame vf;
      if (rx.receiveVideo(vf, 20)) lumas.push_back(centreLuma(vf));
      if (!switched && std::chrono::steady_clock::now() - start > 1s) {
        switched = true;
        show.cameras[0].stages["color"] = {{"enabled", true}};
        rig.engine.apply(show);  // the effect comes on mid-stream
        decodedAfterSwitch = rig.factory.input->framesDecoded();
      }
    }
    encoder = rx.stats().encoder;
  }
  rig.engine.stop();
  REQUIRE(lumas.size() > 30);
  for (int y : lumas) CHECK(y > 120);  // never black, and never the dark slate (about 75)
  CHECK(encoder != "none");  // after the switch the tick path encodes again
  CHECK(rig.factory.input->framesDecoded() > decodedAfterSwitch);  // and decodes the pictures it sends
}

// Review focus 1 and 3: nothing that uses the pictures, so nothing decodes; a preview brings
// the decoded pictures back within a second.
TEST_CASE("OMT pass-through: a camera with no consumer only receives"
          BC_REQUIRES(bc::test::Req::Omt) BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  const std::string name = "bc-idle-omt-" + std::to_string(GetCurrentProcessId());
  omthx::Sender source(name, omthx::Mode::Hx, omthx::Quality::Medium, omthx::Codec::Hevc);
  PatternFeeder feeder(source, omthx::VideoFormat{640, 360, 30, 1, false});

  std::atomic<bool> preview{false};
  std::atomic<uint64_t> ticks{0};  // frames handed to the tick path's tap
  PassRig rig(omthx::Mode::Hx);
  rig.engine.setPreviewWanted([&](const std::string&) { return preview.load(); });
  rig.engine.setFrameTap([&](uint64_t, const std::vector<bc::FrameTap>& frames) { ticks += frames.size(); });
  bc::Show show = passShow(source.address());
  show.cameras[0].outputs.clear();
  rig.engine.apply(show);
  rig.engine.start();
  REQUIRE(rig.factory.input != nullptr);

  std::this_thread::sleep_for(1500ms);  // connect, learn the source, settle
  const uint64_t decoded = rig.factory.input->framesDecoded();
  const uint64_t ticked = ticks.load();
  std::this_thread::sleep_for(1000ms);
  CHECK(ticks.load() == ticked);                        // no tick runs for a camera nobody watches
  CHECK(rig.factory.input->framesDecoded() == decoded);  // and its own decoder never runs
  CHECK(rig.engine.stats().cameras.at("a").fps > 0);     // the coded frames are its rate
  const auto snap = rig.state.snapshot();
  CHECK(snap.at(nlohmann::json::json_pointer("/cameras/a/input/state")) == "live");
  CHECK(snap.at(nlohmann::json::json_pointer("/cameras/a/phase")) == bc::toString(bc::FeedPhase::Live));
  CHECK(snap.at(nlohmann::json::json_pointer("/cameras/a/input/health")) == "receiving");

  preview = true;  // the preview comes on: pictures resume within a second
  bool grew = false;
  for (int i = 0; i < 20 && !grew; ++i) {
    std::this_thread::sleep_for(50ms);
    grew = rig.factory.input->framesDecoded() > decoded;
  }
  CHECK(grew);
  rig.engine.stop();
}

// Review focus 2: an output added to the idle camera gets pictures within two seconds.
TEST_CASE("OMT pass-through: an output added to an idle camera gets pictures"
          BC_REQUIRES(bc::test::Req::Omt) BC_REQUIRES(bc::test::Req::HardwareGpu)) {
  const std::string name = "bc-idle-omt-out-" + std::to_string(GetCurrentProcessId());
  omthx::Sender source(name, omthx::Mode::Hx, omthx::Quality::Medium, omthx::Codec::Hevc);
  PatternFeeder feeder(source, omthx::VideoFormat{640, 360, 30, 1, false});

  std::atomic<uint64_t> ticks{0};
  PassRig rig(omthx::Mode::Hx);
  rig.engine.setFrameTap([&](uint64_t, const std::vector<bc::FrameTap>& frames) { ticks += frames.size(); });
  bc::Show show = passShow(source.address());
  show.cameras[0].outputs.clear();
  rig.engine.apply(show);
  rig.engine.start();
  REQUIRE(rig.factory.input != nullptr);
  std::this_thread::sleep_for(1500ms);  // it only receives, so it does not decode
  const uint64_t ticked = ticks.load();
  std::this_thread::sleep_for(500ms);
  REQUIRE(ticks.load() == ticked);
  const uint64_t decoded = rig.factory.input->framesDecoded();

  show.cameras[0].outputs.push_back(bc::OutputConfig{"o1", "omt", false, true, nlohmann::json::object()});
  rig.engine.apply(show);
  REQUIRE(rig.factory.output != nullptr);
  const std::string address = waitForPassAddress(*rig.factory.output);
  REQUIRE_FALSE(address.empty());
  omthx::Receiver rx(address);
  omthx::VideoFrame vf;
  bool got = false;
  for (int i = 0; i < 20 && !got; ++i) got = rx.receiveVideo(vf, 100);  // within two seconds
  CHECK(got);
  // The output takes the coded frames the camera was already receiving: nothing reopens or decodes.
  CHECK(rig.factory.input->framesDecoded() == decoded);
  rig.engine.stop();
}

TEST_CASE("AdapterFactory: OMT quality" BC_REQUIRES(bc::test::Req::Omt)) {
  auto gpu = bc::GpuDevice::create(bc::GpuDevice::Pref::HardwareOrWarp);
  bc::TexturePool pool{*gpu};
  bc::DefaultAdapterFactory factory{*gpu, pool};
  bc::CameraConfig cam;
  cam.id = "a";
  cam.name = "Cam a";
  bc::OutputConfig o;
  o.id = "o1";
  o.kind = "omt";

  o.params = {{"mode", "hx"}, {"quality", "high"}};
  auto high = factory.makeOutput(o, cam, {60, 1});
  CHECK(dynamic_cast<bc::OmthOutput*>(high.get())->quality() == omthx::Quality::High);

  o.params = {{"mode", "hx"}};
  auto medium = factory.makeOutput(o, cam, {60, 1});
  CHECK(dynamic_cast<bc::OmthOutput*>(medium.get())->quality() == omthx::Quality::Medium);

  // Plain OMT (full) uses the quality too, unlike the codec.
  o.params = {{"mode", "full"}, {"quality", "low"}};
  auto full = factory.makeOutput(o, cam, {60, 1});
  CHECK(dynamic_cast<bc::OmthOutput*>(full.get())->quality() == omthx::Quality::Low);

  o.params = {{"mode", "hx"}, {"quality", 2}};
  CHECK_THROWS_WITH_AS(factory.makeOutput(o, cam, {60, 1}), "OMT quality must be text", std::invalid_argument);

  o.params = {{"mode", "full"}, {"quality", "ultra"}};
  CHECK_THROWS_WITH_AS(factory.makeOutput(o, cam, {60, 1}),
                       "OMT quality must be 'low', 'medium' or 'high'", std::invalid_argument);
}

#endif
