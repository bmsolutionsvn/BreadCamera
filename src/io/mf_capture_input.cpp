#include "io/mf_capture_input.h"

#include <windows.h>

#include "io/dshow_devices.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stdexcept>

#include "core/backoff.h"
#include "core/utf8.h"

using Microsoft::WRL::ComPtr;

namespace bc {
namespace {
void ensureMf() {
  static std::once_flag once;
  std::call_once(once, [] { MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET); });
}

struct ComInit {
  ComInit() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComInit() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }
  HRESULT hr_;
};

std::vector<ComPtr<IMFActivate>> enumerate() {
  ComPtr<IMFAttributes> attrs;
  MFCreateAttributes(&attrs, 1);
  attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate** raw = nullptr;
  UINT32 n = 0;
  std::vector<ComPtr<IMFActivate>> out;
  if (FAILED(MFEnumDeviceSources(attrs.Get(), &raw, &n))) return out;
  for (UINT32 i = 0; i < n; ++i) {
    out.emplace_back(raw[i]);
    raw[i]->Release();
  }
  CoTaskMemFree(raw);
  return out;
}

std::string str(IMFActivate* a, const GUID& key) {
  WCHAR* s = nullptr;
  UINT32 len = 0;
  if (FAILED(a->GetAllocatedString(key, &s, &len))) return {};
  std::string out = toUtf8(s);
  CoTaskMemFree(s);
  return out;
}
}  // namespace

// Delivers each sample to the run loop through a condition variable. Self-contained (no
// back-pointer to MfCaptureInput): Media Foundation holds its own reference, so a callback
// arriving as the input is torn down is still safe — it only touches its own members.
struct MfCaptureInput::MfReaderCallback final : IMFSourceReaderCallback {
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IMFSourceReaderCallback) || riid == __uuidof(IUnknown)) {
      *ppv = static_cast<IMFSourceReaderCallback*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
  }
  STDMETHODIMP OnReadSample(HRESULT st, DWORD /*stream*/, DWORD f, LONGLONG stamp, IMFSample* s) override {
    if (s) s->AddRef();  // hold it until the run loop consumes it
    {
      std::lock_guard l(m);
      if (done) {
        if (s) s->Release();
        return S_OK;
      }
      if (sample) sample->Release();
      status = st;
      flags = f;
      ts = stamp;
      sample = s;
      pending = true;
    }
    cv.notify_one();
    return S_OK;
  }
  STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
  STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

  void stop() {
    std::lock_guard l(m);
    done = true;
    if (sample) sample->Release();
    sample = nullptr;
    pending = false;
    cv.notify_all();
  }

  // Clear state left by the previous reader before an open attempt retries.
  void reset() {
    std::lock_guard l(m);
    done = false;
    pending = false;
    if (sample) sample->Release();
    sample = nullptr;
    status = S_OK;
    flags = 0;
    ts = 0;
  }

  std::atomic<ULONG> refs_{1};
  std::mutex m;
  std::condition_variable cv;
  IMFSample* sample = nullptr;
  LONGLONG ts = 0;
  DWORD flags = 0;
  HRESULT status = S_OK;
  bool pending = false;
  bool done = false;
};

std::vector<CaptureDevice> listCaptureDevices() {
  ensureMf();
  ComInit com;
  std::vector<CaptureDevice> mf;
  for (auto& a : enumerate()) {
    mf.push_back({str(a.Get(), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME), str(a.Get(), MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK)});
  }
  // Media Foundation misses DirectShow-only virtual cameras (OBS Virtual Camera); merge them in.
  return mergeCaptureDevices(mf, listDShowDevices());
}

void MfCaptureInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
  if (!cb_) cb_ = new MfReaderCallback();
  thread_ = std::thread([this] { run(); });
}

void MfCaptureInput::stop() {
  stopping_ = true;
  if (cb_) cb_->stop();  // cancels any pending sample and wakes the run loop
  if (thread_.joinable()) thread_.join();
  if (cb_) {
    cb_->Release();
    cb_ = nullptr;
  }
}

void MfCaptureInput::run() {
  ensureMf();
  ComInit com;
  Backoff backoff;
  while (!stopping_) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    try {
      runOnce();
      if (stopping_) break;
      throw std::runtime_error("the device stopped sending frames");
    } catch (const std::exception& e) {
      if (stopping_) break;
      sink_->onState(SignalState::Offline, ErrorInfo{"capture-failed", device_ + ": " + e.what()});
    }
    // Wait on the callback's condition variable so stop() (cb_->stop()) wakes this too, not
    // just the frame wait — otherwise stop() could block for the whole backoff interval.
    std::unique_lock l(cb_->m);
    cb_->cv.wait_for(l, backoff.next(), [&] { return stopping_.load(); });
  }
  sink_->onState(SignalState::Offline, std::nullopt);
}

void MfCaptureInput::runOnce() {
  cb_->reset();
  ComPtr<IMFActivate> pick;
  for (auto& a : enumerate()) {
    const auto id = str(a.Get(), MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
    const auto name = str(a.Get(), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
    if (id == device_ || (!pick && name.find(device_) != std::string::npos)) pick = a;
  }
  if (!pick) throw std::runtime_error("device not found");
  ComPtr<IMFMediaSource> source;
  if (FAILED(pick->ActivateObject(IID_PPV_ARGS(&source)))) throw std::runtime_error("cannot open the device (in use by another app?)");
  struct Shutdown {
    IMFMediaSource* s;
    ~Shutdown() { s->Shutdown(); }
  } shutdown{source.Get()};
  ComPtr<IMFAttributes> ra;
  MFCreateAttributes(&ra, 2);
  ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  ra->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, cb_);
  ComPtr<IMFSourceReader> reader;
  if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), ra.Get(), &reader))) throw std::runtime_error("cannot create a source reader");

  // Pick a native mode: the requested size/fps, else the largest at >= 25 fps.
  constexpr DWORD kVideo = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  ComPtr<IMFMediaType> best;
  UINT64 bestScore = 0;
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> t;
    if (FAILED(reader->GetNativeMediaType(kVideo, i, &t))) break;
    UINT32 w = 0, h = 0, num = 0, den = 1;
    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
    const double fps = den ? static_cast<double>(num) / den : 0;
    UINT64 score = static_cast<UINT64>(w) * h * (fps >= 25 ? 2 : 1);
    if (width_ && height_) score = (static_cast<int>(w) == width_ && static_cast<int>(h) == height_) ? 1'000'000'000ULL + static_cast<UINT64>(fps) : 0;
    if (fps_ && std::abs(fps - fps_) > 1) score /= 4;
    if (score > bestScore) {
      bestScore = score;
      best = t;
    }
  }
  if (!best) throw std::runtime_error("the device offers no video mode");
  reader->SetCurrentMediaType(kVideo, nullptr, best.Get());
  ComPtr<IMFMediaType> rgb;
  if (FAILED(MFCreateMediaType(&rgb))) throw std::runtime_error("cannot build the RGB media type");
  rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  if (FAILED(reader->SetCurrentMediaType(kVideo, nullptr, rgb.Get()))) throw std::runtime_error("cannot convert the device format to RGB");
  ComPtr<IMFMediaType> actual;
  if (FAILED(reader->GetCurrentMediaType(kVideo, &actual))) throw std::runtime_error("cannot read the device media type");
  UINT32 w = 0, h = 0;
  MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h);
  LONG stride = static_cast<LONG>(w * 4);
  UINT32 s = 0;
  if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) stride = static_cast<LONG>(static_cast<INT32>(s));

  bool live = false;
  reader->ReadSample(kVideo, 0, nullptr, nullptr, nullptr, nullptr);  // async: the callback answers
  while (!stopping_) {
    IMFSample* raw = nullptr;
    LONGLONG ts = 0;
    DWORD flags = 0;
    HRESULT status = S_OK;
    {
      std::unique_lock l(cb_->m);
      cb_->cv.wait(l, [&] { return cb_->pending || cb_->done || stopping_.load(); });
      if (cb_->done || stopping_) break;
      raw = cb_->sample;
      cb_->sample = nullptr;
      cb_->pending = false;
      ts = cb_->ts;
      flags = cb_->flags;
      status = cb_->status;
    }
    if (FAILED(status) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
      if (raw) raw->Release();
      throw std::runtime_error("the device was disconnected");
    }
    if (!raw) {  // MF_SOURCE_READERF_STREAMTICK (no frame yet); ask again
      if (!stopping_) reader->ReadSample(kVideo, 0, nullptr, nullptr, nullptr, nullptr);
      continue;
    }
    ComPtr<IMFSample> sample;
    sample.Attach(raw);
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(&buf))) {
      if (!stopping_) reader->ReadSample(kVideo, 0, nullptr, nullptr, nullptr, nullptr);
      continue;
    }
    BYTE* data = nullptr;
    DWORD len = 0;
    if (FAILED(buf->Lock(&data, nullptr, &len))) {
      if (!stopping_) reader->ReadSample(kVideo, 0, nullptr, nullptr, nullptr, nullptr);
      continue;
    }
    VideoFrameIn f;
    f.layout = PixelLayout::BGRA8;
    f.fullRange = true;
    f.ptsUs = ts / 10;
    f.tex = pool_.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    const UINT pitch = static_cast<UINT>(std::abs(stride));
    const BYTE* src = data;
    if (stride < 0) {  // bottom-up RGB32: row y of the picture is at data + (h-1-y)*pitch
      flip_.resize(static_cast<size_t>(pitch) * h);
      for (UINT32 y = 0; y < h; ++y) std::memcpy(&flip_[static_cast<size_t>(y) * pitch], data + static_cast<size_t>(h - 1 - y) * pitch, pitch);
      src = flip_.data();
    }
    {
      auto lock = gpu_.lock();
      gpu_.context()->UpdateSubresource(f.tex->tex.Get(), 0, nullptr, src, pitch, 0);
    }
    buf->Unlock();
    if (!live) {
      live = true;
      sink_->onState(SignalState::Live, std::nullopt);
    }
    sink_->onVideo(std::move(f));
    if (!stopping_) reader->ReadSample(kVideo, 0, nullptr, nullptr, nullptr, nullptr);
  }
}
}  // namespace bc
