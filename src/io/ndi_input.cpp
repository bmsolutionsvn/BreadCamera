#include "io/ndi_input.h"

#include <Processing.NDI.Lib.h>

#include <chrono>
#include <cstring>

namespace bc {
using Clock = std::chrono::steady_clock;

void NdiInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
  thread_ = std::thread([this] { run(); });
}

void NdiInput::stop() {
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
}

void NdiInput::run() {
  static const bool ok = NDIlib_initialize();
  if (!ok) {
    sink_->onState(SignalState::Offline, ErrorInfo{"ndi-failed", "the NDI runtime could not start"});
    return;
  }
  sink_->onState(SignalState::Connecting, std::nullopt);
  NDIlib_find_create_t fc{};
  fc.show_local_sources = true;
  NDIlib_find_instance_t find = NDIlib_find_create_v2(&fc);
  if (!find) {
    sink_->onState(SignalState::Offline, ErrorInfo{"ndi-failed", "cannot start NDI discovery"});
    return;
  }
  NDIlib_recv_instance_t recv = nullptr;
  bool live = false, rateReported = false, flipReported = false;  // ponytail: flipped frames are reported once and skipped
  auto lastVideo = Clock::now();
  while (!stopping_) {
    if (!recv) {
      NDIlib_find_wait_for_sources(find, 200);
      uint32_t n = 0;
      const NDIlib_source_t* srcs = NDIlib_find_get_current_sources(find, &n);
      const NDIlib_source_t* pick = nullptr;
      for (uint32_t i = 0; i < n; ++i) if (source_ == srcs[i].p_ndi_name) pick = &srcs[i];
      for (uint32_t i = 0; i < n && !pick; ++i) if (std::strstr(srcs[i].p_ndi_name, source_.c_str())) pick = &srcs[i];
      if (!pick) continue;
      NDIlib_recv_create_v3_t rc{};
      rc.source_to_connect_to = *pick;
      rc.color_format = NDIlib_recv_color_format_BGRX_BGRA;
      // NDI|HX is an NDI source like any other (the SDK decodes it); HX mode asks for the
      // source's lowest-bandwidth stream, which is the HX one, instead of full bandwidth.
      rc.bandwidth = hx_ ? NDIlib_recv_bandwidth_lowest : NDIlib_recv_bandwidth_highest;
      rc.allow_video_fields = !hx_;
      rc.p_ndi_recv_name = "BreadCamera";
      recv = NDIlib_recv_create_v3(&rc);
      lastVideo = Clock::now();
      continue;
    }
    NDIlib_video_frame_v2_t v{};
    NDIlib_audio_frame_v3_t a{};
    switch (NDIlib_recv_capture_v3(recv, &v, &a, nullptr, 200)) {
      case NDIlib_frame_type_video: {
        const uint32_t w = static_cast<uint32_t>(v.xres), h = static_cast<uint32_t>(v.yres);
        VideoFrameIn f;
        f.layout = PixelLayout::BGRA8;
        f.fullRange = true;
        f.ptsUs = v.timestamp == NDIlib_recv_timestamp_undefined ? 0 : v.timestamp / 10;
        f.rate = rateFromRational(v.frame_rate_N, v.frame_rate_D);
        f.tex = pool_.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        if (v.line_stride_in_bytes < 0) {  // bottom-up source; we did not ask for the _flipped format
          if (!flipReported) {
            flipReported = true;
            sink_->onState(live ? SignalState::Live : SignalState::Connecting,
                           ErrorInfo{"ndi-format", "flipped NDI video frames are not supported"});
          }
          NDIlib_recv_free_video_v2(recv, &v);
          break;
        }
        {
          // NDI: stride 0 means tightly packed (sizeof(one pixel) * xres).
          const UINT pitch = v.line_stride_in_bytes > 0 ? static_cast<UINT>(v.line_stride_in_bytes) : w * 4;
          auto lock = gpu_.lock();
          gpu_.context()->UpdateSubresource(f.tex->tex.Get(), 0, nullptr, v.p_data, pitch, 0);
        }
        NDIlib_recv_free_video_v2(recv, &v);
        lastVideo = Clock::now();
        if (!live) {
          live = true;
          sink_->onState(SignalState::Live, std::nullopt);
        }
        sink_->onVideo(std::move(f));
        break;
      }
      case NDIlib_frame_type_audio: {
        if (a.sample_rate == 48000 && a.no_channels >= 1) {
          AudioBlock blk;
          blk.samples.resize(static_cast<size_t>(a.no_samples) * 2);
          const auto* base = reinterpret_cast<const uint8_t*>(a.p_data);
          const auto* left = reinterpret_cast<const float*>(base);
          const auto* right = a.no_channels > 1 ? reinterpret_cast<const float*>(base + a.channel_stride_in_bytes) : left;
          for (int i = 0; i < a.no_samples; ++i) {
            blk.samples[static_cast<size_t>(i) * 2] = left[i];
            blk.samples[static_cast<size_t>(i) * 2 + 1] = right[i];
          }
          sink_->onAudio(std::move(blk));
        } else if (!rateReported) {
          rateReported = true;
          sink_->onState(live ? SignalState::Live : SignalState::Connecting,
                         ErrorInfo{"ndi-audio-rate", "NDI audio at " + std::to_string(a.sample_rate) + " Hz is ignored; only 48 kHz is used"});
        }
        NDIlib_recv_free_audio_v3(recv, &a);
        break;
      }
      default:
        break;
    }
    if (live && Clock::now() - lastVideo > std::chrono::seconds(2)) {
      live = false;
      sink_->onState(SignalState::Offline, ErrorInfo{"ndi-no-video", "no video from NDI source \"" + source_ + "\" for 2 s"});
    }
  }
  if (recv) NDIlib_recv_destroy(recv);
  NDIlib_find_destroy(find);
  sink_->onState(SignalState::Offline, std::nullopt);
}
}  // namespace bc
