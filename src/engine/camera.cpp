#include "engine/camera.h"

#include <cmath>
#include <filesystem>
#include <map>

#include "core/lut.h"
#include "core/utf8.h"
#include "gpu/color_grader.h"
#include "gpu/image_io.h"

namespace bc {
using Clock = std::chrono::steady_clock;

Camera::Camera(CameraConfig cfg, AdapterFactory& factory, GpuTools tools, StateStore& state, SyncPolicy policy)
    : cfg_(std::move(cfg)), factory_(factory), tools_(tools), state_(state), sync_(policy) {
  loadStageParams();
  if (tools_.gpu) {
    // Per-stage GPU timing (spec D14). A query failure falls back to the CPU timing.
    try {
      colorTimer_.emplace(*tools_.gpu);
      backgroundTimer_.emplace(*tools_.gpu);
    } catch (const std::exception&) {
      colorTimer_.reset();
      backgroundTimer_.reset();
    }
  }
}

Camera::~Camera() { stop(); }

void Camera::loadStageParams() {
  colorParams_ = normalizeStageParams("color", cfg_.stages.value("color", nlohmann::json::object()));
  auto lutKeep = grade_.lut;
  grade_ = gradeFrom(colorParams_);
  const std::string lut = colorParams_.value("lut", std::string());
  if (lut.empty()) {
    lutPath_.clear();
    lutError_.reset();
    lutKeep.reset();
  } else if (lut != lutPath_) {
    lutPath_ = lut;
    lutError_.reset();
    lutKeep.reset();
    try {
      lutKeep = tools_.grader.upload(readCube(std::filesystem::path(bc::toWide(lut))));
    } catch (const std::exception& e) {
      lutError_ = ErrorInfo{"color-lut", std::string("the colour LUT cannot be read; the camera runs without the correction: ") + e.what()};
    }
  }
  grade_.lut = lutKeep;
  gradeDirty_ = true;
  matteParams_ = normalizeStageParams("matte", cfg_.stages.value("matte", nlohmann::json::object()));
  backgroundParams_ = normalizeStageParams("background", cfg_.stages.value("background", nlohmann::json::object()));
  if (!backgroundParams_.value("enabled", true)) backgroundParams_["mode"] = "none";
  const std::string mode = backgroundParams_.value("mode", std::string("none"));
  const std::string img = backgroundParams_.value("image", std::string());
  if (mode != "replace" || img.empty()) {
    bgImage_.reset();
    bgImagePath_.clear();
    bgImageError_.reset();
  } else if (img != bgImagePath_) {
    bgImageError_.reset();
    try {
      auto pic = loadImageBgra(std::filesystem::path(bc::toWide(img)));
      auto up = tools_.conv.uploadBgra8(pic.bgra.data(), pic.width, pic.height, pic.width * 4);
      bgImage_ = tools_.conv.toLinear(*up, PixelLayout::BGRA8, true);
      bgImagePath_ = img;  // only on success, so a failed load retries on the next reconfigure
    } catch (const std::exception& e) {
      bgImage_.reset();
      bgImagePath_.clear();
      bgImageError_ =
          ErrorInfo{"background-image", std::string("cannot load the background image; using the colour: ") + e.what()};
    }
  }
}

bool Camera::wantsMatte(Clock::time_point now) const {
  return matteParams_.value("enabled", false) && matteGuard_.shouldRun(now);
}

double Camera::stageGpuMs(const std::string& kind) const {
  if (kind == "color") return colorGpuMs_;
  if (kind == "background") return backgroundGpuMs_;
  return 0;
}

void Camera::pollColorTimer(Clock::time_point now) {
  if (!colorTimer_) return;
  if (auto took = colorTimer_->poll()) {
    colorGpuMs_ = std::chrono::duration<double, std::milli>(*took).count();
    colorGuard_.succeeded(*took, std::chrono::milliseconds(4), now);
  }
}

void Camera::pollBackgroundTimer(const TickSchedule& sched, Clock::time_point now) {
  if (!backgroundTimer_) return;
  if (auto took = backgroundTimer_->poll()) {
    backgroundGpuMs_ = std::chrono::duration<double, std::milli>(*took).count();
    backgroundGuard_.succeeded(*took, sched.rate().period() / 4, now);
  }
}

void Camera::startInput() {
  inputState_ = SignalState::Connecting;
  state_.set(path("/input/state"), "connecting");
  state_.set(path("/input/error"), nullptr);
  try {
    input_ = factory_.makeInput(cfg_.input);
    input_->start(*this);
  } catch (const std::exception& e) {
    input_.reset();
    state_.set(path("/input/state"), "offline");
    state_.set(path("/input/error"), errorToJson(ErrorInfo{"input-config", e.what()}));
  }
}

void Camera::startOutput(OutputSlot& s) {
  s.out.reset();
  s.createError.reset();
  if (!s.cfg.enabled) return;
  try {
    s.out = factory_.makeOutput(s.cfg, cfg_);
    s.out->start();
  } catch (const std::exception& e) {
    s.out.reset();
    s.createError = ErrorInfo{"output-config", e.what()};
  }
}

void Camera::start() {
  state_.set(path("/name"), cfg_.name);
  state_.set(path("/phase"), toString(FeedPhase::NoSignal));
  startInput();
  for (auto& o : cfg_.outputs) {
    outputs_.push_back({o, nullptr, std::nullopt});
    startOutput(outputs_.back());
  }
  publishStatus();
}

void Camera::stop() {
  if (input_) {
    input_->stop();
    input_.reset();
  }
  for (auto& s : outputs_) {
    if (s.out) s.out->stop();
  }
  outputs_.clear();
}

void Camera::reconfigure(const CameraConfig& next) {
  if (next.name != cfg_.name) state_.set(path("/name"), next.name);
  const nlohmann::json oldStages = cfg_.stages;
  const bool inputChanged = !(next.input == cfg_.input);
  std::map<std::string, OutputSlot> old;
  for (auto& s : outputs_) old.emplace(s.cfg.id, std::move(s));
  outputs_.clear();
  cfg_ = next;
  if (!(next.stages == oldStages)) {
    matteGuard_ = StageGuard{};
    backgroundGuard_ = StageGuard{};
    colorGuard_ = StageGuard{};
  }
  loadStageParams();
  if (inputChanged) {
    if (input_) input_->stop();
    input_.reset();
    startInput();  // the last frame stays in sync_, so outputs hold, then slate
  }
  for (auto& o : cfg_.outputs) {
    auto it = old.find(o.id);
    if (it != old.end() && it->second.cfg == o) {
      outputs_.push_back(std::move(it->second));
      old.erase(it);
      continue;
    }
    if (it != old.end()) {
      if (it->second.out) it->second.out->stop();
      old.erase(it);
    }
    outputs_.push_back({o, nullptr, std::nullopt});
    startOutput(outputs_.back());
  }
  for (auto& [id, s] : old) {
    if (s.out) s.out->stop();
    state_.erase(path("/outputs/" + id));
  }
  publishStatus();
}

void Camera::onVideo(VideoFrameIn frame) {
  const auto now = Clock::now();
  sync_.push(Pending{std::move(frame), now}, now);
}

void Camera::onAudio(AudioBlock a) {
  if (a.channels == 2 && a.sampleRate == 48000) audio_.push(a.samples);
}

void Camera::onState(SignalState s, std::optional<ErrorInfo> e) {
  inputState_ = s;
  state_.set(path("/input/state"), std::string(toString(s)));
  state_.set(path("/input/error"), errorToJson(e));
}

TextureRef Camera::fittedSlate(const TextureRef& source) {
  if (!source) return source;
  if (source->width == lastSize_.w && source->height == lastSize_.h) return source;
  const bool staleSrc = slateFitSrc_.lock().get() != source.get();
  if (!slateFit_ || staleSrc || slateFit_->width != lastSize_.w || slateFit_->height != lastSize_.h) {
    slateFit_ = tools_.conv.fit(*source, lastSize_.w, lastSize_.h);
    slateFitSrc_ = source;
  }
  return slateFit_;
}

TextureRef Camera::beginTick(Clock::time_point now, const TextureRef& slateSource) {
  pollColorTimer(now);
  auto pick = sync_.pick(now);
  tickArrival_.reset();
  tickPhase_ = pick.phase;
  if (pick.frame) {
    if (pick.fresh || !lastConverted_) {
      const auto& f = pick.frame->frame;
      // The frame is the input's own size: there is no show-wide frame size to scale to.
      lastConverted_ = tools_.conv.toLinear(*f.tex, f.layout, f.fullRange);
      lastSize_.w = lastConverted_->width;
      lastSize_.h = lastConverted_->height;
      if (pick.fresh) tickArrival_ = pick.frame->arrival;
    }
  } else {
    lastConverted_.reset();
  }
  tickRaw_ = lastConverted_ ? lastConverted_ : fittedSlate(slateSource);
  tickFrame_ = tickRaw_;
  const bool colorOn = colorParams_.value("enabled", false);
  if (lastConverted_ && colorOn && colorGuard_.shouldRun(now)) {
    // A size change does not dirty the grade, so the reuse must also match the current frame's
    // size: the colour stage can be bypassed across a renegotiation and leave a stale texture.
    if (!pick.fresh && !gradeDirty_ && lastGraded_ && lastGraded_->width == lastConverted_->width &&
        lastGraded_->height == lastConverted_->height) {
      tickFrame_ = lastGraded_;
    } else {
      const bool gpuTimed = colorTimer_.has_value();
      if (gpuTimed) colorTimer_->begin();
      const auto t0 = Clock::now();
      try {
        lastGraded_ = tools_.grader.apply(tickRaw_, grade_);
        gradeDirty_ = false;
        tickFrame_ = lastGraded_;
        if (gpuTimed) {
          colorTimer_->end();  // the GPU time reaches the guard on a later poll (spec D14)
        } else {
          colorGuard_.succeeded(Clock::now() - t0, std::chrono::milliseconds(4), now);
        }
      } catch (const std::exception& e) {
        if (gpuTimed) colorTimer_->end();
        colorGuard_.failed(e.what(), now);
      }
    }
  }
  return tickFrame_;
}

void Camera::endTick(uint64_t index, const TickSchedule& sched, Clock::time_point now, const TextureRef& rawAlpha,
                     bool previewWanted) {
  pollBackgroundTimer(sched, now);
  TextureRef frame = tickFrame_;
  bool hasAlpha = false;
  TextureRef alpha;
  if (rawAlpha) {
    try {
      alpha = tools_.comp.refine(*rawAlpha, frame->width, frame->height, refineFrom(matteParams_));
    } catch (const std::exception& e) {
      matteGuard_.failed(e.what(), now);
    }
  }
  auto bg = backgroundFrom(backgroundParams_);
  if (bg.mode != BackgroundMode::None && alpha && backgroundGuard_.shouldRun(now)) {
    const bool gpuTimed = backgroundTimer_.has_value();
    if (gpuTimed) backgroundTimer_->begin();
    const auto t0 = Clock::now();
    try {
      bg.image = bgImage_;
      frame = tools_.comp.apply(frame, alpha, bg);
      hasAlpha = bg.mode == BackgroundMode::Keyed;
      // ponytail: a fixed quarter-tick budget per camera; share by camera count if 8+ cameras run.
      if (gpuTimed) {
        backgroundTimer_->end();  // the GPU time reaches the guard on a later poll (spec D14)
      } else {
        backgroundGuard_.succeeded(Clock::now() - t0, sched.rate().period() / 4, now);
      }
    } catch (const std::exception& e) {
      if (gpuTimed) backgroundTimer_->end();
      backgroundGuard_.failed(e.what(), now);
      frame = tickFrame_;
      hasAlpha = false;
    }
  }
  lastFrame_ = frame;

  auto audio = std::make_shared<std::vector<float>>(audio_.pull(sched.audioFramesForTick(index, 48000)));

  // One frame per camera per tick — the composited frame's size (spec §4.3). There is no
  // per-output size and no fit. The readback is asynchronous: finish the previous copy if the
  // GPU is done, start this tick's copy, and hand every CPU output the newest *completed*
  // image (one tick behind at most). The tick never maps the frame. Run once per tick, so two
  // CPU outputs and the frame tap all get the *same* image, not one a tick apart.
  bool anyCpu = false;
  for (auto& s : outputs_) {
    if (s.out && s.out->needsCpu()) {
      anyCpu = true;
      break;
    }
  }
  std::shared_ptr<const CpuImage> cpu;
  if (anyCpu || previewWanted) {
    if (!readback_) readback_.emplace();
    // tryFinish before begin: the slot's previous copy finishes first, then this tick's begins.
    // A begin while pending would discard the unfinished frame.
    CpuImage done;
    if (readback_->slot.tryFinish(tools_.conv, done)) readback_->image = std::make_shared<CpuImage>(std::move(done));
    if (!readback_->slot.pending())
      readback_->slot.begin(tools_.conv, *tools_.conv.toBgra8(*frame, frame->width, frame->height));
    // A resolution change leaves the slot holding the previous size's completed image. Drop it so
    // the old size is never published while the first new-size copy is in flight (spec §4.3).
    if (readback_->image && (readback_->image->width != frame->width || readback_->image->height != frame->height))
      readback_->image.reset();
    cpu = readback_->image;  // may be null on the first ticks: never a synthetic frame
  } else {
    // No CPU consumer this tick (neither a CPU output nor a visible preview): release the slot
    // and the cached image, so a reconfigured or removed consumer holds and reads back nothing.
    readback_.reset();
  }
  tickCpu_ = cpu;

  for (auto& s : outputs_) {
    if (!s.out) continue;
    const std::shared_ptr<const CpuImage> outCpu = s.out->needsCpu() ? cpu : nullptr;
    if (!s.cfg.halfRate) {
      s.out->submit(OutFrame{index, sched.rate(), frame, outCpu, audio, 48000, 2, hasAlpha});
      continue;
    }
    // Half rate: even ticks only, carrying the odd tick's audio so nothing is lost.
    if (index % 2 == 1) {
      s.carryAudio = *audio;
      continue;
    }
    auto both = std::make_shared<std::vector<float>>(std::move(s.carryAudio));
    s.carryAudio.clear();
    both->insert(both->end(), audio->begin(), audio->end());
    const Rate half{sched.rate().num, sched.rate().den * 2};
    s.out->submit(OutFrame{index / 2, half, frame, outCpu, both, 48000, 2, hasAlpha});
  }

  std::lock_guard l(statsMutex_);
  phase_ = tickPhase_;
  if (tickArrival_) {
    ++freshThisSecond_;
    if (lastFresh_ && *tickArrival_ - *lastFresh_ > std::chrono::milliseconds(100)) lastStutter_ = Clock::now();
    lastFresh_ = *tickArrival_;
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - *tickArrival_).count();
    latencyEwmaMs_ = latencyEwmaMs_ == 0 ? ms : latencyEwmaMs_ * 0.9 + ms * 0.1;
  }
}

void Camera::publishStatus() {
  double latency;
  FeedPhase phase;
  bool stutter;
  {
    std::lock_guard l(statsMutex_);
    fps_ = static_cast<double>(freshThisSecond_);
    freshThisSecond_ = 0;
    latency = latencyEwmaMs_;
    phase = phase_;
    stutter = lastStutter_.time_since_epoch().count() != 0 && Clock::now() - lastStutter_ < std::chrono::seconds(10);
  }
  state_.set(path("/phase"), toString(phase));
  const bool colorOn = colorParams_.value("enabled", false);
  const bool matteOn = matteParams_.value("enabled", false);
  const bool bgOn = backgroundParams_.value("mode", std::string("none")) != "none";
  state_.set(path("/stages/color/state"), colorGuard_.stateName(colorOn));
  auto colorErr = colorGuard_.error() ? colorGuard_.error() : lutError_;
  state_.set(path("/stages/color/error"), errorToJson(colorOn ? colorErr : std::nullopt));
  state_.set(path("/stages/matte/state"), matteGuard_.stateName(matteOn));
  state_.set(path("/stages/matte/error"), errorToJson(matteOn ? matteGuard_.error() : std::optional<ErrorInfo>{}));
  state_.set(path("/stages/background/state"), backgroundGuard_.stateName(bgOn));
  auto bgErr = backgroundGuard_.error() ? backgroundGuard_.error() : bgImageError_;
  state_.set(path("/stages/background/error"), errorToJson(bgOn ? bgErr : std::nullopt));
  if (colorTimer_) state_.set(path("/stages/color/gpuMs"), std::round(colorGpuMs_ * 10.0) / 10.0);
  if (backgroundTimer_) state_.set(path("/stages/background/gpuMs"), std::round(backgroundGpuMs_ * 10.0) / 10.0);
  state_.set(path("/input/health"), std::string(inputHealth(inputState_.load(), phase, stutter)));
  const auto target = std::chrono::milliseconds(static_cast<int64_t>(std::lround(latency / 5.0) * 5));
  if (audioDelaySet_.count() < 0 || std::abs((target - audioDelaySet_).count()) > 10) {
    audio_.setDelay(target);
    audioDelaySet_ = target;
  }
  for (auto& s : outputs_) {
    const std::string p = path("/outputs/" + s.cfg.id);
    state_.set(p + "/kind", s.cfg.kind);
    if (!s.cfg.enabled) {
      state_.set(p + "/state", "ready");
      state_.set(p + "/error", nullptr);
    } else if (!s.out) {
      state_.set(p + "/state", "offline");
      state_.set(p + "/error", errorToJson(s.createError));
    } else {
      auto st = s.out->status();
      state_.set(p + "/state", std::string(toString(st.state)));
      state_.set(p + "/error", errorToJson(st.error));
      state_.set(p + "/tally", {{"program", st.program}, {"preview", st.preview}});
    }
  }
}

CameraStats Camera::stats() const {
  std::lock_guard l(statsMutex_);
  return {fps_, sync_.drops(), sync_.repeats(), latencyEwmaMs_, phase_};
}
}  // namespace bc
