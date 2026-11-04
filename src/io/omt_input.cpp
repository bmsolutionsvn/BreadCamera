#include "io/omt_input.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <stdexcept>

#include "omthx/omthx.h"

#include "gpu/upload.h"

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
  // A recorder takes full OMT (VMX) frames as they are. An Hx source reopens as decoded pictures
  // (the same path a Camera sink uses) unless the sink also wants coded HX access units.
  bool codedVmx = sink_->wantsCodedVmx();
  // A recorder that takes HX access units as they arrive keeps the Coded receiver for an Hx
  // source instead of reopening it as pictures.
  const bool codedHx = codedVmx && sink_->wantsCodedHx();
  bool hxMode = false;  // the source on the coded receiver is Hx, not plain OMT
  auto nextKeyRequest = Clock::now() + std::chrono::milliseconds(500);
  // A camera with no enabled stage passes coded OMT frames on itself and asks for pictures only
  // while a preview or another output needs one. That answer changes (an effect comes on), so it is
  // re-read once a second and the receiver is reopened when it does.
  bool codedOmt = sink_->wantsCodedOmt();
  auto nextCodedCheck = Clock::now() + std::chrono::seconds(1);
  // An HX source decodes to full-range BT.709 NV12 for a GPU sink. A sink that wants CPU frames
  // (BreadReplay's recorder tap) keeps the BGRA receiver it had before, so it still gets pictures;
  // one that wants neither keeps the old BGRA receiver too.
  bool nv12 = sink_->wantsGpuVideo() && !sink_->wantsCpuVideo();
  bool checkedMode = false;
  bool live = false;
  bool decoding = false;  // the pass-through decoder has been fed since it was last reset
  std::unique_ptr<omthx::FrameDecoder> decoder;
  auto lastVideo = Clock::now();

  // A decoder's NV12 picture, uploaded exactly as a Receiver's Nv12 picture is.
  const auto uploadNv12 = [this](const omthx::VideoFrame& v) {
    const uint32_t w = static_cast<uint32_t>(v.format.width);
    const uint32_t h = static_cast<uint32_t>(v.format.height);
    if (w == 0 || h == 0 || !v.nv12Y || !v.nv12UV) return;
    VideoFrameIn f;
    f.layout = PixelLayout::NV12;
    f.fullRange = true;
    f.bt709 = true;  // OMT-HX decodes full-range BT.709 at every size, not by height
    f.ptsUs = v.timestamp / 10;  // omthx timestamps are 100 ns units
    f.rate = rateFromRational(v.format.fpsN, v.format.fpsD);
    {
      auto lock = gpu_.lock();
      f.tex = uploadNv12Planes(gpu_, pool_, v.nv12Y, v.nv12UV, w, h);
    }
    sink_->onVideo(std::move(f));
  };

  while (!stopping_) {
    if (!receiver) {
      try {
        receiver = std::make_unique<omthx::Receiver>(
            source_, (codedVmx || codedOmt) ? omthx::PixelFormat::Coded
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
      if (codedVmx) {
        if (codedHx && hxMode && Clock::now() >= nextKeyRequest) {
          receiver->requestKeyframe();  // a recorded HX source: a keyframe at least every 0.5 s
          nextKeyRequest = Clock::now() + std::chrono::milliseconds(500);
        }
        omthx::CodedFrame c;
        if (receiver->receiveCoded(c, 20)) {
          hxMode = c.mode != omthx::Mode::Full;
          if (hxMode && !codedHx) {
            codedVmx = false;  // Hx (HEVC/H.264): reopen with the picture receiver
            receiver.reset();
            live = false;
            continue;
          }
          const uint32_t w = static_cast<uint32_t>(c.format.width);
          const uint32_t h = static_cast<uint32_t>(c.format.height);
          // HX with alpha: the last alphaBytes bytes are the VMX alpha; record the fill only.
          const size_t fillBytes = c.mode == omthx::Mode::Full
                                       ? c.data.size()
                                       : (c.alphaBytes > c.data.size() ? 0 : c.data.size() - c.alphaBytes);
          if (w == 0 || h == 0 || fillBytes == 0) {
            // nothing to pass on
          } else {
            CodedVideoFrame f;
            f.data = std::span<const uint8_t>(c.data.data(), fillBytes);
            f.codec = c.mode == omthx::Mode::Full ? "vmx" : (c.codec == omthx::Codec::H264 ? "h264" : "hevc");
            f.width = w;
            f.height = h;
            f.ptsUs = c.timestamp / 10;  // omthx timestamps are 100 ns units
            f.keyframe = c.keyframe;
            lastVideo = Clock::now();
            if (!live) {
              live = true;
              sink_->onState(SignalState::Live, std::nullopt);
            }
            sink_->onCodedVideo(f);
          }
        }
      } else if (codedOmt) {
        omthx::CodedFrame c;
        if (receiver->receiveCoded(c, 20)) {
          const uint32_t w = static_cast<uint32_t>(c.format.width);
          const uint32_t h = static_cast<uint32_t>(c.format.height);
          if (w != 0 && h != 0 && !c.data.empty()) {
            lastVideo = Clock::now();
            if (!live) {
              live = true;
              sink_->onState(SignalState::Live, std::nullopt);
            }
            auto coded = std::make_shared<const omthx::CodedFrame>(std::move(c));
            if (sink_->onCodedOmt(coded)) receiver->requestKeyframe();  // a downstream receiver asked
            if (sink_->wantsGpuVideo()) {
              if (!decoding) {  // pictures start again: the stream may be mid-GOP
                decoding = true;
                if (!decoder) decoder = std::make_unique<omthx::FrameDecoder>(omthx::PixelFormat::Nv12);
                decoder->reset();
                receiver->requestKeyframe();
              }
              omthx::VideoFrame v;
              if (decoder->decode(*coded, v)) {
                ++framesDecoded_;
                uploadNv12(v);
              }
            } else {
              decoding = false;  // nobody needs a picture: the next one starts from a keyframe
            }
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
              lastVideo = Clock::now();
              if (!live) {
                live = true;
                sink_->onState(SignalState::Live, std::nullopt);
              }
              if (sink_->wantsGpuVideo()) uploadNv12(v);
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

    if (!codedVmx && Clock::now() >= nextCodedCheck) {
      nextCodedCheck = Clock::now() + std::chrono::seconds(1);
      const bool want = sink_->wantsCodedOmt();
      if (want != codedOmt) {
        codedOmt = want;  // the same source, read as coded frames or as pictures
        nv12 = sink_->wantsGpuVideo() && !sink_->wantsCpuVideo();
        receiver.reset();
        live = false;
        continue;
      }
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