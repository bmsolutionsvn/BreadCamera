#pragma once
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>

namespace bc {
class EngineLauncher {
 public:
  virtual ~EngineLauncher() = default;
  virtual bool isRunning() = 0;
  virtual void launch() = 0;
  virtual void kill() = 0;
};

// Engine restart policy (spec §2). poll() every ~250 ms; heartbeat() on every ping reply.
class Supervisor {
 public:
  using Clock = std::chrono::steady_clock;
  struct Policy {
    std::chrono::milliseconds heartbeatTimeout{3000};
    std::chrono::milliseconds startupGrace{8000};
    int quickRestarts = 5;
    std::chrono::milliseconds quickWindow{60000};
    std::chrono::milliseconds slowInterval{10000};
  };
  explicit Supervisor(EngineLauncher& launcher) : Supervisor(launcher, Policy{}) {}
  Supervisor(EngineLauncher& launcher, Policy policy) : launcher_(launcher), policy_(policy) {}
  void heartbeat(Clock::time_point now);
  void poll(Clock::time_point now);
  int launches() const;
  bool crashLooping(Clock::time_point now) const;
  void stopSupervising();

 private:
  bool canLaunch(Clock::time_point now) const;
  int recentLaunches(Clock::time_point now) const;
  EngineLauncher& launcher_;
  Policy policy_;
  mutable std::mutex m_;
  bool stopped_ = false;
  bool baselineSet_ = false;
  std::optional<Clock::time_point> lastBeat_;
  std::optional<Clock::time_point> lastLaunch_;
  std::deque<Clock::time_point> launchTimes_;
  int launchCount_ = 0;
};
}  // namespace bc
