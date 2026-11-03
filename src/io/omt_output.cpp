#include "io/omt_output.h"

#include <stdexcept>

namespace bc {
OmthOutput::OmthOutput(std::string name, omthx::Mode mode, omthx::Codec codec, bool keyed)
    : name_(std::move(name)), mode_(mode), codec_(codec), keyed_(keyed) {}

std::string OmthOutput::address() const {
  std::lock_guard lock(addrMutex_);
  return address_;
}

void OmthOutput::open() {
  send_ = std::make_unique<omthx::Sender>(name_, mode_, omthx::Quality::Medium, codec_);
  std::lock_guard lock(addrMutex_);
  address_ = send_->address();
}

void OmthOutput::refuse(const std::string& why) {
  if (gpuRefused_.exchange(true)) return;  // the first reason is the one that sticks
  std::lock_guard lock(warnMutex_);
  warn_ = "OMT HX is sending through memory: " + why;
}

void OmthOutput::deliver(const OutFrame& f) {
  // A tick the queue dropped needs no keyframe: the encoder never saw it, and omthx numbers the coded
  // frames, so the receiver plays on.
  const int64_t ts = static_cast<int64_t>(f.tick) * 10'000'000LL * f.rate.den / f.rate.num;
  bool sentVideo = false;
  // The GPU path's size check runs on the frame alone, so a refused path is reported even while nobody
  // pulls and the camera makes no picture: a receiver that connects then reads back instead.
  const bool gpu = !f.hasAlpha && gpuPath();
  const uint32_t width = f.rgba8 ? f.rgba8->width : f.linear ? f.linear->width : 0;
  const uint32_t height = f.rgba8 ? f.rgba8->height : f.linear ? f.linear->height : 0;
  if (gpu && (width % 2 != 0 || height % 2 != 0)) {
    refuse("the frame is " + std::to_string(width) + "x" + std::to_string(height) +
           " and the GPU encoder needs even sizes");
  } else if (gpu && f.rgba8) {
    // The GPU path takes no alpha, so a frame with it stays on the CPU path as it always has.
    format_.width = static_cast<int>(f.rgba8->width);
    format_.height = static_cast<int>(f.rgba8->height);
    format_.fpsN = static_cast<int>(f.rate.num);
    format_.fpsD = static_cast<int>(f.rate.den);
    format_.alpha = false;
    try {
      // No GpuDevice::lock() here: an encode can take milliseconds and would stall the tick. The
      // device's immediate context is multithread-protected (omthx ensures it too).
      send_->sendVideoD3D11(format_, f.rgba8->tex.Get(), ts);
      sentVideo = true;
    } catch (const std::exception& e) {
      refuse(e.what());
      return;  // this frame is dropped; the next one comes through memory
    }
  }

  if (!sentVideo && f.bgra && !f.bgra->bgra.empty()) {
    format_.width = static_cast<int>(f.bgra->width);
    format_.height = static_cast<int>(f.bgra->height);
    format_.fpsN = static_cast<int>(f.rate.num);
    format_.fpsD = static_cast<int>(f.rate.den);
    format_.alpha = f.hasAlpha;
    // BreadCamera's BGRA is straight, so premultiplied == false.
    send_->sendVideo(format_, f.bgra->bgra, false, ts);
  }

  if (f.audio && !f.audio->empty()) {
    const int ch = f.channels;
    const int n = static_cast<int>(f.audio->size()) / ch;
    planar_.resize(f.audio->size());
    for (int c = 0; c < ch; ++c)
      for (int i = 0; i < n; ++i) planar_[static_cast<size_t>(c) * n + i] = (*f.audio)[static_cast<size_t>(i) * ch + c];
    send_->sendAudio(planar_, f.sampleRate, ch, ts);
  }

  const omthx::Tally t = send_->tally();  // a receiver may have set program/preview
  program_ = t.program;
  preview_ = t.preview;
  receivers_ = send_->hasReceivers();  // the next tick asks for a picture only if someone is pulling
}

OutputStatus OmthOutput::status() const {
  OutputStatus s = QueuedOutput::status();
  s.program = program_;
  s.preview = preview_;
  if (gpuRefused_) {
    // The output has no state path of its own, so the refusal rides in the error (state stays Live).
    std::lock_guard lock(warnMutex_);
    if (!s.error) s.error = ErrorInfo{"omt-gpu-refused", warn_};
  }
  return s;
}

void OmthOutput::close() noexcept {
  receivers_ = false;
  send_.reset();
  std::lock_guard lock(addrMutex_);
  address_.clear();
}
}  // namespace bc
