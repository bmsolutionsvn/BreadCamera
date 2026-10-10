#include "gpu/compositor.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

#include "alpha_blur.h"
#include "alpha_morph.h"
#include "alpha_threshold.h"
#include "bg_blur.h"
#include "bg_composite.h"
#include "bg_downsample.h"
#include "gpu/converter.h"

namespace bc {
MatteRefine refineFrom(const nlohmann::json& p) {
  return {p.value("choke", 0.0f), p.value("feather", 1.0f), p.value("threshold", 0.5f)};
}

std::array<float, 3> hexToLinear(std::string_view hex) {
  auto byte = [&](size_t i) { return std::stoi(std::string(hex.substr(i, 2)), nullptr, 16) / 255.0f; };
  if (hex.size() != 7 || hex[0] != '#') return {0, 0, 0};
  return {std::pow(byte(1), 2.4f), std::pow(byte(3), 2.4f), std::pow(byte(5), 2.4f)};
}

BackgroundSettings backgroundFrom(const nlohmann::json& p) {
  BackgroundSettings s;
  const auto mode = p.value("mode", std::string("none"));
  s.mode = mode == "keyed" ? BackgroundMode::Keyed
         : mode == "blur" ? BackgroundMode::Blur
         : mode == "replace" ? BackgroundMode::Replace
                             : BackgroundMode::None;
  s.blur = p.value("blur", 12.0f);
  s.colorLinear = hexToLinear(p.value("color", std::string("#101418")));
  return s;
}

Compositor::Compositor(GpuDevice& gpu, TexturePool& pool) : gpu_(gpu), pool_(pool) {
  auto* d = gpu_.device();
  auto mk = [&](const BYTE* code, size_t n, Microsoft::WRL::ComPtr<ID3D11ComputeShader>& out) {
    if (FAILED(d->CreateComputeShader(code, n, nullptr, &out))) throw std::runtime_error("CreateComputeShader failed");
  };
  mk(g_alpha_threshold, sizeof(g_alpha_threshold), threshold_);
  mk(g_alpha_morph, sizeof(g_alpha_morph), morph_);
  mk(g_alpha_blur, sizeof(g_alpha_blur), alphaBlur_);
  mk(g_bg_downsample, sizeof(g_bg_downsample), down_);
  mk(g_bg_blur, sizeof(g_bg_blur), bgBlur_);
  mk(g_bg_composite, sizeof(g_bg_composite), composite_);
  D3D11_BUFFER_DESC bd{sizeof(Params), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  if (FAILED(d->CreateBuffer(&bd, nullptr, &cb_))) throw std::runtime_error("CreateBuffer failed");
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(d->CreateSamplerState(&sd, &sampler_))) throw std::runtime_error("CreateSamplerState failed");
}

void Compositor::run(ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs,
                     ID3D11UnorderedAccessView* uav, const Params& p) {
  auto lock = gpu_.lock();
  auto* ctx = gpu_.context();
  D3D11_MAPPED_SUBRESOURCE m{};
  ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
  std::memcpy(m.pData, &p, sizeof(p));
  ctx->Unmap(cb_.Get(), 0);
  ctx->CSSetShader(cs, nullptr, 0);
  ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ctx->CSSetSamplers(0, 1, sampler_.GetAddressOf());
  ctx->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.begin());
  ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx->Dispatch((p.outW + 7) / 8, (p.outH + 7) / 8, 1);
  ID3D11ShaderResourceView* nulls[3] = {};
  ID3D11UnorderedAccessView* nullUav = nullptr;
  ctx->CSSetShaderResources(0, 3, nulls);
  ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
}

TextureRef Compositor::alphaTex(uint32_t w, uint32_t h) {
  return pool_.acquire(w, h, DXGI_FORMAT_R16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
}

TextureRef Compositor::refine(const GpuTexture& raw, uint32_t w, uint32_t h, const MatteRefine& r) {
  Params p{};
  p.outW = w;
  p.outH = h;
  p.srcW = raw.width;
  p.srcH = raw.height;
  auto cur = alphaTex(w, h);
  p.a[0] = r.threshold;
  run(threshold_.Get(), {raw.srv.Get()}, cur->uav.Get(), p);

  const int radius = static_cast<int>(std::lround(std::abs(r.choke)));
  if (radius > 0) {
    const float dilate = r.choke < 0 ? 1.0f : 0.0f;
    for (auto [dx, dy] : {std::pair{1.0f, 0.0f}, std::pair{0.0f, 1.0f}}) {
      auto next = alphaTex(w, h);
      Params m = p;
      m.a[0] = dx;
      m.a[1] = dy;
      m.a[2] = static_cast<float>(radius);
      m.a[3] = dilate;
      run(morph_.Get(), {cur->srv.Get()}, next->uav.Get(), m);
      cur = next;
    }
  }
  if (r.feather > 0.01f) {
    for (auto [dx, dy] : {std::pair{1.0f, 0.0f}, std::pair{0.0f, 1.0f}}) {
      auto next = alphaTex(w, h);
      Params b = p;
      b.a[0] = dx;
      b.a[1] = dy;
      b.a[2] = r.feather;
      run(alphaBlur_.Get(), {cur->srv.Get()}, next->uav.Get(), b);
      cur = next;
    }
  }
  return cur;
}

TextureRef Compositor::apply(const TextureRef& frame, const TextureRef& alpha, const BackgroundSettings& s) {
  if (s.mode == BackgroundMode::None || !alpha) return frame;
  const uint32_t w = frame->width, h = frame->height;
  auto out = pool_.acquire(w, h, Converter::kLinearFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.outW = w;
  p.outH = h;
  p.srcW = w;
  p.srcH = h;
  ID3D11ShaderResourceView* bgSrv = nullptr;
  TextureRef keepAlive;
  switch (s.mode) {
    case BackgroundMode::Keyed:
      p.a[0] = 1;
      break;
    case BackgroundMode::Blur: {
      const uint32_t qw = (w + 3) / 4, qh = (h + 3) / 4;
      auto q = pool_.acquire(qw, qh, Converter::kLinearFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
      Params d{};
      d.outW = qw;
      d.outH = qh;
      d.srcW = w;
      d.srcH = h;
      run(down_.Get(), {frame->srv.Get(), alpha->srv.Get()}, q->uav.Get(), d);
      const float sigma = s.blur * static_cast<float>(h) / 1080.0f / 4.0f;
      for (auto [dx, dy] : {std::pair{1.0f, 0.0f}, std::pair{0.0f, 1.0f}}) {
        auto next = pool_.acquire(qw, qh, Converter::kLinearFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        Params b = d;
        b.a[0] = dx;
        b.a[1] = dy;
        b.a[2] = sigma;
        run(bgBlur_.Get(), {q->srv.Get()}, next->uav.Get(), b);
        q = next;
      }
      keepAlive = q;
      bgSrv = q->srv.Get();
      p.a[0] = 2;
      break;
    }
    case BackgroundMode::Replace:
      if (s.image) {
        keepAlive = s.image;
        bgSrv = s.image->srv.Get();
        p.a[0] = 4;
      } else {
        p.a[0] = 3;
        p.b[0] = s.colorLinear[0];
        p.b[1] = s.colorLinear[1];
        p.b[2] = s.colorLinear[2];
      }
      break;
    case BackgroundMode::None:
      return frame;
  }
  run(composite_.Get(), {frame->srv.Get(), alpha->srv.Get(), bgSrv}, out->uav.Get(), p);
  return out;
}
}  // namespace bc
