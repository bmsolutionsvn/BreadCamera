#include "engine/camera.h"

#include <cmath>
#include <map>

namespace bc {
using Clock = std::chrono::steady_clock;

Camera::Camera(CameraConfig cfg, AdapterFactory& factory, Converter& conv, StateStore& state, SyncPolicy policy)
    : cfg_(std::move(cfg)), factory_(factory), conv_(conv), state_(state), sync_(policy) {
  loadStageParams();
}

Camera::~Camera() { stop(); }

void Camera::loadStageParams() {
  matteParams_ = normalizeStageParams("matte", cfg_.stages.value("matte", nlohmann::json::object()));
  backgroundParams_ = normalizeStageParams("background", cfg_.stages.value("background", nlohmann::json::object()));
  if (!backgroundParams_.value("enabled", true)) backgroundParams_["mode"] = "none";
}

bool Camera::wantsMatte(Clock::time_point now) const {
  return matteParams_.value("enabled", false) && matteGuard_.shouldRun(now);
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

TextureRef Camera::beginTick(Clock::time_point now, const TextureRef& slate) {
  auto pick = sync_.pick(now);
  tickArrival_.reset();
  tickPhase_ = pick.phase;
  if (pick.frame) {
    if (pick.fresh || !lastConverted_) {
      const auto& f = pick.frame->frame;
      auto lin = conv_.toLinear(*f.tex, f.layout, f.fullRange);
      lastConverted_ = (lin->width == width_ && lin->height == height_) ? lin : conv_.fit(*lin, width_, height_);
      if (pick.fresh) tickArrival_ = pick.frame->arrival;
    }
  } else {
    lastConverted_.reset();
  }
  tickFrame_ = lastConverted_ ? lastConverted_ : slate;
  return tickFrame_;
}

void Camera::endTick(uint64_t index, const TickSchedule& sched, Clock::time_point now, const TextureRef& rawAlpha) {
  (void)now;
  (void)rawAlpha;  // Task 4 runs the stages here
  TextureRef frame = tickFrame_;
  const bool hasAlpha = false;
  lastFrame_ = frame;

  auto audio = std::make_shared<std::vector<float>>(audio_.pull(sched.audioFramesForTick(index, 48000)));

  // One frame (and one readback) per distinct output size this tick (spec §4.3).
  struct Variant {
    TextureRef tex;
    std::shared_ptr<const CpuImage> cpu;
  };
  std::map<std::pair<uint32_t, uint32_t>, Variant> variants;
  auto variant = [&](const OutputConfig& c, bool needsCpu) -> Variant& {
    const uint32_t w = c.width > 0 ? static_cast<uint32_t>(c.width) : frame->width;
    const uint32_t h = c.height > 0 ? static_cast<uint32_t>(c.height) : frame->height;
    auto& v = variants[{w, h}];
    if (!v.tex) v.tex = (w == frame->width && h == frame->height) ? frame : conv_.fit(*frame, w, h);
    if (needsCpu && !v.cpu) v.cpu = std::make_shared<CpuImage>(conv_.readback(*conv_.toBgra8(*v.tex, w, h)));
    return v;
  };
  for (auto& s : outputs_) {
    if (!s.out) continue;
    if (!s.cfg.halfRate) {
      auto& v = variant(s.cfg, s.out->needsCpu());
      s.out->submit(OutFrame{index, sched.rate(), v.tex, v.cpu, audio, 48000, 2, hasAlpha});
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
    auto& v = variant(s.cfg, s.out->needsCpu());
    const Rate half{sched.rate().num, sched.rate().den * 2};
    s.out->submit(OutFrame{index / 2, half, v.tex, v.cpu, both, 48000, 2, hasAlpha});
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
  const bool matteOn = matteParams_.value("enabled", false);
  const bool bgOn = backgroundParams_.value("mode", std::string("none")) != "none";
  state_.set(path("/stages/matte/state"), matteGuard_.stateName(matteOn));
  state_.set(path("/stages/matte/error"), errorToJson(matteOn ? matteGuard_.error() : std::optional<ErrorInfo>{}));
  state_.set(path("/stages/background/state"), backgroundGuard_.stateName(bgOn));
  state_.set(path("/stages/background/error"),
             errorToJson(bgOn ? backgroundGuard_.error() : std::optional<ErrorInfo>{}));
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
    }
  }
}

CameraStats Camera::stats() const {
  std::lock_guard l(statsMutex_);
  return {fps_, sync_.drops(), sync_.repeats(), latencyEwmaMs_, phase_};
}
}  // namespace bc
