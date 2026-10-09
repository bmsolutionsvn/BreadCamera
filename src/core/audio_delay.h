#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

namespace bc {
// Interleaved float FIFO that releases audio `delay` after it arrives, so it stays in
// sync with the delayed video (spec §3.5).
// ponytail: no clock-drift resampling; a buffer that grows 100 ms past the delay is
// trimmed (one audible skip). Add an adaptive resampler if long shows drift audibly.
class AudioDelay {
 public:
  AudioDelay(int sampleRate, int channels) : rate_(sampleRate), channels_(channels) {}
  void setDelay(std::chrono::milliseconds d);
  void push(std::span<const float> interleaved);
  std::vector<float> pull(size_t frames);
  size_t bufferedFrames() const;
  uint64_t underruns() const;
  int sampleRate() const { return rate_; }
  int channels() const { return channels_; }

 private:
  size_t delayFrames() const { return static_cast<size_t>(rate_) * delay_.count() / 1000; }
  mutable std::mutex m_;
  int rate_;
  int channels_;
  std::chrono::milliseconds delay_{0};
  std::deque<float> buf_;
  uint64_t underruns_ = 0;
};
}  // namespace bc
