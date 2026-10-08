#pragma once
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "engine/adapter_factory.h"
#include "gpu/converter.h"
#include "support/fake_output.h"

namespace bc::test {
// Pushes solid BGRA frames + matching audio at `fps` until paused or stopped.
class FakeInput final : public bc::Input {
 public:
  FakeInput(bc::Converter& conv, int fps, uint32_t w, uint32_t h, std::array<uint8_t, 4> bgra,
            bool startPaused = false)
      : conv_(conv), fps_(fps), color_(bgra) {
    width = w;
    height = h;
    paused = startPaused;  // construction-time; re-read in start(), before any thread runs
  }
  ~FakeInput() override { stop(); }
  std::atomic<bool> paused{false};
  std::atomic<uint32_t> width{0}, height{0};

  void start(bc::InputSink& sink) override {
    running_ = true;
    // The construction-time pause decides here, synchronously, before the thread exists:
    // a paused input pushes nothing (never any frame), an unpaused one pushes its first
    // frame right now, so the engine's tick 0 finds it already in the FrameSync.
    const bool startPaused = paused.load();
    if (!startPaused) push(sink, 0);
    thread_ = std::thread([this, &sink, startPaused] {
      sink.onState(bc::SignalState::Live, std::nullopt);
      int64_t n = startPaused ? 0 : 1;
      auto t0 = std::chrono::steady_clock::now();
      while (running_) {
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(n * 1'000'000 / fps_));
        if (running_ && !paused) push(sink, n);
        ++n;
      }
    });
  }
  void stop() override {
    running_ = false;
    if (thread_.joinable()) thread_.join();
  }

 private:
  void push(bc::InputSink& sink, int64_t n) {
    const uint32_t w = width, h = height;
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) std::copy(color_.begin(), color_.end(), px.begin() + i);
    bc::VideoFrameIn f{conv_.uploadBgra8(px.data(), w, h, w * 4), bc::PixelLayout::BGRA8, true, n * 1'000'000 / fps_};
    sink.onVideo(std::move(f));
    sink.onAudio(bc::AudioBlock{48000, 2, std::vector<float>(48000 / fps_ * 2, 0.1f)});
  }

  bc::Converter& conv_;
  int fps_;
  std::array<uint8_t, 4> color_;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

// Input kind "fake": params {tag, fps, width, height, bgra:[b,g,r,a], paused (bool, default
// false; a paused input pushes no frame at all, not even its first)}.
// Output kind "fake": params {cpu (bool, default true), delayMs}. Anything else throws.
class FakeFactory final : public bc::AdapterFactory {
 public:
  explicit FakeFactory(bc::Converter& conv) : conv_(conv) {}
  std::unique_ptr<bc::Input> makeInput(const bc::InputConfig& cfg) override {
    if (cfg.kind != "fake") throw std::invalid_argument("unknown input kind " + cfg.kind);
    auto& p = cfg.params;
    auto c = p.value("bgra", std::vector<int>{0, 0, 200, 255});
    auto in = std::make_unique<FakeInput>(conv_, p.value("fps", 30), p.value("width", 64u), p.value("height", 36u),
                                          std::array<uint8_t, 4>{uint8_t(c[0]), uint8_t(c[1]), uint8_t(c[2]), uint8_t(c[3])},
                                          p.value("paused", false));
    std::lock_guard l(m);
    inputs[p.value("tag", std::string("in"))] = in.get();
    return in;
  }
  std::unique_ptr<bc::Output> makeOutput(const bc::OutputConfig& cfg, const bc::CameraConfig&) override {
    if (cfg.kind != "fake") throw std::invalid_argument("unknown output kind " + cfg.kind);
    auto out = std::make_unique<FakeOutput>(cfg.params.value("cpu", true));
    out->delayMs = cfg.params.value("delayMs", 0);
    std::lock_guard l(m);
    outputs[cfg.id] = out.get();
    return out;
  }
  FakeInput* input(const std::string& tag) {
    std::lock_guard l(m);
    return inputs.at(tag);
  }
  FakeOutput* output(const std::string& id) {
    std::lock_guard l(m);
    return outputs.at(id);
  }

 private:
  bc::Converter& conv_;
  std::mutex m;
  std::map<std::string, FakeInput*> inputs;
  std::map<std::string, FakeOutput*> outputs;
};
}  // namespace bc::test
