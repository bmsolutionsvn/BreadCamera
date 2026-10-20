#include "engine/camera.h"

#include <cmath>
#include <filesystem>
#include <map>

#include "core/lut.h"
#include "core/upscale_size.h"
#include "core/utf8.h"
#include "gpu/color_grader.h"
#include "gpu/image_io.h"

namespace bc {
using Clock = std::chrono::steady_clock;

namespace {
// Match's tiny readbacks (spec §3.1: the main path never copies a full frame to the CPU).
constexpr uint32_t kMatchW = 128, kMatchH = 72;
}  // namespace

Camera::Camera(CameraConfig cfg, AdapterFactory& factory, GpuTools tools, StateStore& state, SyncPolicy policy)
    : cfg_(std::move(cfg)), factory_(factory), tools_(tools), state_(state), sync_(policy), policy_(policy) {
  loadStageParams();
  if (tools_.gpu) {
    // Per-stage GPU timing (spec D14). A query failure falls back to the CPU timing.
    try {
      colorTimer_.emplace(*tools_.gpu);
      backgroundTimer_.emplace(*tools_.gpu);
      upscaleTimer_.emplace(*tools_.gpu);
      cleanupTimer_.emplace(*tools_.gpu);
    } catch (const std::exception&) {
      colorTimer_.reset();
      backgroundTimer_.reset();
      upscaleTimer_.reset();
      cleanupTimer_.reset();
    }
  }
}

Camera::~Camera() { stop(); }

void Camera::loadStageParams() {
  const nlohmann::json prevColor = colorParams_;
  colorParams_ = normalizeStageParams("color", cfg_.stages.value("color", nlohmann::json::object()));
  if (colorParams_ != prevColor) {  // spec D16: the correction lives in the camera and resets here
    correction_ = MatchCorrection{};
    matchTarget_ = MatchCorrection{};
    matchOwn_.reset();
    lastMatchAt_.reset();
  }
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
  upscaleParams_ = normalizeStageParams("upscale", cfg_.stages.value("upscale", nlohmann::json::object()));
  upscaleTargetH_ = upscaleParams_.value("target", std::string("1080")) == "720" ? 720u : 1080u;
  cleanupParams_ = normalizeStageParams("cleanup", cfg_.stages.value("cleanup", nlohmann::json::object()));
}

bool Camera::wantsMatte(Clock::time_point now) const {
  return matteParams_.value("enabled", false) && matteGuard_.shouldRun(now);
}

double Camera::stageGpuMs(const std::string& kind) const {
  if (kind == "color") return colorGpuMs_;
  if (kind == "background") return backgroundGpuMs_;
  if (kind == "upscale") return upscaleGpuMs_;
  if (kind == "cleanup") return cleanupGpuMs_;
  return 0;
}

bool Camera::matchMode() const {
  return colorParams_.value("enabled", false) && colorParams_.value("mode", std::string("calibrate")) == "match";
}

std::string Camera::matchTo() const { return colorParams_.value("matchTo", std::string()); }

void Camera::setMatchReference(std::optional<SkinStats> ref, bool refHasPicture) {
  matchRef_ = std::move(ref);
  matchRefLive_ = refHasPicture;
}

ColorGrade Camera::matchGrade() const {
  ColorGrade g = gradeFrom(colorParams_);
  g.lut.reset();  // spec D9: no LUT in Match; the stored LUT returns in Calibrate
  g.exposure += correction_.exposure;
  g.temperature += correction_.temperature;
  g.tint += correction_.tint;
  return g;
}

void Camera::updateMatchTarget(const std::optional<SkinStats>& own, Clock::time_point now) {
  const double dt = lastMatchAt_ ? std::chrono::duration<double>(now - *lastMatchAt_).count() : 0.1;
  lastMatchAt_ = now;
  std::optional<MatchCorrection> target;
  if (!matchTo().empty()) {
    if (matchRef_ && matchRefLive_ && own) target = solveMatch(*own, *matchRef_);
  } else if (own) {
    target = autoBalance(*own);
  }
  if (!target) return;  // hold: no skin, or the reference is down (spec D12, review focus 3)
  matchTarget_ = *target;
  correction_ = ease(correction_, matchTarget_, dt);
}

void Camera::finishMatchRound(Clock::time_point now) {
  if (!matchRound_) return;
  MatchRound& r = *matchRound_;
  if (r.wantIn && r.in.tryFinish(tools_.conv, r.inImg)) r.wantIn = false;
  if (r.wantOut && r.out.tryFinish(tools_.conv, r.outImg)) r.wantOut = false;
  if (r.wantAlpha && r.alpha.tryFinish(tools_.conv, r.alphaImg)) r.wantAlpha = false;
  if (r.wantIn || r.wantOut || r.wantAlpha) {
    // ponytail: a GPU copy that never lands is dropped after ~2 s; a per-round retry budget if a
    // driver wedges on the tiny copy.
    if (++matchRoundAge_ > 120) matchRound_.reset();
    return;
  }
  const uint8_t* alpha = r.alphaImg.bgra.empty() ? nullptr : r.alphaImg.bgra.data();
  if (!r.outImg.bgra.empty()) {
    matchOutput_ = skinStats(r.outImg.bgra.data(), alpha, kMatchW, kMatchH);
  }
  if (matchMode() && !r.inImg.bgra.empty()) {
    matchOwn_ = skinStats(r.inImg.bgra.data(), alpha, kMatchW, kMatchH);
    updateMatchTarget(matchOwn_, now);
  }
  matchRound_.reset();
}

void Camera::beginMatchRound(const TextureRef& entering) {
  if (matchRound_) return;  // one measurement at a time; a re-begin would discard an unfinished frame
  if (!matchMode() && !matchedBy_) return;
  MatchRound r;
  if (matchMode()) {
    r.in.begin(tools_.conv, *tools_.conv.toBgra8(*entering, kMatchW, kMatchH));
    r.wantIn = true;
  }
  r.out.begin(tools_.conv, *tools_.conv.toBgra8(*tickFrame_, kMatchW, kMatchH));
  r.wantOut = true;
  if (lastAlpha_) {
    r.alpha.begin(tools_.conv, *tools_.conv.toBgra8(*lastAlpha_, kMatchW, kMatchH));
    r.wantAlpha = true;
  }
  matchRoundAge_ = 0;
  matchRound_ = std::move(r);
}

void Camera::pollColorTimer(Clock::time_point now) {
  if (!colorTimer_) return;
  if (auto took = colorTimer_->poll()) {
    colorGpuMs_ = std::chrono::duration<double, std::milli>(*took).count();
    colorGuard_.succeeded(*took, std::chrono::milliseconds(4), now);
  }
}

void Camera::pollBackgroundTimer(Clock::time_point now) {
  if (!backgroundTimer_) return;
  if (auto took = backgroundTimer_->poll()) {
    backgroundGpuMs_ = std::chrono::duration<double, std::milli>(*took).count();
    backgroundGuard_.succeeded(*took, projectRate().period() / 4, now);
  }
}

void Camera::pollUpscaleTimer(Clock::time_point now) {
  // ponytail: a fixed quarter-budget share per camera, the same share Background takes; split it by
  // camera count if 8+ cameras run the FSR path.
  const auto budget = projectRate().period() / 4;
  upscaleBudget_ = budget;  // enhance() runs in beginTick, before this, and uses the last value
  if (!upscaleTimer_) return;
  if (auto took = upscaleTimer_->poll()) {
    upscaleGpuMs_ = std::chrono::duration<double, std::milli>(*took).count();
    upscaleGuard_.succeeded(*took, budget, now);
  }
}

void Camera::pollCleanupTimer(Clock::time_point now) {
  // ponytail: the same fixed quarter-budget share per camera as Upscale; split by camera count if
  // 8+ cameras run Clean up (RTX only, so it is zero-cost on every other PC).
  const auto budget = projectRate().period() / 4;
  cleanupBudget_ = budget;  // enhance() runs in beginTick, before this, and uses the last value
  if (!cleanupTimer_) return;
  if (auto took = cleanupTimer_->poll()) {
    cleanupGpuMs_ = std::chrono::duration<double, std::milli>(*took).count();
    cleanupGuard_.succeeded(*took, budget, now);
  }
}

void Camera::startInput() {
  nominalPacked_.store(0);
  estimator_.reset();
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
    upscaleGuard_ = StageGuard{};
    cleanupGuard_ = StageGuard{};
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
  lastArrival_ = now;
  const auto pack = [](Rate r) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(r.num)) << 32) | static_cast<uint32_t>(r.den);
  };
  if (frame.rate) {
    nominalPacked_.store(pack(*frame.rate));
  } else if (nominalPacked_.load() == 0) {
    estimator_.push(now);
    if (auto r = estimator_.rate()) nominalPacked_.store(pack(*r));
  }
  const uint64_t seq = ++arrivals_;
  sync_.push(Pending{std::move(frame), now, seq}, now);
  if (wake_) wake_();
}

bool Camera::ready(Clock::time_point now) const {
  if (arrivals_.load() > taken_) return true;
  if (!sawFrame_ || !lastArrival_) return false;
  if (nextRepeat_) return now >= *nextRepeat_;
  return now >= *lastArrival_ + policy_.stallAfter;
}

std::optional<Clock::time_point> Camera::nextRepeatAt(Clock::time_point now) const {
  if (!sawFrame_ || !lastArrival_) return std::nullopt;
  const auto stallAt = *lastArrival_ + policy_.stallAfter;
  if (now < stallAt) return stallAt;
  return nextRepeat_ ? nextRepeat_ : std::optional<Clock::time_point>{stallAt};
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

TextureRef Camera::enhance(const TextureRef& in, bool fresh, Clock::time_point now) {
  if (!in) return in;
  const bool cleanupOn = cleanupParams_.value("enabled", false);
  const bool upscaleOn = upscaleParams_.value("enabled", false);
  if (!cleanupOn && !upscaleOn) return in;
  const FrameSize want =
      upscaleOn ? upscaleTarget(in->width, in->height, upscaleTargetH_) : FrameSize{in->width, in->height};
  const bool resize = upscaleOn && (want.w != in->width || want.h != in->height);
  // A target not larger than the input, and no Clean up: nothing to do (D3).
  if (!cleanupOn && !resize) {
    lastEnhanced_.reset();
    return in;
  }
  // Reuse the last result on a repeat tick, as the grade does.
  if (!fresh && lastEnhanced_ && lastEnhanced_->width == want.w && lastEnhanced_->height == want.h) {
    return lastEnhanced_;
  }
  TextureRef frame = in;

  // Clean up runs first (spec pipeline order), at 1×: RTX only. On a non-RTX PC (tools_.rtx ==
  // nullptr) it bypasses with a readable reason and the picture passes through untouched (D7).
  if (cleanupOn && cleanupGuard_.shouldRun(now)) {
    if (tools_.rtx) {
      const bool gpuTimed = cleanupTimer_.has_value();
      if (gpuTimed) cleanupTimer_->begin();
      const auto t0 = Clock::now();
      try {
        frame = tools_.rtx->run(*frame, frame->width, frame->height);
        if (gpuTimed) {
          cleanupTimer_->end();  // the GPU time reaches the guard on a later poll (spec D14)
        } else {
          cleanupGuard_.succeeded(Clock::now() - t0, cleanupBudget_, now);
        }
      } catch (const std::exception& e) {
        if (gpuTimed) cleanupTimer_->end();
        cleanupGuard_.failed(e.what(), now);
        frame = in;  // a failed Clean up passes the frame through; Upscale still runs below
      }
    } else {
      cleanupGuard_.failed(ErrorInfo{"needs-rtx", "Clean up needs an NVIDIA RTX card; it is bypassed on this PC."},
                           now);
    }
  }

  if (resize) {
    // Fallback chain, always at the target size (D3a): RTX (spec D4) → FSR 1 (D6) → plain fit. Not
    // fused: Clean up and Upscale are two VSR passes until an RTX run shows the cost matters.
    bool done = false;
    if (tools_.rtx && upscaleGuard_.shouldRun(now)) {
      const bool gpuTimed = upscaleTimer_.has_value();
      if (gpuTimed) upscaleTimer_->begin();
      const auto t0 = Clock::now();
      try {
        frame = tools_.rtx->run(*frame, want.w, want.h);
        done = true;
        if (gpuTimed) {
          upscaleTimer_->end();
        } else {
          upscaleGuard_.succeeded(Clock::now() - t0, upscaleBudget_, now);
        }
      } catch (const std::exception& e) {
        if (gpuTimed) upscaleTimer_->end();
        upscaleGuard_.failed(e.what(), now);  // fall through to FSR 1 / the fit below
      }
    }
    if (!done) {
      if (tools_.up && upscaleGuard_.shouldRun(now)) {
        const bool gpuTimed = upscaleTimer_.has_value();
        if (gpuTimed) upscaleTimer_->begin();
        const auto t0 = Clock::now();
        try {
          frame = tools_.up->apply(*frame, want.w, want.h);
          if (gpuTimed) {
            upscaleTimer_->end();
          } else {
            upscaleGuard_.succeeded(Clock::now() - t0, upscaleBudget_, now);
          }
        } catch (const std::exception& e) {
          if (gpuTimed) upscaleTimer_->end();
          upscaleGuard_.failed(e.what(), now);
          frame = tools_.conv.fit(*frame, want.w, want.h);  // bypass keeps the target size (D3a)
        }
      } else {
        frame = tools_.conv.fit(*frame, want.w, want.h);  // no RTX/FSR, or bypassed: fit (D3a)
      }
    }
  }
  lastEnhanced_ = frame;
  return frame;
}

TextureRef Camera::beginTick(Clock::time_point now, const TextureRef& slateSource) {
  pollColorTimer(now);
  ++matchTick_;
  finishMatchRound(now);  // never waits: the previous small readbacks may or may not be ready
  auto pick = sync_.pick(now);
  const uint64_t prevTaken = taken_;
  if (pick.fresh && pick.frame) taken_ = pick.frame->seq;
  tickIndex_ = emitIndex_;
  emitIndex_ = advanceIndex(emitIndex_, prevTaken, taken_, pick.fresh);
  tickArrival_.reset();
  tickPhase_ = pick.phase;
  if (pick.frame) {
    if (pick.fresh || !lastConverted_) {
      const auto& f = pick.frame->frame;
      // The frame is the input's own size: there is no show-wide frame size to scale to.
      lastConverted_ = tools_.conv.toLinear(*f.tex, f.layout, f.fullRange);
      sawFrame_ = true;
      if (pick.fresh) tickArrival_ = pick.frame->arrival;
    }
  } else {
    lastConverted_.reset();
  }
  // The camera's own size is the *enhanced* size: the target size derived from the last input, or
  // the target's 16:9 fallback before any frame has been seen (D2/D3a). Updating lastSize_ here
  // makes the slate fit to the target size too, so Upscale never has to resize the slate.
  const bool upscaleOn = upscaleParams_.value("enabled", false);
  if (lastConverted_) {
    const FrameSize s = upscaleOn ? upscaleTarget(lastConverted_->width, lastConverted_->height, upscaleTargetH_)
                                  : FrameSize{lastConverted_->width, lastConverted_->height};
    lastSize_.w = s.w;
    lastSize_.h = s.h;
  } else if (upscaleOn && !sawFrame_) {
    lastSize_.w = upscaleTargetH_ * 16 / 9;
    lastSize_.h = upscaleTargetH_;
  }
  tickRaw_ = lastConverted_ ? lastConverted_ : fittedSlate(slateSource);
  tickFrame_ = enhance(tickRaw_, pick.fresh, now);
  const TextureRef entering = tickFrame_;  // the frame entering Colour, for Match's own statistics
  const bool colorOn = colorParams_.value("enabled", false);
  if (matchMode()) {
    // spec D11/D16: Match drives the existing grade. Dirty only when a field moves by more than
    // 1e-3, so a still correction reuses the last graded texture (as Calibrate does).
    const ColorGrade g = matchGrade();
    const bool moved = std::abs(g.exposure - grade_.exposure) > 1e-3f ||
                       std::abs(g.temperature - grade_.temperature) > 1e-3f ||
                       std::abs(g.tint - grade_.tint) > 1e-3f;
    if (moved || grade_.lut) {  // the correction moved, or a Calibrate LUT is still attached
      grade_ = g;
      gradeDirty_ = true;
    }
  }
  if (lastConverted_ && colorOn && colorGuard_.shouldRun(now)) {
    // A size change does not dirty the grade, so the reuse must also match the current frame's
    // size: the colour stage can be bypassed across a renegotiation and leave a stale texture.
    if (!pick.fresh && !gradeDirty_ && lastGraded_ && lastGraded_->width == tickFrame_->width &&
        lastGraded_->height == tickFrame_->height) {
      tickFrame_ = lastGraded_;
    } else {
      const bool gpuTimed = colorTimer_.has_value();
      if (gpuTimed) colorTimer_->begin();
      const auto t0 = Clock::now();
      try {
        lastGraded_ = tools_.grader.apply(tickFrame_, grade_);
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
  if (matchTick_ % 6 == 0) beginMatchRound(entering);  // three tiny readbacks, finished later
  if (!(nominalRate() == audioSched_.rate())) audioSched_ = TickSchedule(nominalRate());
  if (pick.phase == FeedPhase::Holding || pick.phase == FeedPhase::Slate) {
    nextRepeat_ = now + nominalRate().period();
  } else {
    nextRepeat_.reset();
  }
  return tickFrame_;
}

void Camera::endTick(Clock::time_point now, const TextureRef& rawAlpha, bool previewWanted) {
  pollBackgroundTimer(now);
  pollUpscaleTimer(now);
  pollCleanupTimer(now);
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
  lastAlpha_ = alpha;  // Match masks skin with the previous tick's refined matte (one-tick lag)
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
        backgroundGuard_.succeeded(Clock::now() - t0, projectRate().period() / 4, now);
      }
    } catch (const std::exception& e) {
      if (gpuTimed) backgroundTimer_->end();
      backgroundGuard_.failed(e.what(), now);
      frame = tickFrame_;
      hasAlpha = false;
    }
  }
  lastFrame_ = frame;

  auto audio = std::make_shared<std::vector<float>>(audio_.pull(audioSched_.audioFramesForTick(tickIndex_, 48000)));

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
    std::shared_ptr<const std::vector<float>> outAudio = audio;
    if (s.cfg.halfRate) {
      // Half rate: every second emitted frame, carrying the odd frame's audio so nothing is lost.
      if (s.emitted++ % 2 == 1) {
        s.carryAudio = *audio;
        continue;
      }
      auto both = std::make_shared<std::vector<float>>(std::move(s.carryAudio));
      s.carryAudio.clear();
      both->insert(both->end(), audio->begin(), audio->end());
      outAudio = both;
    }
    const Rate outRate = s.cfg.halfRate ? Rate{nominalRate().num, nominalRate().den * 2} : nominalRate();
    s.out->submit(OutFrame{tickIndex_, outRate, frame, outCpu, outAudio, 48000, 2, hasAlpha});
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
  // spec D9/D12: the Match state the Inspector shows. Null in Calibrate.
  std::optional<std::string> matchState;
  if (matchMode()) {
    if (!matchOwn_) matchState = "holding-no-face";
    else if (matchTo().empty()) matchState = "auto";
    else if (!matchRef_ || !matchRefLive_) matchState = "holding-reference-down";
    else matchState = "matching";
  }
  state_.set(path("/stages/color/match"), matchState ? nlohmann::json(*matchState) : nlohmann::json(nullptr));
  state_.set(path("/stages/matte/state"), matteGuard_.stateName(matteOn));
  state_.set(path("/stages/matte/error"), errorToJson(matteOn ? matteGuard_.error() : std::optional<ErrorInfo>{}));
  state_.set(path("/stages/background/state"), backgroundGuard_.stateName(bgOn));
  auto bgErr = backgroundGuard_.error() ? backgroundGuard_.error() : bgImageError_;
  state_.set(path("/stages/background/error"), errorToJson(bgOn ? bgErr : std::nullopt));
  if (colorTimer_) state_.set(path("/stages/color/gpuMs"), std::round(colorGpuMs_ * 10.0) / 10.0);
  if (backgroundTimer_) state_.set(path("/stages/background/gpuMs"), std::round(backgroundGpuMs_ * 10.0) / 10.0);
  const bool upscaleOn = upscaleParams_.value("enabled", false);
  state_.set(path("/stages/upscale/state"), upscaleGuard_.stateName(upscaleOn));
  state_.set(path("/stages/upscale/error"), errorToJson(upscaleOn ? upscaleGuard_.error() : std::optional<ErrorInfo>{}));
  if (upscaleTimer_) state_.set(path("/stages/upscale/gpuMs"), std::round(upscaleGpuMs_ * 10.0) / 10.0);
  std::optional<std::string> upscaleNote;
  if (upscaleOn && lastConverted_) {
    const FrameSize want = upscaleTarget(lastConverted_->width, lastConverted_->height, upscaleTargetH_);
    if (want.w == lastConverted_->width && want.h == lastConverted_->height) {
      upscaleNote = upscaleTargetH_ == 720 ? "Input is already 720p" : "Input is already 1080p";
    }
  }
  state_.set(path("/stages/upscale/note"), upscaleNote ? nlohmann::json(*upscaleNote) : nlohmann::json(nullptr));
  const bool cleanupOn = cleanupParams_.value("enabled", false);
  state_.set(path("/stages/cleanup/state"), cleanupGuard_.stateName(cleanupOn));
  state_.set(path("/stages/cleanup/error"),
             errorToJson(cleanupOn ? cleanupGuard_.error() : std::optional<ErrorInfo>{}));
  if (cleanupTimer_) state_.set(path("/stages/cleanup/gpuMs"), std::round(cleanupGpuMs_ * 10.0) / 10.0);
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
