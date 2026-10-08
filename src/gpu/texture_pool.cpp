#include "gpu/texture_pool.h"

#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace bc {
namespace {
constexpr size_t kMaxFreePerKey = 8;

void makeSrv(ID3D11Device* dev, ID3D11Texture2D* tex, DXGI_FORMAT fmt, ComPtr<ID3D11ShaderResourceView>& out) {
  D3D11_SHADER_RESOURCE_VIEW_DESC d{};
  d.Format = fmt;
  d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  d.Texture2D.MipLevels = 1;
  if (FAILED(dev->CreateShaderResourceView(tex, &d, &out))) throw std::runtime_error("CreateShaderResourceView failed");
}
}  // namespace

TextureRef wrapTexture(ID3D11Device* dev, ComPtr<ID3D11Texture2D> tex) {
  D3D11_TEXTURE2D_DESC d{};
  tex->GetDesc(&d);
  auto t = std::make_shared<GpuTexture>();
  t->tex = tex;
  t->format = d.Format;
  t->width = d.Width;
  t->height = d.Height;
  if (d.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
    if (d.Format == DXGI_FORMAT_NV12) {
      makeSrv(dev, tex.Get(), DXGI_FORMAT_R8_UNORM, t->srvLuma);
      makeSrv(dev, tex.Get(), DXGI_FORMAT_R8G8_UNORM, t->srvChroma);
    } else if (d.Format == DXGI_FORMAT_P010) {
      makeSrv(dev, tex.Get(), DXGI_FORMAT_R16_UNORM, t->srvLuma);
      makeSrv(dev, tex.Get(), DXGI_FORMAT_R16G16_UNORM, t->srvChroma);
    } else {
      makeSrv(dev, tex.Get(), d.Format, t->srv);
    }
  }
  if (d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
    if (FAILED(dev->CreateUnorderedAccessView(tex.Get(), nullptr, &t->uav))) throw std::runtime_error("CreateUnorderedAccessView failed");
  }
  if (d.BindFlags & D3D11_BIND_RENDER_TARGET) {
    if (FAILED(dev->CreateRenderTargetView(tex.Get(), nullptr, &t->rtv))) throw std::runtime_error("CreateRenderTargetView failed");
  }
  return t;
}

TexturePool::TexturePool(GpuDevice& gpu) : gpu_(gpu) {}

TextureRef TexturePool::acquire(uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, D3D11_USAGE usage, UINT cpuAccess) {
  if ((fmt == DXGI_FORMAT_NV12 || fmt == DXGI_FORMAT_P010) && ((w | h) & 1)) {
    throw std::invalid_argument("NV12/P010 textures need even width and height");
  }
  Key key{w, h, fmt, bind, usage, cpuAccess};
  std::unique_ptr<GpuTexture> tex;
  {
    std::lock_guard l(state_->m);
    auto& list = state_->free[key];
    if (!list.empty()) {
      tex = std::move(list.back());
      list.pop_back();
    }
  }
  if (!tex) {
    D3D11_TEXTURE2D_DESC d{w, h, 1, 1, fmt, {1, 0}, usage, bind, cpuAccess, 0};
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(gpu_.device()->CreateTexture2D(&d, nullptr, &t))) throw std::runtime_error("CreateTexture2D failed");
    tex = std::make_unique<GpuTexture>(*wrapTexture(gpu_.device(), t));
  }
  std::weak_ptr<State> weak = state_;
  return TextureRef(tex.release(), [weak, key](GpuTexture* t) {
    if (auto s = weak.lock()) {
      std::lock_guard l(s->m);
      auto& list = s->free[key];
      if (list.size() < kMaxFreePerKey) {
        list.emplace_back(t);
        return;
      }
    }
    delete t;
  });
}

size_t TexturePool::freeCount() const {
  std::lock_guard l(state_->m);
  size_t n = 0;
  for (auto& [k, v] : state_->free) n += v.size();
  return n;
}
}  // namespace bc
