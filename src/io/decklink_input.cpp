#include "io/decklink_input.h"

#include <windows.h>

// See decklink.cpp: WIN32_LEAN_AND_MEAN needs objbase.h for the MIDL header's
// `interface` macro and CoInitializeEx.
#include <objbase.h>

#include <chrono>

#include "DeckLinkAPI_h.h"
#include "core/backoff.h"
#include "io/decklink.h"
#include "io/uyvy.h"

namespace bc {
namespace {
class Callback final : public IDeckLinkInputCallback {
 public:
  explicit Callback(DeckLinkInput* owner) : owner_(owner) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode* mode,
                                                    BMDDetectedVideoInputFormatFlags flags) override {
    owner_->onFormatChanged(mode->GetDisplayMode(), (flags & bmdDetectedVideoInputRGB444) != 0);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* frame, IDeckLinkAudioInputPacket* audio) override {
    const int32_t* samples = nullptr;
    int count = 0;
    if (audio) {
      void* p = nullptr;
      audio->GetBytes(&p);
      samples = static_cast<const int32_t*>(p);
      count = static_cast<int>(audio->GetSampleFrameCount());
    }
    if (!frame) {
      owner_->onFrame(nullptr, 0, 0, 0, true, samples, count);
      return S_OK;
    }
    const bool noSignal = (frame->GetFlags() & bmdFrameHasNoInputSource) != 0;
    IDeckLinkVideoBuffer* buf = nullptr;
    void* bytes = nullptr;
    if (frame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&buf)) == S_OK &&
        buf->StartAccess(bmdBufferAccessRead) == S_OK) {
      buf->GetBytes(&bytes);
      owner_->onFrame(static_cast<const uint8_t*>(bytes), static_cast<int>(frame->GetWidth()), static_cast<int>(frame->GetHeight()),
                      static_cast<int>(frame->GetRowBytes()), noSignal, samples, count);
      buf->EndAccess(bmdBufferAccessRead);
    }
    if (buf) buf->Release();
    return S_OK;
  }

 private:
  DeckLinkInput* owner_;
  std::atomic<ULONG> refs_{1};
};
}  // namespace

void DeckLinkInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
  thread_ = std::thread([this] { run(); });
}

void DeckLinkInput::stop() {
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
}

void DeckLinkInput::onFormatChanged(uint32_t mode, bool rgb) {
  // Snapshot the input under the lock (holding a reference), then drive it outside the lock.
  // run() detaches and releases input_ outside the lock too, so neither side can block the
  // other: StopStreams() waits for this callback, which must not wait for m_ in return.
  IDeckLinkInput* input = nullptr;
  {
    std::lock_guard l(m_);
    input = input_;
    if (input) input->AddRef();
  }
  if (!input) return;
  mode_ = mode;
  input->PauseStreams();
  input->EnableVideoInput(static_cast<BMDDisplayMode>(mode), rgb ? bmdFormat8BitBGRA : bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection);
  input->FlushStreams();
  input->StartStreams();
  input->Release();
}

void DeckLinkInput::onFrame(const uint8_t* bytes, int w, int h, int rowBytes, bool noSignal, const int32_t* audio, int audioFrames) {
  if (audio && audioFrames > 0) {
    AudioBlock blk;
    blk.samples.resize(static_cast<size_t>(audioFrames) * 2);
    for (size_t i = 0; i < blk.samples.size(); ++i) blk.samples[i] = static_cast<float>(audio[i]) / 2147483648.0f;
    sink_->onAudio(std::move(blk));
  }
  if (!bytes || noSignal) {
    if (live_) {
      live_ = false;
      sink_->onState(SignalState::Offline, ErrorInfo{"decklink-no-signal", "no signal on the DeckLink input"});
    }
    return;
  }
  // rowBytes == w*4 means the card delivered BGRA (RGB 4:4:4 detected); else UYVY.
  std::vector<uint8_t> bgra = rowBytes >= w * 4 ? std::vector<uint8_t>() : uyvyToBgra(bytes, w, h, rowBytes, h >= 720);
  VideoFrameIn f;
  f.layout = PixelLayout::BGRA8;
  f.fullRange = true;
  f.rate = deckLinkRateFor(mode_.load());
  f.tex = pool_.acquire(static_cast<uint32_t>(w), static_cast<uint32_t>(h), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
  {
    auto lock = gpu_.lock();
    gpu_.context()->UpdateSubresource(f.tex->tex.Get(), 0, nullptr, bgra.empty() ? bytes : bgra.data(),
                                      static_cast<UINT>(bgra.empty() ? rowBytes : w * 4), 0);
  }
  if (!live_) {
    live_ = true;
    sink_->onState(SignalState::Live, std::nullopt);
  }
  sink_->onVideo(std::move(f));
}

void DeckLinkInput::run() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Backoff backoff;
  while (!stopping_) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    Callback* cb = nullptr;
    try {
      device_ = openDeckLink(index_);
      if (device_->QueryInterface(IID_IDeckLinkInput, reinterpret_cast<void**>(&input_)) != S_OK) {
        throw std::runtime_error("DeckLink device " + std::to_string(index_) + " has no input");
      }
      cb = new Callback(this);
      input_->SetCallback(cb);
      mode_ = bmdModeHD1080p6000;
      if (input_->EnableVideoInput(bmdModeHD1080p6000, bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection) != S_OK ||
          input_->EnableAudioInput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger, 2) != S_OK ||
          input_->StartStreams() != S_OK) {
        throw std::runtime_error("the DeckLink input could not start (in use by another app?)");
      }
      backoff.reset();
      while (!stopping_) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } catch (const std::exception& e) {
      sink_->onState(SignalState::Offline, ErrorInfo{"decklink-failed", e.what()});
    }
    // Detach under the lock, then stop/release outside it. StopStreams() can block until the
    // in-flight SDK callback returns, and that callback (onFormatChanged) takes m_ while the
    // input is still attached — releasing here instead of inside the lock avoids the deadlock.
    IDeckLinkInput* input = nullptr;
    {
      std::lock_guard l(m_);
      input = input_;
      input_ = nullptr;
    }
    if (input) {
      input->StopStreams();
      input->SetCallback(nullptr);
      input->Release();
    }
    if (cb) cb->Release();
    if (device_) {
      device_->Release();
      device_ = nullptr;
    }
    for (auto d = backoff.next(); !stopping_ && d.count() > 0; d -= std::chrono::milliseconds(100)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  sink_->onState(SignalState::Offline, std::nullopt);
  CoUninitialize();
}
}  // namespace bc