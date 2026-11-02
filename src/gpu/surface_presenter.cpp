#include "gpu/surface_presenter.h"

#include <dcomp.h>

#include <algorithm>
#include <stdexcept>

#include "gpu/converter.h"
#include "gpu/device.h"

using Microsoft::WRL::ComPtr;

namespace bc {
namespace {
uint32_t clampSize(uint32_t v) { return std::clamp(v, 16u, 4096u); }
}  // namespace

SurfacePresenter::SurfacePresenter(GpuDevice& gpu, Converter& conv, uint32_t w, uint32_t h)
    : gpu_(gpu), conv_(conv), width_(clampSize(w)), height_(clampSize(h)) {
  try {
    HRESULT hr = DCompositionCreateSurfaceHandle(COMPOSITIONOBJECT_ALL_ACCESS, nullptr, &surface_);
    if (FAILED(hr)) throw std::runtime_error("DCompositionCreateSurfaceHandle failed");

    ComPtr<IDXGIFactoryMedia> factory;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) throw std::runtime_error("CreateDXGIFactory1(IDXGIFactoryMedia) failed");

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = factory->CreateSwapChainForCompositionSurfaceHandle(gpu_.device(), surface_, &desc, nullptr, &swap_);
    if (FAILED(hr)) throw std::runtime_error("CreateSwapChainForCompositionSurfaceHandle failed");
    chainW_ = width_;
    chainH_ = height_;
  } catch (...) {
    swap_.Reset();
    if (surface_) CloseHandle(surface_);
    surface_ = nullptr;
    throw;
  }
}

SurfacePresenter::~SurfacePresenter() {
  swap_.Reset();  // let go of the buffers before the surface goes away
  if (surface_) CloseHandle(surface_);
}

void SurfacePresenter::resize(uint32_t w, uint32_t h) {
  width_ = clampSize(w);
  height_ = clampSize(h);
}

bool SurfacePresenter::present(const GpuTexture& linear) {
  if (broken_) return false;
  try {
    auto lock = gpu_.lock();
    // No back-buffer reference is held between presents, so ResizeBuffers is legal here.
    if (width_ != chainW_ || height_ != chainH_) {
      const HRESULT hr = swap_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, 0);
      if (FAILED(hr)) {
        broken_ = true;
        return false;
      }
      chainW_ = width_;
      chainH_ = height_;
    }

    // swapRb = false keeps true RGBA, matching the R8G8B8A8 back buffer for CopyResource.
    auto fitted = conv_.fit(linear, width_, height_);
    auto rgba = conv_.toBgra8(*fitted, width_, height_, /*swapRb=*/false);

    ComPtr<ID3D11Texture2D> back;
    HRESULT hr = swap_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (SUCCEEDED(hr)) {
      gpu_.context()->CopyResource(back.Get(), rgba->tex.Get());
      hr = swap_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    }
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    if (FAILED(hr)) {
      broken_ = true;
      return false;
    }
    return true;
  } catch (...) {
    broken_ = true;
    return false;
  }
}

bool SurfacePresenter::available(GpuDevice& gpu, Converter& conv, std::string* reason) {
  if (reason) reason->clear();
  try {
    SurfacePresenter p(gpu, conv, 16, 16);
    auto solid = conv.solid(16, 16, {0, 0, 0, 1});
    if (p.present(*solid)) return true;
  } catch (const std::exception& e) {
    if (reason) *reason = e.what();
    return false;
  } catch (...) {
  }
  if (reason) *reason = "the composition swap chain did not present";
  return false;
}
}  // namespace bc