#include "io/omt_input.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "omthx/omthx.h"

#include "gpu/nv12_upload.h"

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
  // A recorder takes full OMT (VMX) frames as they are; an Hx source cannot be passed through,
  // so it reopens as decoded pictures (the same path a Camera sink uses).
  bool coded = sink_->wantsCodedVmx();
  // An HX source decodes to full-range BT.709 NV12 for a GPU sink. A sink that wants CPU frames
  // (BreadReplay's recorder tap) keeps the BGRA receiver it had before, so it still gets pictures;
  // one that wants neither keeps the old BGRA receiver too.
  bool nv12 = sink_->wantsGpuVideo() && !sink_->wantsCpuVideo();
  bool checkedMode = false;
  bool live = false;
  auto lastVideo = Clock::now();

  while (!stopping_) {
    if (!receiver) {
      try {
        receiver = std::make_unique<omthx::Receiver>(
            source_, coded ? omthx::PixelFormat::Coded
                            : (nv12 ? omthx::PixelFormat::Nv12 : omthx::PixelFormat::Bgra));
        receiver->requestKeyframe();
        checkedMode = false;
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
      if (coded) {
        omthx::CodedFrame c;
        if (receiver->receiveCoded(c, 20)) {
          if (c.mode != omthx::Mode::Full) {
            coded = false;  // Hx (HEVC/H.264): reopen with the picture receiver
            receiver.reset();
            live = false;
            continue;
          }
          const uint32_t w = static_cast<uint32_t>(c.format.width);
          const uint32_t h = static_cast<uint32_t>(c.format.height);
          if (w == 0 || h == 0 || c.data.empty()) {
            // nothing to pass on
          } else {
            CodedVideoFrame f;
            f.data = c.data;
            f.codec = "vmx";
            f.width = w;
            f.height = h;
            f.ptsUs = c.timestamp / 10;  // omthx timestamps are 100 ns units
            lastVideo = Clock::now();
            if (!live) {
              live = true;
              sink_->onState(SignalState::Live, std::nullopt);
            }
            sink_->onCodedVideo(f);
          }
        }
      } else {
        omthx::VideoFrame v;
        if (receiver->receiveVideo(v, 20)) {
          const uint32_t w = static_cast<uint32_t>(v.format.width);
          const uint32_t h = static_cast<uint32_t>(v.format.height);
          if (!checkedMode) {
            checkedMode = true;
            if (nv12 && receiver->stats().mode == omthx::Mode::Full) {
              // A plain OMT source keeps today's BGRA picture (its 4:2:2 chroma and alpha too).
              nv12 = false;
              receiver.reset();
              live = false;
              continue;
            }
          }
          if (nv12) {
            if (w != 0 && h != 0 && v.nv12Y && v.nv12UV) {
              VideoFrameIn f;
              f.layout = PixelLayout::NV12;
              f.fullRange = true;
              f.bt709 = true;  // OMT-HX decodes full-range BT.709 at every size, not by height
              f.ptsUs = v.timestamp / 10;  // omthx timestamps are 100 ns units
              f.rate = rateFromRational(v.format.fpsN, v.format.fpsD);
              lastVideo = Clock::now();
              if (!live) {
                live = true;
                sink_->onState(SignalState::Live, std::nullopt);
              }
              if (sink_->wantsGpuVideo()) {
                // Copy the decoder's planes before the next receive; the upload maps DYNAMIC
                // textures rather than calling UpdateSubresource, which leaks (see Task 1).
                {
                  auto lock = gpu_.lock();
                  f.tex = uploadNv12Planes(gpu_, pool_, v.nv12Y, v.nv12UV, w, h);
                }
                sink_->onVideo(std::move(f));
              }
            }
          } else if (w != 0 && h != 0 && v.bgra.size() >= static_cast<size_t>(w) * h * 4) {
            VideoFrameIn f;
            f.layout = PixelLayout::BGRA8;
            f.fullRange = true;  // omthx hands back the source's display-referred BGRA
            f.ptsUs = v.timestamp / 10;  // omthx timestamps are 100 ns units
            f.rate = rateFromRational(v.format.fpsN, v.format.fpsD);
            if (sink_->wantsCpuVideo()) {
              CpuVideoFrame cf;
              cf.data = v.bgra.data();
              cf.stride = static_cast<int>(w * 4);
              cf.width = w;
              cf.height = h;
              cf.format = CpuPixel::BGRA;
              cf.fullRange = true;
              cf.ptsUs = f.ptsUs;
              cf.rate = f.rate;
              sink_->onCpuVideo(cf);
            }
            lastVideo = Clock::now();
            if (!live) {
              live = true;
              sink_->onState(SignalState::Live, std::nullopt);
            }
            if (sink_->wantsGpuVideo()) {
              // Map/WRITE_DISCARD on a DYNAMIC texture, not UpdateSubresource on a DEFAULT one: the
              // runtime keeps one private copy of each UpdateSubresource upload until the context is
              // submitted (a Flush made the growth stop), and the engine never presents.
              f.tex = pool_.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                                    D3D11_USAGE_DYNAMIC, D3D11_CPU_ACCESS_WRITE);
              {
                auto lock = gpu_.lock();
                D3D11_MAPPED_SUBRESOURCE m{};
                if (FAILED(gpu_.context()->Map(f.tex->tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                  throw std::runtime_error("cannot fill the picture texture for OMT source \"" + source_ + "\" (" +
                                           std::to_string(w) + "x" + std::to_string(h) + "): Map failed");
                }
                for (uint32_t y = 0; y < h; ++y) {
                  std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                              v.bgra.data() + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4);
                }
                gpu_.context()->Unmap(f.tex->tex.Get(), 0);
              }
              sink_->onVideo(std::move(f));
            }
          }
        }
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