#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "core/bounded_queue.h"
#include "io/output.h"

namespace bc {
// Drop-oldest queue of 2 + one worker thread + retry with backoff (spec §4.1, §3.6).
// Subclass destructors MUST call stop() so the worker never calls into a destroyed subclass.
class QueuedOutput : public Output {
 public:
  ~QueuedOutput() override;
  void start() override;
  void submit(OutFrame frame) override;
  void stop() override;
  OutputStatus status() const override;

 protected:
  virtual void open() = 0;                        // on the worker thread
  virtual void deliver(const OutFrame& frame) = 0;  // on the worker thread
  virtual void close() noexcept = 0;              // on the worker thread
  virtual std::string errorCode() const { return "output-failed"; }
  bool stopRequested() const { return stopping_; }  // FFmpeg interrupt callbacks
  // Latches a drop: true at most once per delivery when the queue discarded a frame since the last call.
  bool consumeDrop() { return dropPending_.exchange(false); }

 private:
  void run();
  void setState(SignalState s, std::optional<ErrorInfo> e);
  BoundedQueue<OutFrame> queue_{2};
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> dropPending_{false};
  mutable std::mutex m_;
  OutputStatus status_;
  std::mutex waitMutex_;
  std::condition_variable waitCv_;
};
}  // namespace bc
