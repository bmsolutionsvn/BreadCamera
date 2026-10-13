#include "io/omt_output.h"

#include <stdexcept>

namespace bc {
OmthOutput::OmthOutput(std::string name, omthx::Mode mode) : name_(std::move(name)), mode_(mode) {}

std::string OmthOutput::address() const {
  std::lock_guard lock(addrMutex_);
  return address_;
}

void OmthOutput::open() {
  send_ = std::make_unique<omthx::Sender>(name_, mode_, omthx::Quality::Medium);
  std::lock_guard lock(addrMutex_);
  address_ = send_->address();
}

void OmthOutput::deliver(const OutFrame& f) {
  if (consumeDrop()) send_->requestKeyframe();  // the tick outran us: ask for a fresh keyframe

  if (f.bgra && !f.bgra->bgra.empty()) {
    format_.width = static_cast<int>(f.bgra->width);
    format_.height = static_cast<int>(f.bgra->height);
    format_.fpsN = static_cast<int>(f.rate.num);
    format_.fpsD = static_cast<int>(f.rate.den);
    format_.alpha = f.hasAlpha;
    const int64_t ts = static_cast<int64_t>(f.tick) * 10'000'000LL * f.rate.den / f.rate.num;
    // BreadCamera's BGRA is straight, so premultiplied == false.
    send_->sendVideo(format_, f.bgra->bgra, false, ts);
  }

  if (f.audio && !f.audio->empty()) {
    const int ch = f.channels;
    const int n = static_cast<int>(f.audio->size()) / ch;
    planar_.resize(f.audio->size());
    for (int c = 0; c < ch; ++c)
      for (int i = 0; i < n; ++i) planar_[static_cast<size_t>(c) * n + i] = (*f.audio)[static_cast<size_t>(i) * ch + c];
    const int64_t ts = static_cast<int64_t>(f.tick) * 10'000'000LL * f.rate.den / f.rate.num;
    send_->sendAudio(planar_, f.sampleRate, ch, ts);
  }
}

void OmthOutput::close() noexcept {
  send_.reset();
  std::lock_guard lock(addrMutex_);
  address_.clear();
}
}  // namespace bc
