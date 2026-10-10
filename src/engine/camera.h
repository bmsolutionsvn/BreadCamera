#pragma once
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/audio_delay.h"
#include "core/frame_sync.h"
#include "core/input_health.h"
#include "core/show.h"
#include "core/stage_guard.h"
#include "core/stage_params.h"
#include "core/state_store.h"
#include "core/tick_schedule.h"
#include "engine/adapter_factory.h"
#include "gpu/converter.h"

namespace bc {
struct CameraStats {
  double fps = 0;
  uint64_t drops = 0;
  uint64_t repeats = 0;
  double latencyMs = 0;
  FeedPhase phase = FeedPhase::NoSignal;
};

// One camera: input → frame sync → convert → fan-out (spec §3.2). tick() runs on the
// engine's tick thread; the InputSink callbacks run on the input thread.
class Camera final : public InputSink {
 public:
  Camera(CameraConfig cfg, AdapterFactory& factory, Converter& conv, StateStore& state, SyncPolicy policy);
  ~Camera() override;
  const CameraConfig& config() const { return cfg_; }
  void start();
  void stop();
  void reconfigure(const CameraConfig& cfg);
  void setPolicy(SyncPolicy p) { sync_.setPolicy(p); }
  // The show's frame size; called from the tick thread's lock (Engine holds mutex_).
  void setFormat(uint32_t w, uint32_t h) {
    width_ = w;
    height_ = h;
    lastConverted_.reset();
  }
  // First half (tick thread): newest frame → linear texture, or the slate. Never null.
  TextureRef beginTick(std::chrono::steady_clock::time_point now, const TextureRef& slate);
  // True when this camera's matte is enabled and its guard lets it run this tick.
  bool wantsMatte(std::chrono::steady_clock::time_point now) const;
  const nlohmann::json& matteParams() const { return matteParams_; }
  const nlohmann::json& backgroundParams() const { return backgroundParams_; }
  // Second half: rawAlpha (null = no matte this tick) → stages → readback → fan-out.
  void endTick(uint64_t index, const TickSchedule& sched, std::chrono::steady_clock::time_point now,
               const TextureRef& rawAlpha);
  StageGuard& matteGuard() { return matteGuard_; }
  StageGuard& backgroundGuard() { return backgroundGuard_; }
  TextureRef lastFrame() const { return lastFrame_; }
  void publishStatus();
  CameraStats stats() const;

  void onVideo(VideoFrameIn frame) override;
  void onAudio(AudioBlock audio) override;
  void onState(SignalState state, std::optional<ErrorInfo> error) override;

 private:
  struct Pending {
    VideoFrameIn frame;
    std::chrono::steady_clock::time_point arrival;
  };
  struct OutputSlot {
    OutputConfig cfg;
    std::unique_ptr<Output> out;
    std::optional<ErrorInfo> createError;
    std::vector<float> carryAudio;  // half-rate audio from the odd tick
  };
  void startInput();
  void startOutput(OutputSlot& slot);
  void loadStageParams();
  std::string path(const std::string& rest) const { return "/cameras/" + cfg_.id + rest; }

  CameraConfig cfg_;
  AdapterFactory& factory_;
  Converter& conv_;
  StateStore& state_;
  FrameSync<Pending> sync_;
  AudioDelay audio_{48000, 2};
  std::unique_ptr<Input> input_;
  std::vector<OutputSlot> outputs_;
  TextureRef lastConverted_;
  TextureRef lastFrame_;
  std::chrono::milliseconds audioDelaySet_{-1};
  uint32_t width_ = 1920, height_ = 1080;
  std::atomic<SignalState> inputState_{SignalState::Connecting};
  std::optional<std::chrono::steady_clock::time_point> lastFresh_;
  std::chrono::steady_clock::time_point lastStutter_{};
  mutable std::mutex statsMutex_;
  double latencyEwmaMs_ = 0;
  uint64_t freshThisSecond_ = 0;
  double fps_ = 0;
  FeedPhase phase_ = FeedPhase::NoSignal;
  nlohmann::json matteParams_;
  nlohmann::json backgroundParams_;
  StageGuard matteGuard_;
  StageGuard backgroundGuard_;
  TextureRef tickFrame_;  // this tick's linear frame, between the halves
  std::optional<std::chrono::steady_clock::time_point> tickArrival_;
  FeedPhase tickPhase_ = FeedPhase::NoSignal;
};
}  // namespace bc
