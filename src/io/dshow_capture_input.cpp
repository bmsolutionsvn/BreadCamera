#include "io/dshow_capture_input.h"

#include <windows.h>

#include <dshow.h>  // GUIDs are defined once in dshow_devices.cpp (no initguid here)
#include <dvdmedia.h>  // VIDEOINFOHEADER2 (a FORMAT_VideoInfo2 connection)
#include <wrl/client.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>

#include "core/backoff.h"
#include "io/dshow_devices.h"
#include "io/dshow_format.h"

using Microsoft::WRL::ComPtr;

// The in-box SampleGrabber filter is still shipped, but qedit.h was removed from the SDK, so
// its interface and GUIDs are declared here. Guarded in case a future SDK ships them again.
#if !defined(__ISampleGrabber_INTERFACE_DEFINED__)
struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85")) ISampleGrabberCB : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SampleCB(double sampleTime, IMediaSample* sample) = 0;
  virtual HRESULT STDMETHODCALLTYPE BufferCB(double sampleTime, BYTE* buffer, long len) = 0;
};
struct __declspec(uuid("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F")) ISampleGrabber : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL oneShot) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* type) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* type) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL bufferThem) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* size, long* buffer) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** sample) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* callback, long whichMethod) = 0;
};
// {C1F400A0-3F08-11D3-9F0B-006008039E37}
static const GUID CLSID_SampleGrabber = {0xc1f400a0, 0x3f08, 0x11d3, {0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};
// {C1F400A4-3F08-11D3-9F0B-006008039E37}
static const GUID CLSID_NullRenderer = {0xc1f400a4, 0x3f08, 0x11d3, {0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};
#endif

namespace bc {
namespace {
struct ComInit {
  ComInit() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
  ~ComInit() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }
  HRESULT hr_;
};
struct NeedsRgbFallback {};  // the connected subtype is not convertible; retry forcing RGB32
}  // namespace

// Delivers each sample to the run loop through a condition variable. A SampleGrabber sample is
// valid only during the callback, so it copies the bytes into an owned slot (drop-to-newest):
// the GPU upload then happens on the input's own thread, never on the DirectShow thread.
struct DShowCaptureInput::GrabberCB final : ISampleGrabberCB {
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ISampleGrabberCB)) {
      *ppv = static_cast<ISampleGrabberCB*>(this);
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
  STDMETHODIMP SampleCB(double, IMediaSample* sample) override {
    if (!sample) return S_OK;
    BYTE* p = nullptr;
    if (FAILED(sample->GetPointer(&p)) || !p) return S_OK;
    const long len = sample->GetActualDataLength();
    if (len <= 0) return S_OK;
    {
      std::lock_guard l(m);
      if (done) return S_OK;
      buf.assign(p, p + len);
      pending = true;
    }
    cv.notify_one();
    return S_OK;
  }
  STDMETHODIMP BufferCB(double, BYTE*, long) override { return S_OK; }

  void stop() {
    std::lock_guard l(m);
    done = true;
    buf.clear();
    buf.shrink_to_fit();
    pending = false;
    cv.notify_all();
  }
  void reset() {
    std::lock_guard l(m);
    done = false;
    pending = false;
    buf.clear();
  }

  std::atomic<ULONG> refs_{1};
  std::mutex m;
  std::condition_variable cv;
  std::vector<uint8_t> buf;
  bool pending = false;
  bool done = false;
};

DShowCaptureInput::DShowCaptureInput(GpuDevice& gpu, TexturePool& pool, std::string displayName)
    : gpu_(gpu), pool_(pool), displayName_(std::move(displayName)) {}

DShowCaptureInput::~DShowCaptureInput() { stop(); }

void DShowCaptureInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
  if (!cb_) cb_ = new GrabberCB();
  thread_ = std::thread([this] { run(); });
}

void DShowCaptureInput::stop() {
  stopping_ = true;
  if (cb_) cb_->stop();  // cancels a pending sample and wakes the run loop
  if (thread_.joinable()) thread_.join();
  if (cb_) {
    cb_->Release();
    cb_ = nullptr;
  }
}

void DShowCaptureInput::run() {
  Backoff backoff;
  while (!stopping_) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    try {
      try {
        runOnce(false);
      } catch (const NeedsRgbFallback&) {
        if (stopping_) break;
        // The source negotiated a format we cannot convert (e.g. MJPEG). Force RGB32, which
        // physical webcams stream.
        runOnce(true);
      }
      if (stopping_) break;
      throw std::runtime_error("the device stopped sending frames");
    } catch (const std::exception& e) {
      if (stopping_) break;
      sink_->onState(SignalState::Offline, ErrorInfo{"capture-failed", displayName_ + ": " + e.what()});
    }
    std::unique_lock l(cb_->m);
    cb_->cv.wait_for(l, backoff.next(), [&] { return stopping_.load(); });
  }
  sink_->onState(SignalState::Offline, std::nullopt);
}

void DShowCaptureInput::runOnce(bool forceRgb32) {
  cb_->reset();
  ComInit com;
  ComPtr<IMoniker> moniker;
  moniker.Attach(findDShowMoniker(displayName_));
  if (!moniker) throw std::runtime_error("device not found");

  ComPtr<IBaseFilter> capture;
  if (FAILED(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&capture))))
    throw std::runtime_error("cannot open the device (in use by another app?)");

  ComPtr<IGraphBuilder> graph;
  if (FAILED(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph))))
    throw std::runtime_error("cannot create the filter graph");
  ComPtr<ICaptureGraphBuilder2> builder;
  if (FAILED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder))))
    throw std::runtime_error("cannot create the capture graph builder");
  if (FAILED(builder->SetFiltergraph(graph.Get()))) throw std::runtime_error("cannot set the filter graph");
  if (FAILED(graph->AddFilter(capture.Get(), L"Capture"))) throw std::runtime_error("cannot add the capture filter");

  ComPtr<IBaseFilter> grabberFilter;
  if (FAILED(CoCreateInstance(CLSID_SampleGrabber, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&grabberFilter))))
    throw std::runtime_error("the SampleGrabber filter is not available");
  ComPtr<ISampleGrabber> grabber;
  if (FAILED(grabberFilter.As(&grabber))) throw std::runtime_error("cannot use the SampleGrabber filter");
  // Default: let RenderStream connect the source's native format (OBS Virtual Camera only streams
  // natively). RGB32 is forced only on the fallback attempt.
  if (forceRgb32) {
    AM_MEDIA_TYPE mt{};
    mt.majortype = MEDIATYPE_Video;
    mt.subtype = MEDIASUBTYPE_RGB32;
    mt.formattype = FORMAT_VideoInfo;
    if (FAILED(grabber->SetMediaType(&mt))) throw std::runtime_error("cannot set the RGB32 media type");
  }
  if (FAILED(graph->AddFilter(grabberFilter.Get(), L"Grabber"))) throw std::runtime_error("cannot add the grabber filter");
  // The SampleGrabber is a pass-through (input and output pins), so the graph needs a real
  // renderer downstream of it or nothing flows: capture -> grabber -> null.
  ComPtr<IBaseFilter> nullRenderer;
  if (FAILED(CoCreateInstance(CLSID_NullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&nullRenderer))))
    throw std::runtime_error("the NullRenderer filter is not available");
  if (FAILED(graph->AddFilter(nullRenderer.Get(), L"Null"))) throw std::runtime_error("cannot add the null renderer");

  if (FAILED(builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, capture.Get(), grabberFilter.Get(), nullRenderer.Get())))
    throw std::runtime_error("cannot connect the device's capture pin");

  // The negotiated size and row order come from the connected media type (RGB32/VIDEOINFOHEADER).
  AM_MEDIA_TYPE connected{};
  if (FAILED(grabber->GetConnectedMediaType(&connected))) throw std::runtime_error("cannot read the device media type");
  struct FreeMt {
    AM_MEDIA_TYPE* mt;
    ~FreeMt() {
      if (mt->pbFormat) CoTaskMemFree(mt->pbFormat);
      if (mt->pUnk) mt->pUnk->Release();
    }
  } freeMt{&connected};
  if (!connected.pbFormat) throw std::runtime_error("the device's video format is not supported");
  const BITMAPINFOHEADER* bmi = nullptr;
  if (connected.formattype == FORMAT_VideoInfo)
    bmi = &reinterpret_cast<const VIDEOINFOHEADER*>(connected.pbFormat)->bmiHeader;
  else if (connected.formattype == FORMAT_VideoInfo2)
    bmi = &reinterpret_cast<const VIDEOINFOHEADER2*>(connected.pbFormat)->bmiHeader;
  if (!bmi) throw std::runtime_error("the device's video format is not supported");

  const int hRaw = static_cast<int>(bmi->biHeight);
  width_ = static_cast<int>(bmi->biWidth);
  height_ = std::abs(hRaw);
  if (width_ <= 0 || height_ <= 0) throw std::runtime_error("the device reports no picture size");

  subtype_ = connected.subtype;
  if (!dshowSubtypeSupported(subtype_)) {
    if (!forceRgb32) throw NeedsRgbFallback{};
    throw std::runtime_error("the device's video format is not supported");
  }
  const bool rgb = IsEqualGUID(subtype_, MEDIASUBTYPE_RGB32) || IsEqualGUID(subtype_, MEDIASUBTYPE_RGB24);
  bottomUp_ = rgb && hRaw > 0;  // a positive DIB height is bottom-up for RGB; YUV is top-down
  stride_ = dshowRowBytes(subtype_, width_);
  if (stride_ <= 0) throw std::runtime_error("the device's video format is not supported");
  bt709_ = height_ >= 720;

  if (FAILED(grabber->SetBufferSamples(FALSE))) throw std::runtime_error("cannot configure the SampleGrabber");
  if (FAILED(grabber->SetCallback(cb_, 0))) throw std::runtime_error("cannot set the frame callback");  // 0 = SampleCB (1 = BufferCB)

  // A reference clock makes OBS Virtual Camera deliver one sample then stall; a single live source
  // has no A/V pair to synchronise, so run the graph clock-free.
  ComPtr<IMediaFilter> mediaFilter;
  if (SUCCEEDED(graph.As(&mediaFilter))) mediaFilter->SetSyncSource(nullptr);

  ComPtr<IMediaControl> control;
  if (FAILED(graph.As(&control))) throw std::runtime_error("cannot control the filter graph");
  if (FAILED(control->Run())) throw std::runtime_error("cannot start the device");

  const UINT pitch = static_cast<UINT>(width_) * 4;
  bool live = false;
  bool stalled = false;
  while (!stopping_) {
    std::vector<uint8_t> frame;
    {
      std::unique_lock l(cb_->m);
      // A live device delivers well within this; a timeout means it stalled or was unplugged,
      // which must surface as offline + retry, never a silent freeze on the last frame (spec §4.2).
      if (!cb_->cv.wait_for(l, std::chrono::seconds(2), [&] { return cb_->pending || cb_->done || stopping_.load(); })) {
        stalled = true;
        break;
      }
      if (cb_->done || stopping_) break;
      frame.swap(cb_->buf);
      cb_->pending = false;
    }
    if (frame.size() < static_cast<size_t>(stride_) * height_) continue;  // a short sample; wait for the next
    std::vector<uint8_t> bgra = dshowToBgra8(subtype_, frame.data(), width_, height_, stride_, bottomUp_, bt709_);
    VideoFrameIn f;
    f.layout = PixelLayout::BGRA8;
    f.fullRange = true;
    f.ptsUs = 0;
    f.tex = pool_.acquire(width_, height_, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    {
      auto lock = gpu_.lock();
      gpu_.context()->UpdateSubresource(f.tex->tex.Get(), 0, nullptr, bgra.data(), pitch, 0);
    }
    if (!live) {
      live = true;
      sink_->onState(SignalState::Live, std::nullopt);
    }
    sink_->onVideo(std::move(f));
  }
  control->Stop();
  graph->Abort();  // a filter that will not stop on its own must not hang teardown
  if (stalled) throw std::runtime_error("the device stopped sending frames");
}
}  // namespace bc
