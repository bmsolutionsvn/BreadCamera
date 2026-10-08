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

 private:
  void run();
  void setState(SignalState s, std::optional<ErrorInfo> e);
  BoundedQueue<OutFrame> queue_{2};
  std::thread thread_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex m_;
  OutputStatus status_;
  std::mutex waitMutex_;
  std::condition_variable waitCv_;
};
}  // namespace bc
