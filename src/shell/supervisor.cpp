#include "shell/supervisor.h"

namespace bc {
void Supervisor::heartbeat(Clock::time_point now) {
  std::lock_guard l(m_);
  lastBeat_ = now;
}

int Supervisor::recentLaunches(Clock::time_point now) const {
  int n = 0;
  for (auto t : launchTimes_) n += (now - t < policy_.quickWindow) ? 1 : 0;
  return n;
}

bool Supervisor::crashLooping(Clock::time_point now) const {
  std::lock_guard l(m_);
  return recentLaunches(now) >= policy_.quickRestarts;
}

bool Supervisor::canLaunch(Clock::time_point now) const {
  if (recentLaunches(now) < policy_.quickRestarts) return true;
  return !lastLaunch_ || now - *lastLaunch_ >= policy_.slowInterval;
}

void Supervisor::poll(Clock::time_point now) {
  std::lock_guard l(m_);
  if (stopped_) return;
  if (!baselineSet_) {  // an adopted engine gets the same grace as a fresh one
    baselineSet_ = true;
    lastBeat_ = now;
    if (!lastLaunch_) lastLaunch_ = now;
  }
  bool running = launcher_.isRunning();
  const bool inGrace = lastLaunch_ && now - *lastLaunch_ < policy_.startupGrace;
  if (running && !inGrace && lastBeat_ && now - *lastBeat_ > policy_.heartbeatTimeout) {
    launcher_.kill();
    running = false;
  }
  if (running || !canLaunch(now)) return;
  launcher_.launch();
  ++launchCount_;
  lastLaunch_ = now;
  lastBeat_ = now;
  launchTimes_.push_back(now);
  while (launchTimes_.size() > 32) launchTimes_.pop_front();
}

int Supervisor::launches() const {
  std::lock_guard l(m_);
  return launchCount_;
}

void Supervisor::stopSupervising() {
  std::lock_guard l(m_);
  stopped_ = true;
}
}  // namespace bc
