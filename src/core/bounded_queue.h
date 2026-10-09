#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace bc {
// Drop-oldest queue between the tick and one output (spec §4.1). push() never blocks,
// so a slow consumer can never slow the producer.
template <class T>
class BoundedQueue {
 public:
  explicit BoundedQueue(size_t capacity) : capacity_(capacity) {}

  bool push(T v) {
    bool dropped = false;
    {
      std::lock_guard l(m_);
      if (closed_) return false;
      if (q_.size() >= capacity_) {
        q_.pop_front();
        ++dropped_;
        dropped = true;
      }
      q_.push_back(std::move(v));
    }
    cv_.notify_one();
    return dropped;
  }

  std::optional<T> pop(std::chrono::milliseconds timeout) {
    std::unique_lock l(m_);
    cv_.wait_for(l, timeout, [&] { return closed_ || !q_.empty(); });
    if (q_.empty()) return std::nullopt;
    T v = std::move(q_.front());
    q_.pop_front();
    return v;
  }

  void close() {
    {
      std::lock_guard l(m_);
      closed_ = true;
      q_.clear();
    }
    cv_.notify_all();
  }

  void reopen() {
    std::lock_guard l(m_);
    closed_ = false;
  }

  uint64_t dropped() const {
    std::lock_guard l(m_);
    return dropped_;
  }

 private:
  mutable std::mutex m_;
  std::condition_variable cv_;
  std::deque<T> q_;
  size_t capacity_;
  bool closed_ = false;
  uint64_t dropped_ = 0;
};
}  // namespace bc
