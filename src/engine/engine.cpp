#include "engine/engine.h"

#include <windows.h>
#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include "core/tick_schedule.h"
#include "gpu/image_io.h"

namespace bc {
using Clock = std::chrono::steady_clock;

Engine::Engine(GpuTools tools, StateStore& state, AdapterFactory& factory)
    : tools_(tools), state_(state), factory_(factory) {
  std::lock_guard l(mutex_);
  loadSlate();
  state_.set("/engine/rate", settings_.rate.str());
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
  // The slate is a *source*: each camera fits it to its own frame size on the no-picture path.
  state_.set("/engine/warnings/slate", "");
  if (!settings_.slatePath.empty()) {
    try {
      auto img = loadImageBgra(settings_.slatePath);
      auto up = tools_.conv.uploadBgra8(img.bgra.data(), img.width, img.height, img.width * 4);
      slate_ = tools_.conv.toLinear(*up, PixelLayout::BGRA8, true);
      return;
    } catch (const std::exception& e) {
      state_.set("/engine/warnings/slate", std::string("slate image could not be loaded: ") + e.what() +
                                               "; using the built-in slate");
    }
  }
  // A base size; the camera fits this to its own frame size when it has no picture.
  slate_ = tools_.conv.solid(1280, 720, {0.05f, 0.05f, 0.06f, 1.0f});
}

void Engine::apply(const Show& show) {
  factory_.setShowRate(show.settings.rate);
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
        auto cam = std::make_unique<Camera>(cfg, factory_, tools_, state_, policy());
        cam->start();
        cameras_.push_back(std::move(cam));
      }
    }
    for (auto& c : cameras_) c->setBudgetRate(show.settings.rate);
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

std::map<std::string, std::vector<CpuImage>> Engine::capture(const std::vector<std::string>& ids, int frames, int every,
                                                             uint32_t w, uint32_t h, std::chrono::milliseconds timeout) {
  auto job = std::make_shared<CaptureJob>();
  job->ids = ids;
  job->frames = frames;
  job->every = every;
  job->w = w;
  job->h = h;
  std::unique_lock l(mutex_);
  for (auto& id : ids) {
    if (std::none_of(cameras_.begin(), cameras_.end(), [&](auto& c) { return c->config().id == id; })) {
      throw std::invalid_argument("no camera " + id);
    }
  }
  if (capture_) throw std::runtime_error("another capture is running");
  if (!running_) throw std::invalid_argument("the engine is not running");
  capture_ = job;
  const bool finished = captureCv_.wait_for(l, timeout, [&] { return job->done; });
  capture_.reset();
  if (!finished) throw std::runtime_error("capture timed out");
  if (!job->error.empty()) throw std::invalid_argument(job->error);
  return std::move(job->got);
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
    std::vector<FrameTap> frames;
    {
      std::lock_guard l(mutex_);
      // Match wiring (spec D9, Task 7), before half 1: hand every Match camera its reference's
      // measured output, and mark the referenced cameras so they measure their output even when
      // they are not in Match mode themselves.
      for (auto& cam : cameras_) cam->setMatchedBy(false);
      for (auto& cam : cameras_) {
        if (!cam->matchMode()) continue;
        const std::string to = cam->matchTo();
        if (to.empty()) continue;  // Auto balance needs no reference
        Camera* ref = nullptr;
        for (auto& c : cameras_) {
          if (c->config().id == to) {
            ref = c.get();
            break;
          }
        }
        cam->setMatchReference(ref ? ref->matchOutputStats() : std::nullopt,
                               ref != nullptr && ref->tickPhase() == FeedPhase::Live);
        if (ref) ref->setMatchedBy(true);
      }
      // Half 1: every camera picks and converts.
      for (auto& cam : cameras_) {
        try {
          cam->beginTick(now, slate_);
        } catch (const std::exception& e) {
          state_.set("/cameras/" + cam->config().id + "/tickError", e.what());
        }
      }
      if (capture_ && !capture_->done && index % static_cast<uint64_t>(capture_->every) == 0) {
        auto& job = *capture_;
        for (auto& cam : cameras_) {
          const auto& id = cam->config().id;
          if (std::find(job.ids.begin(), job.ids.end(), id) == job.ids.end()) continue;
          if (cam->tickPhase() != FeedPhase::Live) {
            job.error = "camera \"" + cam->config().name + "\" (" + id + ") has no picture";
            break;
          }
          auto& v = job.got[id];
          if (static_cast<int>(v.size()) < job.frames) {
            v.push_back(tools_.conv.readback(*tools_.conv.toBgra8(*cam->tickRaw(), job.w, job.h)));
          }
        }
        bool all = job.error.empty();
        for (auto& id : job.ids) all = all && static_cast<int>(job.got[id].size()) >= job.frames;
        if (all || !job.error.empty()) {
          job.done = true;
          captureCv_.notify_all();
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
          const bool previewWanted = previewWanted_ && previewWanted_(id);
          cam->endTick(index, sched, now, alphas.count(cam.get()) ? alphas[cam.get()] : nullptr, previewWanted);
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
        frames.push_back(FrameTap{id, cam->lastFrame(), cam->tickCpu()});
      }
      if (index % ticksPerSecond == 0) {
        for (auto& cam : cameras_) cam->publishStatus();
        state_.set("/engine/overruns", overruns_.load());
        if (matte_) state_.set("/engine/matteMs", matte_->lastInferenceMs());
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
