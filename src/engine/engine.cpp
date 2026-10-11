#include "engine/engine.h"

#include <windows.h>
#include <timeapi.h>

#include <cmath>
#include "core/tick_schedule.h"
#include "gpu/image_io.h"

namespace bc {
using Clock = std::chrono::steady_clock;

Engine::Engine(Converter& conv, Compositor& comp, StateStore& state, AdapterFactory& factory)
    : conv_(conv), comp_(comp), state_(state), factory_(factory) {
  std::lock_guard l(mutex_);
  loadSlate();
  state_.set("/engine/rate", settings_.rate.str());
  state_.set("/engine/resolution", resolutionStr(settings_.width, settings_.height));
  state_.set("/engine/overruns", 0);
}

void Engine::setMatteBackend(std::unique_ptr<MatteBackend> backend, std::string unavailableReason) {
  std::lock_guard l(mutex_);
  matte_ = std::move(backend);
  matteUnavailable_ = std::move(unavailableReason);
  state_.set("/engine/matteBackend", matte_ ? matte_->name() : std::string());
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
      slate_ = conv_.fit(*conv_.toLinear(*up, PixelLayout::BGRA8, true), settings_.width, settings_.height);
      return;
    } catch (const std::exception& e) {
      state_.set("/engine/warnings/slate", std::string("slate image could not be loaded: ") + e.what() +
                                               "; using the built-in slate");
    }
  }
  slate_ = conv_.solid(settings_.width, settings_.height, {0.05f, 0.05f, 0.06f, 1.0f});
}

void Engine::apply(const Show& show) {
  std::vector<std::unique_ptr<Camera>> removed;
  {
    std::lock_guard l(mutex_);
    if (!settingsApplied_ || !(show.settings == settings_)) {
      const bool slateChanged = !settingsApplied_ || show.settings.slatePath != settings_.slatePath;
      const bool sizeChanged = !settingsApplied_ || show.settings.width != settings_.width ||
                               show.settings.height != settings_.height;
      rateChanged_ = rateChanged_ || !(show.settings.rate == settings_.rate);
      settings_ = show.settings;
      settingsApplied_ = true;
      if (slateChanged || sizeChanged) loadSlate();
      state_.set("/engine/rate", settings_.rate.str());
      state_.set("/engine/resolution", resolutionStr(settings_.width, settings_.height));
      for (auto& c : cameras_) c->setPolicy(policy());
      if (sizeChanged)
        for (auto& c : cameras_) c->setFormat(settings_.width, settings_.height);
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
        auto cam = std::make_unique<Camera>(cfg, factory_, conv_, comp_, state_, policy());
        cam->setFormat(settings_.width, settings_.height);
        cam->start();
        cameras_.push_back(std::move(cam));
      }
    }
    for (auto& [id, cam] : existing) {
      state_.erase("/cameras/" + id);
      lastPhase_.erase(id);
      if (matte_) matte_->forget(id);
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
  Clock::time_point start = Clock::now();
  uint64_t index = 0;
  uint64_t ticksPerSecond = 60;
  while (running_) {
    {
      std::lock_guard l(mutex_);
      if (rateChanged_) {
        sched = TickSchedule(settings_.rate);
        ticksPerSecond = static_cast<uint64_t>(std::lround(settings_.rate.fps()));
        start = Clock::now();
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
      // Half 1: every camera picks and converts.
      for (auto& cam : cameras_) {
        try {
          cam->beginTick(now, slate_);
        } catch (const std::exception& e) {
          state_.set("/cameras/" + cam->config().id + "/tickError", e.what());
        }
      }
      // One batched matte call for every camera that wants one this tick (spec §3.4).
      std::vector<MatteJob> jobs;
      std::vector<Camera*> jobCams;
      for (auto& cam : cameras_) {
        if (!cam->wantsMatte(now)) continue;
        const bool perf = cam->matteParams().value("quality", std::string("quality")) == "performance";
        jobs.push_back({cam->config().id, cam->tickFrame(),
                        perf ? MatteQuality::Performance : MatteQuality::Quality, nullptr});
        jobCams.push_back(cam.get());
      }
      std::map<Camera*, TextureRef> alphas;
      if (!jobs.empty() && !matte_) {
        for (auto* c : jobCams) c->matteGuard().failed(ErrorInfo{"matte-unavailable", matteUnavailable_}, now);
      } else if (!jobs.empty()) {
        std::vector<MatteJob*> ptrs;
        for (auto& j : jobs) ptrs.push_back(&j);
        const auto t0 = Clock::now();
        try {
          matte_->process(ptrs);
          const auto took = Clock::now() - t0;
          const auto budget = std::chrono::duration_cast<std::chrono::nanoseconds>(sched.rate().period() * 6 / 10);
          for (size_t i = 0; i < jobs.size(); ++i) {
            if (!jobs[i].alpha) {
              jobCams[i]->matteGuard().failed("the matte backend returned no matte", now);
              continue;
            }
            jobCams[i]->matteGuard().succeeded(took, budget, now);
            if (!jobCams[i]->matteGuard().bypassed()) alphas[jobCams[i]] = jobs[i].alpha;
          }
        } catch (const std::exception& e) {
          for (auto* c : jobCams) c->matteGuard().failed(e.what(), now);
        }
      }
      // Half 2: stages, readback, fan-out.
      for (auto& cam : cameras_) {
        const auto& id = cam->config().id;
        try {
          cam->endTick(index, sched, now, alphas.count(cam.get()) ? alphas[cam.get()] : nullptr);
          // The error block must clear once the tick is clean again (spec §6.4: an
          // error is persistent *while true*, never forever).
          state_.erase("/cameras/" + id + "/tickError");
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
