#include "io/fullscreen_output.h"

#include <stdexcept>

#include "fullscreen_ps.h"
#include "fullscreen_vs.h"
#include "io/monitors.h"

using Microsoft::WRL::ComPtr;

namespace bc {
namespace {
constexpr wchar_t kClass[] = L"BreadCameraFullscreen";

void registerClassOnce() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    return RegisterClassExW(&wc);
  }();
  if (!atom) throw std::runtime_error("cannot register the fullscreen window class");
}

void check(HRESULT hr, const char* what) {
  if (FAILED(hr)) throw std::runtime_error(std::string(what) + " failed (" + std::to_string(hr) + ")");
}
}  // namespace

FullscreenOutput::FullscreenOutput(GpuDevice& gpu, int monitorIndex) : gpu_(gpu), monitor_(monitorIndex) {}

void FullscreenOutput::open() {
  auto mons = listMonitors();
  if (monitor_ < 0 || monitor_ >= static_cast<int>(mons.size())) {
    throw std::runtime_error("monitor " + std::to_string(monitor_) + " not found (" + std::to_string(mons.size()) + " connected)");
  }
  const auto& m = mons[monitor_];
  registerClassOnce();
  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"BreadCamera", WS_POPUP, m.x, m.y, m.width,
                          m.height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!hwnd_) throw std::runtime_error("cannot create the fullscreen window");
  ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  width_ = static_cast<UINT>(m.width);
  height_ = static_cast<UINT>(m.height);

  auto* dev = gpu_.device();
  ComPtr<IDXGIDevice> dxgiDev;
  check(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)), "QueryInterface(IDXGIDevice)");
  ComPtr<IDXGIAdapter> adapter;
  check(dxgiDev->GetAdapter(&adapter), "GetAdapter");
  ComPtr<IDXGIFactory2> factory;
  check(adapter->GetParent(IID_PPV_ARGS(&factory)), "GetParent(IDXGIFactory2)");
  DXGI_SWAP_CHAIN_DESC1 d{};
  d.Width = width_;
  d.Height = height_;
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  d.SampleDesc = {1, 0};
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.BufferCount = 2;
  d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  check(factory->CreateSwapChainForHwnd(dev, hwnd_, &d, nullptr, nullptr, &swap_), "CreateSwapChainForHwnd");
  factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
  ComPtr<ID3D11Texture2D> back;
  check(swap_->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
  check(dev->CreateRenderTargetView(back.Get(), nullptr, &rtv_), "CreateRenderTargetView");
  check(dev->CreateVertexShader(g_fullscreen_vs, sizeof(g_fullscreen_vs), nullptr, &vs_), "CreateVertexShader");
  check(dev->CreatePixelShader(g_fullscreen_ps, sizeof(g_fullscreen_ps), nullptr, &ps_), "CreatePixelShader");
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  check(dev->CreateSamplerState(&sd, &sampler_), "CreateSamplerState");
}

void FullscreenOutput::deliver(const OutFrame& f) {
  MSG msg;
  while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (!f.linear) return;
  {
    auto lock = gpu_.lock();
    auto* ctx = gpu_.context();
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    ctx->OMSetRenderTargets(1, rtv_.GetAddressOf(), nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->PSSetShaderResources(0, 1, f.linear->srv.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
  }
  // Present outside the context lock: it can wait for vsync.
  check(swap_->Present(1, 0), "Present");
}

void FullscreenOutput::close() noexcept {
  rtv_.Reset();
  swap_.Reset();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}
}  // namespace bc
