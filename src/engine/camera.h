#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
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
#include "core/rate_estimator.h"
#include "core/show.h"
#include "core/skin_match.h"
#include "core/stage_guard.h"
#include "core/stage_params.h"
#include "core/state_store.h"
#include "core/tick_schedule.h"
#include "engine/adapter_factory.h"
#include "engine/gpu_tools.h"
#include "gpu/compositor.h"
#include "gpu/converter.h"
#include "gpu/gpu_timer.h"
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
  void setPolicy(SyncPolicy p) {
    policy_ = p;
    sync_.setPolicy(p);
  }
  // The Project rate is the effect-budget reference; it never paces the camera.
  void setBudgetRate(Rate r) { budgetRate_ = r; }
  Rate projectRate() const { return budgetRate_; }
  // The input's declared rate, or the measured one, or the Project rate before either is known.
  Rate nominalRate() const {
    const uint64_t p = nominalPacked_.load();
    if (!p) return budgetRate_;
    const int num = static_cast<int>(p >> 32);
    const int den = static_cast<int>(p & 0xffffffffu);
    if (num <= 0 || den <= 0) return budgetRate_;
    return Rate{num, den};
  }
  bool hasNominalRate() const { return nominalPacked_.load() != 0; }
  // Wakes the engine when an input frame arrives (called from the input thread).
  void setWake(std::function<void()> wake) { wake_ = std::move(wake); }
  // True when the engine should process this camera now: a new input frame is pending, or a
  // hold/slate repeat is due.
  bool ready(std::chrono::steady_clock::time_point now) const;
  std::optional<std::chrono::steady_clock::time_point> nextRepeatAt(
      std::chrono::steady_clock::time_point now) const;
  // The frame's wall-clock tick: a fresh frame covers every input interval superseded since the
  // last taken one; a held/slate repeat covers one. Audio pulls the same number of intervals.
  static uint64_t stampTick(uint64_t current, uint64_t taken, uint64_t seq, bool fresh) {
    return fresh ? current + (seq - taken - 1) : current;
  }
  // The monotonic index of the last emitted frame.
  uint64_t emitIndex() const { return emitIndex_; }
  uint64_t tickIndex() const { return tickIndex_; }
  // First half (tick thread): newest frame → linear texture at its own size, or the slate fitted
  // to the camera's size. Never null.
  TextureRef beginTick(std::chrono::steady_clock::time_point now, const TextureRef& slateSource);
  // This tick's linear frame, between the halves.
  TextureRef tickFrame() const { return tickFrame_; }
  // Upscale (spec D6): the frame at the target size, from the fallback chain RTX (Task 4) →
  // tools_.up->apply (FSR 1) → conv.fit. Cached and reused while !fresh and the size matches;
  // never changes size when the stage is bypassed (D3a).
  TextureRef enhance(const TextureRef& in, bool fresh, std::chrono::steady_clock::time_point now);
  // This tick's frame before any stage (the slate when there is no picture).
  TextureRef tickRaw() const { return tickRaw_; }
  // True when this camera's matte is enabled and its guard lets it run this tick.
  bool wantsMatte(std::chrono::steady_clock::time_point now) const;
  const nlohmann::json& matteParams() const { return matteParams_; }
  const nlohmann::json& backgroundParams() const { return backgroundParams_; }
  const nlohmann::json& colorParams() const { return colorParams_; }
  // Match (spec D9-D16): a camera matches another in the Engine's tick, which reads these. The
  // search of skin statistics and the eased correction stay entirely in the camera; the tick is
  // never blocked (the readbacks are 128x72 and asynchronous) and stage params are never written.
  bool matchMode() const;
  std::string matchTo() const;
  // The last measured skin statistics of the frame leaving Colour — what a camera matching this
  // one reads (with the previous tick's matte as the alpha mask).
  std::optional<SkinStats> matchOutputStats() const { return matchOutput_; }
  void setMatchReference(std::optional<SkinStats> ref, bool refHasPicture);
  // True while another camera matches this one: it measures its output even out of Match mode.
  void setMatchedBy(bool matched) { matchedBy_ = matched; }
  // Second half: rawAlpha (null = no matte this tick) → stages → readback → fan-out.
  // previewWanted forces the one readback even when no output needsCpu(), so a visible preview
  // (D5) still gets the image through the frame tap.
  void endTick(std::chrono::steady_clock::time_point now, const TextureRef& rawAlpha,
               bool previewWanted = false);
  StageGuard& matteGuard() { return matteGuard_; }
  StageGuard& backgroundGuard() { return backgroundGuard_; }
  StageGuard& colorGuard() { return colorGuard_; }
  StageGuard& upscaleGuard() { return upscaleGuard_; }
  StageGuard& cleanupGuard() { return cleanupGuard_; }
  // The last GPU measurement for a stage in ms, 0 when none (spec D14).
  double stageGpuMs(const std::string& kind) const;
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
    uint64_t seq = 0;
  };
  struct OutputSlot {
    OutputConfig cfg;
    std::unique_ptr<Output> out;
    std::optional<ErrorInfo> createError;
    std::vector<float> carryAudio;  // half-rate audio from the odd frame
    uint64_t emitted = 0;           // frames submitted to this output
    uint64_t halfTick = 0;          // the half-rate output's own consecutive tick
    Rate outRate{0, 1};             // the SDI output's fixed rate; 0 until it starts
    bool deferred = false;          // SDI waits for the camera's first frame
  };
  // One in-flight readback plus the newest completed image. image is null until the first copy
  // finishes (never a synthetic frame).
  struct ReadbackState {
    ReadbackSlot slot;
    std::shared_ptr<const CpuImage> image;
  };
  // One Match measurement in flight: up to three tiny readbacks (entering, leaving, alpha).
  struct MatchRound {
    ReadbackSlot in, out, alpha;
    bool wantIn = false, wantOut = false, wantAlpha = false;
    CpuImage inImg, outImg, alphaImg;
  };
  void startInput();
  void startOutput(OutputSlot& slot);
  void loadStageParams();
  // Poll each stage's GpuTimer once a tick; a finished measurement feeds that stage's
  // guard (spec D14). A no-op when tools_.gpu is null (the stages time themselves on CPU).
  void pollColorTimer(std::chrono::steady_clock::time_point now);
  void pollBackgroundTimer(std::chrono::steady_clock::time_point now);
  void pollUpscaleTimer(std::chrono::steady_clock::time_point now);
  void pollCleanupTimer(std::chrono::steady_clock::time_point now);
  // Match (Task 7): the grade for Match mode — the params' grade without the LUT, plus the eased
  // correction. Calibrate keeps the stored LUT (rebuilt by loadStageParams).
  ColorGrade matchGrade() const;
  // Finish any in-flight 128x72 measurement without waiting, and ease the correction when the
  // round is complete. Called once per tick before the frame is picked.
  void finishMatchRound(std::chrono::steady_clock::time_point now);
  // Begin a measurement round every 6th tick: the frame entering Colour, the frame leaving it, and
  // the previous tick's refined matte. The copies are cheap dispatches; the map happens later.
  void beginMatchRound(const TextureRef& entering);
  // Solve a new target (or hold) and ease towards it. `own` is the entering frame's statistics.
  void updateMatchTarget(const std::optional<SkinStats>& own, std::chrono::steady_clock::time_point now);
  // The slate source fitted to the camera's last frame size, cached per (source, size). Only runs
  // on the no-picture path, so it costs nothing while the camera is live.
  TextureRef fittedSlate(const TextureRef& source);
  std::string path(const std::string& rest) const { return "/cameras/" + cfg_.id + rest; }

  CameraConfig cfg_;
  AdapterFactory& factory_;
  GpuTools tools_;
  StateStore& state_;
  FrameSync<Pending> sync_;
  SyncPolicy policy_;
  std::atomic<int64_t> lastArrivalNs_{0};  // steady_clock ns; 0 = never
  std::optional<std::chrono::steady_clock::time_point> nextRepeat_;
  TickSchedule audioSched_{Rate{60, 1}};
  uint64_t audioFirstTick_ = 0;
  uint32_t audioIntervals_ = 1;
  Rate budgetRate_{60, 1};
  std::atomic<bool> rateStale_{false};  // a new input session is renegotiating its rate
  std::atomic<uint64_t> nominalPacked_{0};  // packed Rate; 0 = not known yet
  RateEstimator estimator_;
  std::function<void()> wake_;
  std::atomic<uint64_t> arrivals_{0};
  uint64_t taken_ = 0;
  uint64_t emitIndex_ = 0;
  uint64_t tickIndex_ = 0;
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
  nlohmann::json upscaleParams_;
  nlohmann::json cleanupParams_;
  uint32_t upscaleTargetH_ = 1080;  // the Upscale target height: 720 or 1080 (spec D2)
  bool sawFrame_ = false;           // a frame has been converted at least once (the slate fallback)
  std::chrono::nanoseconds upscaleBudget_{4000000};  // sched.rate().period() / 4, for the CPU-fit path
  std::chrono::nanoseconds cleanupBudget_{4000000};  // sched.rate().period() / 4, for the RTX path
  ColorGrade grade_;
  std::string lutPath_;
  std::optional<ErrorInfo> lutError_;
  StageGuard matteGuard_;
  StageGuard backgroundGuard_;
  StageGuard colorGuard_;
  StageGuard upscaleGuard_;
  StageGuard cleanupGuard_;
  std::optional<GpuTimer> colorTimer_;       // present when tools_.gpu is set (spec D14)
  std::optional<GpuTimer> backgroundTimer_;
  std::optional<GpuTimer> upscaleTimer_;
  std::optional<GpuTimer> cleanupTimer_;
  double colorGpuMs_ = 0;       // last GPU measurement per stage, in ms
  double backgroundGpuMs_ = 0;
  double upscaleGpuMs_ = 0;
  double cleanupGpuMs_ = 0;
  TextureRef bgImage_;               // linear RGBA16F replace background, or null
  std::string bgImagePath_;          // the loaded image's path, to skip reloading
  std::optional<ErrorInfo> bgImageError_;
  TextureRef tickFrame_;  // this tick's linear frame, between the halves
  TextureRef tickRaw_;    // this tick's frame before any stage
  TextureRef lastEnhanced_;  // the last Upscale result, reused on a repeat tick
  TextureRef lastGraded_;
  bool gradeDirty_ = true;
  std::optional<std::chrono::steady_clock::time_point> tickArrival_;
  FeedPhase tickPhase_ = FeedPhase::NoSignal;
  // The single readback in flight and the last completed frame, shared by every CPU output and
  // (through the tap) the preview. Absent when neither an output needs CPU nor a visible preview
  // wants it, so such a camera does no readback work.
  std::optional<ReadbackState> readback_;
  std::shared_ptr<const CpuImage> tickCpu_;  // this tick's shared image, for the frame tap
  // Match state (Task 7). All tick-thread only.
  uint64_t matchTick_ = 0;                             // beginTick counter, for the every-6th cadence
  std::optional<MatchRound> matchRound_;               // one measurement at a time
  int matchRoundAge_ = 0;                              // ticks the current round has been in flight
  bool matchedBy_ = false;                             // another camera matches this one
  MatchCorrection correction_;                         // the eased correction the grade carries
  MatchCorrection matchTarget_;                        // the last solved target
  std::optional<SkinStats> matchOwn_;                  // the entering frame's statistics
  std::optional<SkinStats> matchRef_;                  // the reference camera's output statistics
  std::optional<SkinStats> matchOutput_;               // this camera's output statistics (for others)
  bool matchRefLive_ = false;                          // the reference had a live picture
  std::optional<std::chrono::steady_clock::time_point> lastMatchAt_;
  TextureRef lastAlpha_;  // the previous tick's refined matte, for the skin mask
};
}  // namespace bc
