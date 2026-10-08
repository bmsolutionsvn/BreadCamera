#include "core/audio_delay.h"

#include <algorithm>

namespace bc {
void AudioDelay::setDelay(std::chrono::milliseconds d) {
  std::lock_guard l(m_);
  delay_ = d;
  buf_.assign(delayFrames() * channels_, 0.0f);  // prime with silence
}

void AudioDelay::push(std::span<const float> interleaved) {
  std::lock_guard l(m_);
  buf_.insert(buf_.end(), interleaved.begin(), interleaved.end());
  const size_t maxFrames = delayFrames() + static_cast<size_t>(rate_) / 10;
  if (buf_.size() / channels_ > maxFrames) {
    const size_t excess = buf_.size() - delayFrames() * channels_;
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(excess));
  }
}

std::vector<float> AudioDelay::pull(size_t frames) {
  std::lock_guard l(m_);
  std::vector<float> out(frames * channels_, 0.0f);
  const size_t have = std::min(out.size(), buf_.size());
  std::copy(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(have), out.begin());
  buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(have));
  if (have < out.size()) ++underruns_;
  return out;
}

size_t AudioDelay::bufferedFrames() const {
  std::lock_guard l(m_);
  return buf_.size() / channels_;
}

uint64_t AudioDelay::underruns() const {
  std::lock_guard l(m_);
  return underruns_;
}
}  // namespace bc
