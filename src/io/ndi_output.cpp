#include "io/ndi_output.h"

#include <Processing.NDI.Lib.h>

#include <mutex>
#include <stdexcept>

namespace bc {
NdiOutput::NdiOutput(std::string sourceName) : name_(std::move(sourceName)) {}

void NdiOutput::open() {
  static const bool ok = NDIlib_initialize();
  if (!ok) throw std::runtime_error("the NDI runtime could not start on this CPU");
  NDIlib_send_create_t c{};
  c.p_ndi_name = name_.c_str();
  c.clock_video = false;  // the camera's input paces us
  c.clock_audio = false;
  send_ = NDIlib_send_create(&c);
  if (!send_) throw std::runtime_error("cannot create NDI source '" + name_ + "'");
}

void NdiOutput::deliver(const OutFrame& f) {
  if (f.bgra && !f.bgra->bgra.empty()) {
    NDIlib_video_frame_v2_t v{};
    v.xres = static_cast<int>(f.bgra->width);
    v.yres = static_cast<int>(f.bgra->height);
    v.FourCC = f.hasAlpha ? NDIlib_FourCC_type_BGRA : NDIlib_FourCC_type_BGRX;
    v.frame_rate_N = f.rate.num;
    v.frame_rate_D = f.rate.den;
    v.picture_aspect_ratio = 0.0f;  // = xres / yres
    v.frame_format_type = NDIlib_frame_format_type_progressive;
    v.timecode = NDIlib_send_timecode_synthesize;
    v.p_data = const_cast<uint8_t*>(f.bgra->bgra.data());
    v.line_stride_in_bytes = v.xres * 4;
    NDIlib_send_send_video_v2(send_, &v);
  }
  if (f.audio && !f.audio->empty()) {
    const int ch = f.channels;
    const int n = static_cast<int>(f.audio->size()) / ch;
    planar_.resize(f.audio->size());
    for (int c = 0; c < ch; ++c)
      for (int i = 0; i < n; ++i) planar_[static_cast<size_t>(c) * n + i] = (*f.audio)[static_cast<size_t>(i) * ch + c];
    NDIlib_audio_frame_v3_t a{};
    a.sample_rate = f.sampleRate;
    a.no_channels = ch;
    a.no_samples = n;
    a.timecode = NDIlib_send_timecode_synthesize;
    a.FourCC = NDIlib_FourCC_audio_type_FLTP;
    a.p_data = reinterpret_cast<uint8_t*>(planar_.data());
    a.channel_stride_in_bytes = n * static_cast<int>(sizeof(float));
    NDIlib_send_send_audio_v3(send_, &a);
  }
}

void NdiOutput::close() noexcept {
  if (send_) {
    NDIlib_send_destroy(send_);
    send_ = nullptr;
  }
}
}  // namespace bc
