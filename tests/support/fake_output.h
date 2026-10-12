#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "io/queued_output.h"

namespace bc::test {
// Records what it receives. Can be slowed down or made to throw.
class FakeOutput final : public bc::QueuedOutput {
 public:
  struct Seen {
    uint64_t tick;
    std::chrono::steady_clock::time_point at;
    std::array<uint8_t, 4> firstPixel;  // BGRA of pixel (0,0); zeros when no CPU image
    std::array<uint8_t, 4> centerPixel;  // BGRA of the centre pixel; zeros when no CPU image
    uint32_t width;
    size_t audioSamples;
  };

  explicit FakeOutput(bool needsCpu = true) : needsCpu_(needsCpu) {}
  ~FakeOutput() override { stop(); }
  bool needsCpu() const override { return needsCpu_; }

  std::atomic<int> opens{0};
  std::atomic<int> delivered{0};
  std::atomic<int> failOnDelivery{-1};  // throw when the delivery count reaches this
  std::atomic<int> delayMs{0};

  std::vector<Seen> seen() {
    std::lock_guard l(m_);
    return seen_;
  }

  // The most recent frame with a CPU image (null until one arrives).
  std::shared_ptr<const bc::CpuImage> lastImage() const {
    std::lock_guard l(m_);
    return lastImage_;
  }

 protected:
  void open() override { ++opens; }
  void deliver(const bc::OutFrame& f) override {
    if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs.load()));
    const int n = ++delivered;
    if (n == failOnDelivery) throw std::runtime_error("boom");
    Seen s{f.tick, std::chrono::steady_clock::now(), {0, 0, 0, 0}, {0, 0, 0, 0}, 0, f.audio ? f.audio->size() : 0};
    if (f.bgra && !f.bgra->bgra.empty()) {
      std::copy_n(f.bgra->bgra.begin(), 4, s.firstPixel.begin());
      const size_t c = (static_cast<size_t>(f.bgra->height / 2) * f.bgra->width + f.bgra->width / 2) * 4;
      std::copy_n(f.bgra->bgra.begin() + c, 4, s.centerPixel.begin());
      s.width = f.bgra->width;
    } else if (f.linear) {
      s.width = f.linear->width;
    }
    std::lock_guard l(m_);
    if (f.bgra) lastImage_ = f.bgra;
    seen_.push_back(s);
  }
  void close() noexcept override {}

 private:
  bool needsCpu_;
  mutable std::mutex m_;
  std::vector<Seen> seen_;
  std::shared_ptr<const bc::CpuImage> lastImage_;
};
}  // namespace bc::test
