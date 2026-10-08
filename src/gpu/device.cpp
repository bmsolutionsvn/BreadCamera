#include "gpu/device.h"

#include <dxgi.h>

#include <cstdlib>
#include <stdexcept>

#include "core/utf8.h"

using Microsoft::WRL::ComPtr;

namespace bc {
namespace {
bool forceWarp() {
  char* v = nullptr;
  size_t n = 0;
  _dupenv_s(&v, &n, "BC_FORCE_WARP");
  bool on = v && std::string(v) == "1";
  free(v);
  return on;
}
}  // namespace

std::unique_ptr<GpuDevice> GpuDevice::create(Pref pref) {
  if (forceWarp()) pref = Pref::Warp;
  std::unique_ptr<GpuDevice> dev(new GpuDevice());
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  const UINT base = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  auto tryCreate = [&](D3D_DRIVER_TYPE type, UINT flags) {
    dev->device_.Reset();
    dev->context_.Reset();
    return D3D11CreateDevice(nullptr, type, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                             &dev->device_, nullptr, &dev->context_);
  };
  HRESULT hr = E_FAIL;
  if (pref != Pref::Warp) {
    hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, base | D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
    if (FAILED(hr)) hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, base);
  }
  if (FAILED(hr) && pref != Pref::Hardware) hr = tryCreate(D3D_DRIVER_TYPE_WARP, base);
  if (FAILED(hr)) throw std::runtime_error("D3D11CreateDevice failed: " + std::to_string(hr));

  ComPtr<ID3D10Multithread> mt;
  dev->context_.As(&mt);
  mt->SetMultithreadProtected(TRUE);

  ComPtr<IDXGIDevice> dxgi;
  dev->device_.As(&dxgi);
  ComPtr<IDXGIAdapter> adapter;
  dxgi->GetAdapter(&adapter);
  DXGI_ADAPTER_DESC desc{};
  adapter->GetDesc(&desc);
  dev->info_.adapterName = toUtf8(desc.Description);
  dev->info_.vendorId = desc.VendorId;
  dev->info_.warp = desc.VendorId == 0x1414;  // Microsoft (WARP / Basic Render Driver)

  ComPtr<ID3D11VideoDevice> video;
  dev->videoDecode_ = !dev->info_.warp && SUCCEEDED(dev->device_.As(&video));
  return dev;
}
}  // namespace bc
