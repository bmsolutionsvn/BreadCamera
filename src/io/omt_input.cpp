#include "io/omt_input.h"

#include <chrono>
#include <cstring>
#include <memory>

#include "omthx/omthx.h"

namespace bc {
using Clock = std::chrono::steady_clock;

void OmtInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
  thread_ = std::thread([this] { run(); });
}

void OmtInput::stop() {
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
}

void OmtInput::run() {
  sink_->onState(SignalState::Connecting, std::nullopt);
  std::unique_ptr<omthx::Receiver> receiver;
  bool live = false;
  auto lastVideo = Clock::now();

  while (!stopping_) {
    if (!receiver) {
      try {
        receiver = std::make_unique<omthx::Receiver>(source_, omthx::PixelFormat::Bgra);
        receiver->requestKeyframe();
        lastVideo = Clock::now();
      } catch (const std::exception& e) {
        sink_->onState(SignalState::Offline,
                       ErrorInfo{"omt-failed", "cannot open OMT source \"" + source_ + "\": " + e.what()});
        // A source may start after the camera does; try again once a second.
        for (int i = 0; i < 10 && !stopping_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
    }

    try {
      omthx::VideoFrame v;
      if (receiver->receiveVideo(v, 20)) {
        const uint32_t w = static_cast<uint32_t>(v.format.width);
        const uint32_t h = static_cast<uint32_t>(v.format.height);
        if (w == 0 || h == 0 || v.bgra.size() < static_cast<size_t>(w) * h * 4) continue;

        VideoFrameIn f;
        f.layout = PixelLayout::BGRA8;
        f.fullRange = true;  // omthx hands back the source's display-referred BGRA
        f.ptsUs = v.timestamp / 10;  // omthx timestamps are 100 ns units
        f.rate = rateFromRational(v.format.fpsN, v.format.fpsD);
        f.tex = pool_.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        {
          auto lock = gpu_.lock();
          gpu_.context()->UpdateSubresource(f.tex->tex.Get(), 0, nullptr, v.bgra.data(), w * 4, 0);
        }
        lastVideo = Clock::now();
        if (!live) {
          live = true;
          sink_->onState(SignalState::Live, std::nullopt);
        }
        sink_->onVideo(std::move(f));
      }

      // Audio is drained with a zero timeout; anything slower than the picture falls behind, so
      // only the newest block of each pass is kept.
      omthx::AudioFrame a;
      while (receiver->receiveAudio(a, 0)) {
        if (a.sampleRate == 48000 && a.channels >= 1 &&
            a.planar.size() >= static_cast<size_t>(a.samples) * static_cast<size_t>(a.channels)) {
          AudioBlock blk;
          blk.samples.resize(static_cast<size_t>(a.samples) * 2);
          for (int i = 0; i < a.samples; ++i) {
            blk.samples[static_cast<size_t>(i) * 2] = a.planar[static_cast<size_t>(i)];
            blk.samples[static_cast<size_t>(i) * 2 + 1] =
                a.planar[static_cast<size_t>(a.samples) + static_cast<size_t>(i)];
          }
          sink_->onAudio(std::move(blk));
        }
      }
    } catch (const std::exception& e) {
      sink_->onState(SignalState::Offline, ErrorInfo{"omt-failed", e.what()});
      receiver.reset();  // open again; a sender restart is the usual cause
      live = false;
    }

    if (live && Clock::now() - lastVideo > std::chrono::seconds(2)) {
      live = false;
      sink_->onState(SignalState::Offline,
                     ErrorInfo{"omt-no-video", "no video from OMT source \"" + source_ + "\" for 2 s"});
      receiver.reset();  // the loop opens it again (and reports a failure to open)
    }
  }

  sink_->onState(SignalState::Offline, std::nullopt);
}
}  // namespace bc