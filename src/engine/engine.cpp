#include "engine/engine.h"

#include <windows.h>
#include <timeapi.h>

#include <cmath>
#include "core/tick_schedule.h"
#include "gpu/image_io.h"

namespace bc {
using Clock = std::chrono::steady_clock;

Engine::Engine(Converter& conv, StateStore& state, AdapterFactory& factory)
    : conv_(conv), state_(state), factory_(factory) {
  std::lock_guard l(mutex_);
  loadSlate();
  state_.set("/engine/rate", settings_.rate.str());
  state_.set("/engine/overruns", 0);
}

Engine::~Engine() {
  stop();
  std::vector<std::unique_ptr<Camera>> gone;
  {
    std::lock_guard l(mutex_);
    gone = std::move(cameras_);
  }
}

void Engine::loadSlate() {
  state_.set("/engine/warnings/slate", "");
  if (!settings_.slatePath.empty()) {
    try {
      auto img = loadImageBgra(settings_.slatePath);
      auto up = conv_.uploadBgra8(img.bgra.data(), img.width, img.height, img.width * 4);
      slate_ = conv_.toLinear(*up, PixelLayout::BGRA8, true);
      return;
    } catch (const std::exception& e) {
      state_.set("/engine/warnings/slate", std::string("slate image could not be loaded: ") + e.what() +
                                               "; using the built-in slate");
    }
  }
  slate_ = conv_.solid(1920, 1080, {0.05f, 0.05f, 0.06f, 1.0f});
}

void Engine::apply(const Show& show) {
  std::vector<std::unique_ptr<Camera>> removed;
  {
    std::lock_guard l(mutex_);
    if (!settingsApplied_ || !(show.settings == settings_)) {
      const bool slateChanged = !settingsApplied_ || show.settings.slatePath != settings_.slatePath;
      rateChanged_ = rateChanged_ || !(show.settings.rate == settings_.rate);
      settings_ = show.settings;
      settingsApplied_ = true;
      if (slateChanged) loadSlate();
      state_.set("/engine/rate", settings_.rate.str());
      for (auto& c : cameras_) c->setPolicy(policy());
    }
    std::map<std::string, std::unique_ptr<Camera>> existing;
    for (auto& c : cameras_) existing.emplace(c->config().id, std::move(c));
    cameras_.clear();
    for (auto& cfg : show.cameras) {
      auto it = existing.find(cfg.id);
      if (it != existing.end()) {
        if (!(it->second->config() == cfg)) it->second->reconfigure(cfg);
        cameras_.push_back(std::move(it->second));
        existing.erase(it);
      } else {
        auto cam = std::make_unique<Camera>(cfg, factory_, conv_, state_, policy());
        cam->start();
        cameras_.push_back(std::move(cam));
      }
    }
    for (auto& [id, cam] : existing) {
      state_.erase("/cameras/" + id);
      lastPhase_.erase(id);
      removed.push_back(std::move(cam));
    }
  }
  removed.clear();  // joins adapter threads outside the lock
}

void Engine::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void Engine::stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

EngineStats Engine::stats() const {
  std::lock_guard l(mutex_);
  EngineStats s;
  s.overruns = overruns_;
  for (auto& c : cameras_) s.cameras[c->config().id] = c->stats();
  return s;
}

void Engine::run() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  timeBeginPeriod(1);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

  TickSchedule sched(settings_.rate);
  // One full period of settle before tick 0: a camera's input starts with the camera,
  // so its first frame is in flight before the engine's first tick (no one-frame slate
  // flash when a show starts, and the first delivered frame is the input's own size).
  Clock::time_point start = Clock::now() + sched.offset(1);
  uint64_t index = 0;
  uint64_t ticksPerSecond = 60;
  while (running_) {
    {
      std::lock_guard l(mutex_);
      if (rateChanged_) {
        sched = TickSchedule(settings_.rate);
        ticksPerSecond = static_cast<uint64_t>(std::lround(settings_.rate.fps()));
        start = Clock::now() + sched.offset(1);
        index = 0;
        rateChanged_ = false;
      }
    }
    const auto deadline = start + sched.offset(index);
    const auto remaining = deadline - Clock::now();
    if (remaining.count() > 0) {
      LARGE_INTEGER due;
      due.QuadPart = -static_cast<LONGLONG>(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
      SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
      WaitForSingleObject(timer, INFINITE);
    }
    const auto now = Clock::now();
    std::vector<std::pair<std::string, TextureRef>> frames;
    {
      std::lock_guard l(mutex_);
      for (auto& cam : cameras_) {
        const auto& id = cam->config().id;
        try {
          cam->tick(index, sched, now, slate_);
        } catch (const std::exception& e) {
          state_.set("/cameras/" + id + "/tickError", e.what());
        }
        const std::string phase = toString(cam->stats().phase);
        auto& last = lastPhase_[id];
        if (last != phase) {
          last = phase;
          state_.set("/cameras/" + id + "/phase", phase);
        }
        frames.emplace_back(id, cam->lastFrame());
      }
      if (index % ticksPerSecond == 0) {
        for (auto& cam : cameras_) cam->publishStatus();
        state_.set("/engine/overruns", overruns_.load());
      }
    }
    if (tap_) tap_(index, frames);
    const auto elapsed = Clock::now() - start;
    uint64_t next = index + 1;
    if (sched.offset(next) < elapsed) {
      const uint64_t skipTo = sched.nextIndexAtOrAfter(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
      overruns_ += skipTo - next;
      next = skipTo;
    }
    index = next;
  }
  CloseHandle(timer);
  timeEndPeriod(1);
}
}  // namespace bc
