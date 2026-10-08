#pragma once
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace bc {
enum class FeedPhase { NoSignal, Live, Holding, Slate };

inline const char* toString(FeedPhase p) {
  switch (p) {
    case FeedPhase::NoSignal: return "nosignal";
    case FeedPhase::Live: return "live";
    case FeedPhase::Holding: return "holding";
    case FeedPhase::Slate: return "slate";
  }
  return "nosignal";
}

struct SyncPolicy {
  std::chrono::milliseconds stallAfter{250};  // no new frame for this long = input stalled
  std::chrono::milliseconds hold{5000};       // hold the last frame this long, then slate
};

// Latest-frame slot between an input thread and the master tick (spec §3.3).
// The tick always takes the newest frame; slower inputs repeat, faster ones drop.
template <class Frame>
class FrameSync {
 public:
  using Clock = std::chrono::steady_clock;
  struct Pick {
    std::optional<Frame> frame;  // empty for NoSignal and Slate
    FeedPhase phase;
    bool fresh;                  // true when this pick took a frame not seen before
  };

  explicit FrameSync(SyncPolicy p) : policy_(p) {}

  void setPolicy(SyncPolicy p) {
    std::lock_guard l(m_);
    policy_ = p;
  }

  void push(Frame f, Clock::time_point arrival) {
    std::lock_guard l(m_);
    if (pending_) ++drops_;
    pending_ = std::move(f);
    lastArrival_ = arrival;
  }

  Pick pick(Clock::time_point now) {
    std::lock_guard l(m_);
    bool fresh = false;
    if (pending_) {
      current_ = std::move(pending_);
      pending_.reset();
      fresh = true;
    } else if (current_) {
      ++repeats_;
    }
    FeedPhase phase = FeedPhase::NoSignal;
    if (lastArrival_) {
      auto gap = now - *lastArrival_;
      if (gap <= policy_.stallAfter) phase = FeedPhase::Live;
      else if (gap <= policy_.stallAfter + policy_.hold) phase = FeedPhase::Holding;
      else phase = FeedPhase::Slate;
    }
    std::optional<Frame> out;
    if (phase == FeedPhase::Live || phase == FeedPhase::Holding) out = current_;
    return {std::move(out), phase, fresh};
  }

  void reset() {
    std::lock_guard l(m_);
    pending_.reset();
    current_.reset();
    lastArrival_.reset();
  }

  uint64_t drops() const {
    std::lock_guard l(m_);
    return drops_;
  }
  uint64_t repeats() const {
    std::lock_guard l(m_);
    return repeats_;
  }

 private:
  mutable std::mutex m_;
  SyncPolicy policy_;
  std::optional<Frame> pending_;
  std::optional<Frame> current_;
  std::optional<Clock::time_point> lastArrival_;
  uint64_t drops_ = 0;
  uint64_t repeats_ = 0;
};
}  // namespace bc
