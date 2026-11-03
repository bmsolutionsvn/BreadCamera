#include "io/decklink_output.h"

#include <windows.h>

extern "C" {
#include <libswscale/swscale.h>
}

// See decklink.cpp: WIN32_LEAN_AND_MEAN needs objbase.h for the MIDL header's
// `interface` macro and CoInitializeEx.
#include <objbase.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "DeckLinkAPI_h.h"
#include "io/decklink.h"

namespace bc {
void DeckLinkOutput::open() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  device_ = openDeckLink(index_);
  if (device_->QueryInterface(IID_IDeckLinkOutput, reinterpret_cast<void**>(&output_)) != S_OK) {
    throw std::runtime_error("DeckLink device " + std::to_string(index_) + " has no output");
  }
  if (output_->EnableVideoOutput(static_cast<BMDDisplayMode>(deckLinkModeFor(rate_)), bmdVideoOutputFlagDefault) != S_OK) {
    throw std::runtime_error("the DeckLink output could not start (in use, or the mode is not supported)");
  }
  // Reused UYVY frames. The SDK says of DisplayVideoFrameSync: "after call return, the frame
  // may be released", so a pool frame is free again once that call returns. Keyed outputs never
  // ask for UYVY, so they skip the pool; a card that refuses it keeps the working BGRA path.
  if (!keyed_) {
    for (auto*& f : yuvFrames_) {
      if (output_->CreateVideoFrame(1920, 1080, 1920 * 2, bmdFormat8BitYUV, bmdFrameFlagDefault, &f) != S_OK || !f) {
        for (auto*& g : yuvFrames_) {
          if (g) {
            g->Release();
            g = nullptr;
          }
        }
        yuvOk_ = false;
        break;
      }
    }
  }
  output_->EnableAudioOutput(bmdAudioSampleRate48kHz, bmdAudioSampleType16bitInteger, 2, bmdAudioOutputStreamContinuous);
  if (keyed_) {
    if (device_->QueryInterface(IID_IDeckLinkKeyer, reinterpret_cast<void**>(&keyer_)) != S_OK || keyer_->Enable(TRUE) != S_OK) {
      throw std::runtime_error("this DeckLink card has no external keyer; use an unkeyed output");
    }
    keyer_->SetLevel(255);
  }
}

void DeckLinkOutput::deliver(const OutFrame& f) {
  constexpr int W = 1920, H = 1080;
  if (f.uyvy && yuvFrames_[0] && !f.uyvy->bgra.empty()) {
    // One memcpy into a reused 8-bit UYVY frame; the frame is already 4:2:2, so no alpha loop.
    IDeckLinkMutableVideoFrame* frame = yuvFrames_[yuvNext_];
    yuvNext_ = (yuvNext_ + 1) % 3;
    IDeckLinkVideoBuffer* buf = nullptr;
    void* bytes = nullptr;
    if (frame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&buf)) != S_OK || !buf ||
        buf->StartAccess(bmdBufferAccessWrite) != S_OK) {
      if (buf) buf->Release();
      throw std::runtime_error("cannot access a DeckLink UYVY frame buffer");
    }
    if (buf->GetBytes(&bytes) != S_OK || !bytes) {
      buf->EndAccess(bmdBufferAccessWrite);
      buf->Release();
      throw std::runtime_error("cannot access a DeckLink UYVY frame buffer");
    }
    std::memcpy(bytes, f.uyvy->bgra.data(), static_cast<size_t>(W) * H * 2);
    buf->EndAccess(bmdBufferAccessWrite);
    buf->Release();
    if (output_->DisplayVideoFrameSync(frame) != S_OK)
      throw std::runtime_error("the DeckLink output stopped accepting frames");
  } else if (f.bgra && !f.bgra->bgra.empty()) {
    IDeckLinkMutableVideoFrame* frame = nullptr;
    if (output_->CreateVideoFrame(W, H, W * 4, bmdFormat8BitBGRA, bmdFrameFlagDefault, &frame) != S_OK) {
      throw std::runtime_error("cannot allocate a DeckLink frame");
    }
    IDeckLinkVideoBuffer* buf = nullptr;
    void* bytes = nullptr;
    if (frame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&buf)) != S_OK || !buf ||
        buf->StartAccess(bmdBufferAccessWrite) != S_OK) {
      if (buf) buf->Release();
      frame->Release();
      throw std::runtime_error("cannot access a DeckLink frame buffer");
    }
    if (buf->GetBytes(&bytes) != S_OK || !bytes) {
      buf->EndAccess(bmdBufferAccessWrite);
      buf->Release();
      frame->Release();
      throw std::runtime_error("cannot access a DeckLink frame buffer");
    }
    if (static_cast<int>(f.bgra->width) == W && static_cast<int>(f.bgra->height) == H) {
      std::memcpy(bytes, f.bgra->bgra.data(), static_cast<size_t>(W) * H * 4);
    } else {
      sws_ = sws_getCachedContext(sws_, static_cast<int>(f.bgra->width), static_cast<int>(f.bgra->height), AV_PIX_FMT_BGRA, W, H,
                                  AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!sws_) {
        buf->EndAccess(bmdBufferAccessWrite);
        buf->Release();
        frame->Release();
        throw std::runtime_error("cannot create a pixel converter for the DeckLink frame");
      }
      const uint8_t* src[1] = {f.bgra->bgra.data()};
      const int ss[1] = {static_cast<int>(f.bgra->width * 4)};
      uint8_t* dst[1] = {static_cast<uint8_t*>(bytes)};
      const int ds[1] = {W * 4};
      sws_scale(sws_, src, ss, 0, static_cast<int>(f.bgra->height), dst, ds);
    }
    if (!f.hasAlpha) {  // unkeyed frames must be opaque on the key output
      auto* p = static_cast<uint8_t*>(bytes);
      for (size_t i = 3; i < static_cast<size_t>(W) * H * 4; i += 4) p[i] = 255;
    }
    buf->EndAccess(bmdBufferAccessWrite);
    buf->Release();
    const HRESULT hr = output_->DisplayVideoFrameSync(frame);
    frame->Release();
    if (hr != S_OK) throw std::runtime_error("the DeckLink output stopped accepting frames");
  } else {
    return;
  }
  if (f.audio && !f.audio->empty()) {
    pcm_.resize(f.audio->size());
    for (size_t i = 0; i < pcm_.size(); ++i) pcm_[i] = static_cast<int16_t>(std::clamp((*f.audio)[i], -1.0f, 1.0f) * 32767.0f);
    // Continuous audio output may accept fewer frames than offered; resubmit the remainder so
    // no samples are dropped (which would gap/desync the audio against the video).
    int16_t* p = pcm_.data();
    uint32_t remaining = static_cast<uint32_t>(pcm_.size() / 2);
    while (remaining > 0) {
      uint32_t written = 0;
      if (output_->WriteAudioSamplesSync(p, remaining, &written) != S_OK || written == 0) break;
      remaining -= written;
      p += static_cast<size_t>(written) * 2;  // 2 int16 samples per frame
    }
  }
}

void DeckLinkOutput::close() noexcept {
  for (auto*& f : yuvFrames_) {
    if (f) {
      f->Release();
      f = nullptr;
    }
  }
  if (keyer_) {
    keyer_->Disable();
    keyer_->Release();
    keyer_ = nullptr;
  }
  if (output_) {
    output_->DisableAudioOutput();
    output_->DisableVideoOutput();
    output_->Release();
    output_ = nullptr;
  }
  if (device_) {
    device_->Release();
    device_ = nullptr;
  }
  sws_freeContext(sws_);
  sws_ = nullptr;
}
}  // namespace bc