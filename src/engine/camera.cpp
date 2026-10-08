#include "engine/camera.h"

#include <cmath>
#include <map>

namespace bc {
using Clock = std::chrono::steady_clock;

Camera::Camera(CameraConfig cfg, AdapterFactory& factory, Converter& conv, StateStore& state, SyncPolicy policy)
    : cfg_(std::move(cfg)), factory_(factory), conv_(conv), state_(state), sync_(policy) {}

Camera::~Camera() { stop(); }

void Camera::startInput() {
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
  const bool inputChanged = !(next.input == cfg_.input);
  std::map<std::string, OutputSlot> old;
  for (auto& s : outputs_) old.emplace(s.cfg.id, std::move(s));
  outputs_.clear();
  cfg_ = next;
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
  state_.set(path("/input/state"), std::string(toString(s)));
  state_.set(path("/input/error"), errorToJson(e));
}

void Camera::tick(uint64_t index, const TickSchedule& sched, Clock::time_point now, const TextureRef& slate) {
  auto pick = sync_.pick(now);
  std::optional<Clock::time_point> arrival;
  if (pick.frame) {
    if (pick.fresh || !lastConverted_) {
      const auto& f = pick.frame->frame;
      lastConverted_ = conv_.toLinear(*f.tex, f.layout, f.fullRange);
      if (pick.fresh) arrival = pick.frame->arrival;
    }
  } else {
    lastConverted_.reset();
  }
  TextureRef frame = lastConverted_ ? lastConverted_ : slate;
  lastFrame_ = frame;

  auto audio = std::make_shared<std::vector<float>>(audio_.pull(sched.audioFramesForTick(index, 48000)));
  bool anyCpu = false;
  for (auto& s : outputs_) anyCpu = anyCpu || (s.out && s.out->needsCpu());
  std::shared_ptr<const CpuImage> cpu;
  if (anyCpu) cpu = std::make_shared<CpuImage>(conv_.readback(*conv_.toBgra8(*frame, frame->width, frame->height)));

  OutFrame out{index, sched.rate(), frame, cpu, audio, 48000, 2, false};
  for (auto& s : outputs_) {
    if (s.out) s.out->submit(out);
  }

  std::lock_guard l(statsMutex_);
  phase_ = pick.phase;
  if (arrival) {
    ++freshThisSecond_;
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - *arrival).count();
    latencyEwmaMs_ = latencyEwmaMs_ == 0 ? ms : latencyEwmaMs_ * 0.9 + ms * 0.1;
  }
}

void Camera::publishStatus() {
  double latency;
  FeedPhase phase;
  {
    std::lock_guard l(statsMutex_);
    fps_ = static_cast<double>(freshThisSecond_);
    freshThisSecond_ = 0;
    latency = latencyEwmaMs_;
    phase = phase_;
  }
  state_.set(path("/phase"), toString(phase));
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
