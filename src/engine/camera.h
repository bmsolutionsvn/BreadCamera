#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
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
#include "engine/gpu_tools.h"
#include "gpu/compositor.h"
#include "gpu/converter.h"
#include "gpu/readback.h"

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
  Camera(CameraConfig cfg, AdapterFactory& factory, GpuTools tools, StateStore& state, SyncPolicy policy);
  ~Camera() override;
  const CameraConfig& config() const { return cfg_; }
  void start();
  void stop();
  void reconfigure(const CameraConfig& cfg);
  void setPolicy(SyncPolicy p) { sync_.setPolicy(p); }
  // First half (tick thread): newest frame → linear texture at its own size, or the slate fitted
  // to the camera's size. Never null.
  TextureRef beginTick(std::chrono::steady_clock::time_point now, const TextureRef& slateSource);
  // This tick's linear frame, between the halves.
  TextureRef tickFrame() const { return tickFrame_; }
  // This tick's frame before any stage (the slate when there is no picture).
  TextureRef tickRaw() const { return tickRaw_; }
  // True when this camera's matte is enabled and its guard lets it run this tick.
  bool wantsMatte(std::chrono::steady_clock::time_point now) const;
  const nlohmann::json& matteParams() const { return matteParams_; }
  const nlohmann::json& backgroundParams() const { return backgroundParams_; }
  // Second half: rawAlpha (null = no matte this tick) → stages → readback → fan-out.
  // previewWanted forces the one readback even when no output needsCpu(), so a visible preview
  // (D5) still gets the image through the frame tap.
  void endTick(uint64_t index, const TickSchedule& sched, std::chrono::steady_clock::time_point now,
               const TextureRef& rawAlpha, bool previewWanted = false);
  StageGuard& matteGuard() { return matteGuard_; }
  StageGuard& backgroundGuard() { return backgroundGuard_; }
  StageGuard& colorGuard() { return colorGuard_; }
  FeedPhase tickPhase() const { return tickPhase_; }
  TextureRef lastFrame() const { return lastFrame_; }
  // This tick's single readback, the image shared by every CPU output and the frame tap. Null
  // until the first async copy finishes, and null when no CPU consumer wanted it this tick.
  std::shared_ptr<const CpuImage> tickCpu() const { return tickCpu_; }
  void publishStatus();
  CameraStats stats() const;
  // bcam-readback: the one readback slot (0 when no output needs CPU and no preview wants it).
  // Diagnostic and test hook for the "no CPU consumer does no readback work" contract.
  size_t readbackSlotCount() const { return readback_ ? 1 : 0; }

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
  // One in-flight readback plus the newest completed image. image is null until the first copy
  // finishes (never a synthetic frame).
  struct ReadbackState {
    ReadbackSlot slot;
    std::shared_ptr<const CpuImage> image;
  };
  void startInput();
  void startOutput(OutputSlot& slot);
  void loadStageParams();
  // The slate source fitted to the camera's last frame size, cached per (source, size). Only runs
  // on the no-picture path, so it costs nothing while the camera is live.
  TextureRef fittedSlate(const TextureRef& source);
  std::string path(const std::string& rest) const { return "/cameras/" + cfg_.id + rest; }

  CameraConfig cfg_;
  AdapterFactory& factory_;
  GpuTools tools_;
  StateStore& state_;
  FrameSync<Pending> sync_;
  AudioDelay audio_{48000, 2};
  std::unique_ptr<Input> input_;
  std::vector<OutputSlot> outputs_;
  TextureRef lastConverted_;
  TextureRef lastFrame_;
  std::chrono::milliseconds audioDelaySet_{-1};
  // The camera's own frame size: the last converted input's size, or 1280x720 before any frame.
  struct {
    uint32_t w = 1280, h = 720;
  } lastSize_;
  std::weak_ptr<GpuTexture> slateFitSrc_;  // the source the fitted slate was made from
  TextureRef slateFit_;                    // the slate source fitted to lastSize_
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
  nlohmann::json colorParams_;
  ColorGrade grade_;
  std::string lutPath_;
  std::optional<ErrorInfo> lutError_;
  StageGuard matteGuard_;
  StageGuard backgroundGuard_;
  StageGuard colorGuard_;
  TextureRef bgImage_;               // linear RGBA16F replace background, or null
  std::string bgImagePath_;          // the loaded image's path, to skip reloading
  std::optional<ErrorInfo> bgImageError_;
  TextureRef tickFrame_;  // this tick's linear frame, between the halves
  TextureRef tickRaw_;    // this tick's frame before any stage
  TextureRef lastGraded_;
  bool gradeDirty_ = true;
  std::optional<std::chrono::steady_clock::time_point> tickArrival_;
  FeedPhase tickPhase_ = FeedPhase::NoSignal;
  // The single readback in flight and the last completed frame, shared by every CPU output and
  // (through the tap) the preview. Absent when neither an output needs CPU nor a visible preview
  // wants it, so such a camera does no readback work.
  std::optional<ReadbackState> readback_;
  std::shared_ptr<const CpuImage> tickCpu_;  // this tick's shared image, for the frame tap
};
}  // namespace bc
