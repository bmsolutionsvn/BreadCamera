#include "io/queued_output.h"

#include <cstdlib>

#include "core/backoff.h"

namespace bc {
using namespace std::chrono_literals;

QueuedOutput::~QueuedOutput() {
  if (thread_.joinable()) std::abort();  // a subclass forgot to call stop() in its destructor
}

void QueuedOutput::start() {
  if (thread_.joinable()) return;
  stopping_ = false;
  queue_.reopen();
  thread_ = std::thread([this] { run(); });
}

void QueuedOutput::submit(OutFrame frame) { queue_.push(std::move(frame)); }

void QueuedOutput::stop() {
  stopping_ = true;
  queue_.close();
  waitCv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

OutputStatus QueuedOutput::status() const {
  std::lock_guard l(m_);
  OutputStatus s = status_;
  s.dropped = queue_.dropped();
  return s;
}

void QueuedOutput::setState(SignalState s, std::optional<ErrorInfo> e) {
  std::lock_guard l(m_);
  status_.state = s;
  status_.error = std::move(e);
}

void QueuedOutput::run() {
  Backoff backoff;
  while (!stopping_) {
    setState(SignalState::Connecting, std::nullopt);
    try {
      open();
      bool live = false;
      while (!stopping_) {
        auto f = queue_.pop(100ms);
        if (!f) continue;
        deliver(*f);
        {
          std::lock_guard l(m_);
          ++status_.delivered;
        }
        if (!live) {
          live = true;
          setState(SignalState::Live, std::nullopt);
          backoff.reset();
        }
      }
      close();
    } catch (const std::exception& e) {
      close();
      setState(SignalState::Offline, ErrorInfo{errorCode(), e.what()});
      std::unique_lock l(waitMutex_);
      waitCv_.wait_for(l, backoff.next(), [&] { return stopping_.load(); });
    }
  }
  setState(SignalState::Offline, std::nullopt);
}
}  // namespace bc
