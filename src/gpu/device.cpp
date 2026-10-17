#include "gpu/device.h"

#include <dxgi.h>
#if defined(__has_include)
#if __has_include(<dxgi1_6.h>)
#include <dxgi1_6.h>
#define BC_HAVE_DXGI1_6 1
#endif
#endif
#ifndef BC_HAVE_DXGI1_6
#define BC_HAVE_DXGI1_6 0
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/utf8.h"

using Microsoft::WRL::ComPtr;

namespace bc {
namespace {
std::string envValue(const char* name) {
  char* v = nullptr;
  size_t n = 0;
  _dupenv_s(&v, &n, name);
  std::string s = v ? v : "";
  free(v);
  return s;
}

bool forceWarp() { return envValue("BC_FORCE_WARP") == "1"; }

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string trim(std::string s) {
  const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
  s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
  return s;
}

std::string describe(IDXGIAdapter1* a) {
  DXGI_ADAPTER_DESC1 d{};
  a->GetDesc1(&d);
  return toUtf8(d.Description);
}

bool isSoftware(IDXGIAdapter1* a) {
  DXGI_ADAPTER_DESC1 d{};
  a->GetDesc1(&d);
  return (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
}

// An adapter selection: the adapter to create the device on (null lets D3D11 pick the old
// path), plus whether BC_GPU_ADAPTER asked for it, so the caller can warn when a requested
// adapter turns out unusable.
struct AdapterChoice {
  ComPtr<IDXGIAdapter> adapter;
  bool overrideRequested = false;  // BC_GPU_ADAPTER was set to a non-blank value
  bool overrideMatched = false;    // ...and it selected `adapter`
  std::string request;             // the trimmed BC_GPU_ADAPTER value
  std::string overrideName;        // the matched adapter's description
};

// Preference order: BC_GPU_ADAPTER (a 0-based DXGI index, or when it is not all digits, a
// case-insensitive description substring); else the highest-performance adapter (IDXGIFactory6
// when available); else the first hardware adapter.
AdapterChoice chooseAdapter() {
  AdapterChoice choice;
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return choice;

  // Native DXGI order — a numeric BC_GPU_ADAPTER addresses this list.
  std::vector<ComPtr<IDXGIAdapter1>> adapters;
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> a;
    if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
    if (!a) break;
    adapters.push_back(a);
  }

  const std::string request = trim(envValue("BC_GPU_ADAPTER"));
  if (!request.empty()) {
    choice.overrideRequested = true;
    choice.request = request;
    ComPtr<IDXGIAdapter> wanted;
    std::string matchedName;
    // An all-digits value is always an index; it is never a description substring, because
    // hardware descriptions are full of digits ("… GTX 1650", "… Graphics 630"). An
    // out-of-range index is therefore warned about and ignored, not substring-matched.
    const bool asIndex = std::all_of(request.begin(), request.end(),
                                     [](unsigned char c) { return std::isdigit(c) != 0; });
    if (asIndex) {
      const unsigned long i = std::strtoul(request.c_str(), nullptr, 10);
      if (i < adapters.size()) {
        wanted = adapters[i];
        matchedName = describe(adapters[i].Get());
      } else {
        std::fprintf(stderr, "BC_GPU_ADAPTER=\"%s\" is out of range; using the default choice\n",
                     request.c_str());
      }
    } else {
      const std::string needle = toLower(request);
      for (const auto& a : adapters) {
        if (toLower(describe(a.Get())).find(needle) != std::string::npos) {
          wanted = a;
          matchedName = describe(a.Get());
          break;
        }
      }
      if (!wanted) {
        std::fprintf(stderr, "BC_GPU_ADAPTER=\"%s\" matches no adapter; using the default choice\n",
                     request.c_str());
      }
    }
    if (wanted) {
      choice.adapter = wanted;
      choice.overrideMatched = true;
      choice.overrideName = matchedName;
      return choice;
    }
  }

  // Highest performance, where the OS/IDXGI supports it (Windows 10 1803+).
#if BC_HAVE_DXGI1_6
  ComPtr<IDXGIFactory6> factory6;
  if (SUCCEEDED(factory.As(&factory6))) {
    ComPtr<IDXGIAdapter1> best;
    if (SUCCEEDED(factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                       IID_PPV_ARGS(&best))) &&
        best && !isSoftware(best.Get())) {
      choice.adapter = best;
      return choice;
    }
  }
#endif

  for (const auto& a : adapters) {
    if (!isSoftware(a.Get())) {
      choice.adapter = a;
      return choice;
    }
  }
  return choice;
}
}  // namespace

std::unique_ptr<GpuDevice> GpuDevice::create(Pref pref) {
  if (forceWarp()) pref = Pref::Warp;
  std::unique_ptr<GpuDevice> dev(new GpuDevice());
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  const UINT base = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  auto tryCreate = [&](IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, UINT flags) {
    dev->device_.Reset();
    dev->context_.Reset();
    return D3D11CreateDevice(adapter, type, nullptr, flags, levels, 2, D3D11_SDK_VERSION,
                             &dev->device_, nullptr, &dev->context_);
  };
  HRESULT hr = E_FAIL;
  if (pref != Pref::Warp) {
    // A chosen adapter needs D3D_DRIVER_TYPE_UNKNOWN; if it fails, fall back to the
    // driver-type path (the default hardware adapter, then WARP below).
    const AdapterChoice chosen = chooseAdapter();
    if (chosen.adapter) {
      hr = tryCreate(chosen.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
                     base | D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
      if (FAILED(hr)) hr = tryCreate(chosen.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, base);
      if (FAILED(hr) && chosen.overrideMatched) {
        std::fprintf(stderr,
                     "BC_GPU_ADAPTER=\"%s\" matched \"%s\" but D3D11CreateDevice failed "
                     "(0x%08lX); using the default adapter\n",
                     chosen.request.c_str(), chosen.overrideName.c_str(),
                     static_cast<unsigned long>(hr));
      }
    }
    if (FAILED(hr)) hr = tryCreate(nullptr, D3D_DRIVER_TYPE_HARDWARE, base | D3D11_CREATE_DEVICE_VIDEO_SUPPORT);
    if (FAILED(hr)) hr = tryCreate(nullptr, D3D_DRIVER_TYPE_HARDWARE, base);
  }
  if (FAILED(hr) && pref != Pref::Hardware) hr = tryCreate(nullptr, D3D_DRIVER_TYPE_WARP, base);
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
