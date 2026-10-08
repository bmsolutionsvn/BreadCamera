#include "gpu/converter.h"

#include <cstring>
#include <stdexcept>

#include "bgra_to_linear.h"
#include "fit_linear.h"
#include "linear_to_bgra8.h"
#include "yuv_to_linear.h"

using Microsoft::WRL::ComPtr;

namespace bc {
YuvMatrix yuvToRgbMatrix(uint32_t height, bool fullRange) {
  const float kr = height >= 720 ? 0.2126f : 0.299f;
  const float kb = height >= 720 ? 0.0722f : 0.114f;
  const float kg = 1.0f - kr - kb;
  // Y' = ys*Y + yo ; Pb = cs*Cb + co ; Pr = cs*Cr + co   (inputs are unorm 0..1)
  const float ys = fullRange ? 1.0f : 255.0f / 219.0f;
  const float yo = fullRange ? 0.0f : -16.0f / 219.0f;
  const float cs = fullRange ? 1.0f : 255.0f / 224.0f;
  const float co = fullRange ? -128.0f / 255.0f : -128.0f / 224.0f;
  const float rv = 2.0f * (1.0f - kr);
  const float gu = -2.0f * kb * (1.0f - kb) / kg;
  const float gv = -2.0f * kr * (1.0f - kr) / kg;
  const float bu = 2.0f * (1.0f - kb);
  YuvMatrix m;
  m.rows[0] = {ys, 0.0f, rv * cs, yo + rv * co};
  m.rows[1] = {ys, gu * cs, gv * cs, yo + (gu + gv) * co};
  m.rows[2] = {ys, bu * cs, 0.0f, yo + bu * co};
  return m;
}

Converter::Converter(GpuDevice& gpu, TexturePool& pool) : gpu_(gpu), pool_(pool) {
  auto* dev = gpu_.device();
  if (FAILED(dev->CreateComputeShader(g_yuv_to_linear, sizeof(g_yuv_to_linear), nullptr, &yuvCs_)) ||
      FAILED(dev->CreateComputeShader(g_bgra_to_linear, sizeof(g_bgra_to_linear), nullptr, &bgraCs_)) ||
      FAILED(dev->CreateComputeShader(g_linear_to_bgra8, sizeof(g_linear_to_bgra8), nullptr, &outCs_)) ||
      FAILED(dev->CreateComputeShader(g_fit_linear, sizeof(g_fit_linear), nullptr, &fitCs_))) {
    throw std::runtime_error("CreateComputeShader failed");
  }
  D3D11_BUFFER_DESC bd{sizeof(Params), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_))) throw std::runtime_error("CreateBuffer failed");
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(dev->CreateSamplerState(&sd, &sampler_))) throw std::runtime_error("CreateSamplerState failed");
}

void Converter::dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* const* srvs, UINT nSrv,
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
  ctx->CSSetShaderResources(0, nSrv, srvs);
  ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx->Dispatch((p.outW + 7) / 8, (p.outH + 7) / 8, 1);
  ID3D11ShaderResourceView* nullSrv[2] = {};
  ID3D11UnorderedAccessView* nullUav = nullptr;
  ctx->CSSetShaderResources(0, 2, nullSrv);
  ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
}

TextureRef Converter::toLinear(const GpuTexture& src, PixelLayout layout, bool fullRange) {
  auto out = pool_.acquire(src.width, src.height, kLinearFormat,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.outW = p.srcW = src.width;
  p.outH = p.srcH = src.height;
  if (layout == PixelLayout::BGRA8) {
    ID3D11ShaderResourceView* srvs[] = {src.srv.Get()};
    dispatch(bgraCs_.Get(), srvs, 1, out->uav.Get(), p);
  } else {
    auto m = yuvToRgbMatrix(src.height, fullRange);
    std::memcpy(p.row0, m.rows[0].data(), sizeof(p.row0));
    std::memcpy(p.row1, m.rows[1].data(), sizeof(p.row1));
    std::memcpy(p.row2, m.rows[2].data(), sizeof(p.row2));
    ID3D11ShaderResourceView* srvs[] = {src.srvLuma.Get(), src.srvChroma.Get()};
    dispatch(yuvCs_.Get(), srvs, 2, out->uav.Get(), p);
  }
  return out;
}

TextureRef Converter::toBgra8(const GpuTexture& linear, uint32_t outW, uint32_t outH) {
  auto out = pool_.acquire(outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.outW = outW;
  p.outH = outH;
  p.srcW = linear.width;
  p.srcH = linear.height;
  ID3D11ShaderResourceView* srvs[] = {linear.srv.Get()};
  dispatch(outCs_.Get(), srvs, 1, out->uav.Get(), p);
  return out;
}

TextureRef Converter::fit(const GpuTexture& linear, uint32_t w, uint32_t h) {
  auto out = pool_.acquire(w, h, kLinearFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.outW = w;
  p.outH = h;
  p.srcW = linear.width;
  p.srcH = linear.height;
  ID3D11ShaderResourceView* srvs[] = {linear.srv.Get()};
  dispatch(fitCs_.Get(), srvs, 1, out->uav.Get(), p);
  return out;
}

CpuImage Converter::readback(const GpuTexture& rgba8) {
  auto staging = pool_.acquire(rgba8.width, rgba8.height, rgba8.format, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
  CpuImage img;
  img.width = rgba8.width;
  img.height = rgba8.height;
  img.bgra.resize(static_cast<size_t>(img.width) * img.height * 4);
  auto lock = gpu_.lock();
  auto* ctx = gpu_.context();
  ctx->CopyResource(staging->tex.Get(), rgba8.tex.Get());
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(ctx->Map(staging->tex.Get(), 0, D3D11_MAP_READ, 0, &m))) throw std::runtime_error("Map failed");
  for (uint32_t y = 0; y < img.height; ++y) {
    std::memcpy(img.bgra.data() + static_cast<size_t>(y) * img.width * 4,
                static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, img.width * 4);
  }
  ctx->Unmap(staging->tex.Get(), 0);
  return img;
}

TextureRef Converter::solid(uint32_t w, uint32_t h, std::array<float, 4> c) {
  auto out = pool_.acquire(w, h, kLinearFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  auto lock = gpu_.lock();
  gpu_.context()->ClearUnorderedAccessViewFloat(out->uav.Get(), c.data());
  return out;
}

TextureRef Converter::uploadBgra8(const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride) {
  auto out = pool_.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
  auto lock = gpu_.lock();
  gpu_.context()->UpdateSubresource(out->tex.Get(), 0, nullptr, data, stride, 0);
  return out;
}
}  // namespace bc
