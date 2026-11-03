#include "gpu/converter.h"

#include <dxgi.h>

#include <cstring>
#include <stdexcept>

#include "bgra_to_linear.h"
#include "crop_linear.h"
#include "fit_linear.h"
#include "linear_to_bgra8.h"
#include "linear_to_uyvy.h"
#include "uyvy_to_linear.h"
#include "yuv_to_linear.h"

#include "gpu/upload.h"

using Microsoft::WRL::ComPtr;

namespace bc {
namespace {
// Copies a mapped staging resource into a tightly packed bgra image, one row at a time
// (the mapped RowPitch may exceed width * 4).
void copyRows(const D3D11_MAPPED_SUBRESOURCE& m, uint32_t width, uint32_t height, CpuImage& out) {
  out.width = width;
  out.height = height;
  out.bgra.resize(static_cast<size_t>(width) * height * 4);
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(out.bgra.data() + static_cast<size_t>(y) * width * 4,
                static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch, width * 4);
  }
}
}  // namespace

YuvMatrix yuvToRgbMatrix(uint32_t height, bool fullRange, bool forceBt709) {
  const bool bt709 = forceBt709 || height >= 720;
  const float kr = bt709 ? 0.2126f : 0.299f;
  const float kb = bt709 ? 0.0722f : 0.114f;
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

YuvMatrix rgbToYuvMatrix(uint32_t height) {
  const bool bt709 = height >= 720;
  const float kr = bt709 ? 0.2126f : 0.299f;
  const float kb = bt709 ? 0.0722f : 0.114f;
  const float kg = 1.0f - kr - kb;
  const float ys = 219.0f / 255.0f, yo = 16.0f / 255.0f;
  const float cs = 224.0f / 255.0f, co = 128.0f / 255.0f;
  const float uR = -kr / (2.0f * (1.0f - kb)), uG = -kg / (2.0f * (1.0f - kb));
  const float vG = -kg / (2.0f * (1.0f - kr)), vB = -kb / (2.0f * (1.0f - kr));
  YuvMatrix m;
  m.rows[0] = {ys * kr, ys * kg, ys * kb, yo};
  m.rows[1] = {cs * uR, cs * uG, cs * 0.5f, co};
  m.rows[2] = {cs * 0.5f, cs * vG, cs * vB, co};
  return m;
}

Converter::Converter(GpuDevice& gpu, TexturePool& pool) : gpu_(gpu), pool_(pool) {
  auto* dev = gpu_.device();
  if (FAILED(dev->CreateComputeShader(g_yuv_to_linear, sizeof(g_yuv_to_linear), nullptr, &yuvCs_)) ||
      FAILED(dev->CreateComputeShader(g_bgra_to_linear, sizeof(g_bgra_to_linear), nullptr, &bgraCs_)) ||
      FAILED(dev->CreateComputeShader(g_linear_to_bgra8, sizeof(g_linear_to_bgra8), nullptr, &outCs_)) ||
      FAILED(dev->CreateComputeShader(g_linear_to_uyvy, sizeof(g_linear_to_uyvy), nullptr, &uyvyCs_)) ||
      FAILED(dev->CreateComputeShader(g_uyvy_to_linear, sizeof(g_uyvy_to_linear), nullptr, &uyvyInCs_)) ||
      FAILED(dev->CreateComputeShader(g_fit_linear, sizeof(g_fit_linear), nullptr, &fitCs_)) ||
      FAILED(dev->CreateComputeShader(g_crop_linear, sizeof(g_crop_linear), nullptr, &cropCs_))) {
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

TextureRef Converter::toLinear(const GpuTexture& src, PixelLayout layout, bool fullRange, bool forceBt709) {
  auto out = pool_.acquire(src.width, src.height, kLinearFormat,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.outW = p.srcW = src.width;
  p.outH = p.srcH = src.height;
  if (layout == PixelLayout::BGRA8) {
    ID3D11ShaderResourceView* srvs[] = {src.srv.Get()};
    dispatch(bgraCs_.Get(), srvs, 1, out->uav.Get(), p);
  } else if (layout == PixelLayout::UYVY) {
    auto m = yuvToRgbMatrix(src.height, fullRange, forceBt709);
    std::memcpy(p.row0, m.rows[0].data(), sizeof(p.row0));
    std::memcpy(p.row1, m.rows[1].data(), sizeof(p.row1));
    std::memcpy(p.row2, m.rows[2].data(), sizeof(p.row2));
    p.srcW = src.width / 2;  // the packed texture
    ID3D11ShaderResourceView* srvs[] = {src.srv.Get()};
    dispatch(uyvyInCs_.Get(), srvs, 1, out->uav.Get(), p);
  } else {
    auto m = yuvToRgbMatrix(src.height, fullRange, forceBt709);
    std::memcpy(p.row0, m.rows[0].data(), sizeof(p.row0));
    std::memcpy(p.row1, m.rows[1].data(), sizeof(p.row1));
    std::memcpy(p.row2, m.rows[2].data(), sizeof(p.row2));
    ID3D11ShaderResourceView* srvs[] = {src.srvLuma.Get(), src.srvChroma.Get()};
    dispatch(yuvCs_.Get(), srvs, 2, out->uav.Get(), p);
  }
  return out;
}

TextureRef Converter::toBgra8(const GpuTexture& linear, uint32_t outW, uint32_t outH, bool swapRb) {
  auto out = pool_.acquire(outW, outH, DXGI_FORMAT_R8G8B8A8_UNORM,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.row0[0] = swapRb ? 0.0f : 1.0f;
  p.outW = outW;
  p.outH = outH;
  p.srcW = linear.width;
  p.srcH = linear.height;
  ID3D11ShaderResourceView* srvs[] = {linear.srv.Get()};
  dispatch(outCs_.Get(), srvs, 1, out->uav.Get(), p);
  return out;
}

TextureRef Converter::toUyvy(const GpuTexture& linear) {
  if (linear.width % 2 != 0) return nullptr;
  auto out = pool_.acquire(linear.width / 2, linear.height, DXGI_FORMAT_R8G8B8A8_UNORM,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.outW = linear.width / 2;
  p.outH = linear.height;
  p.srcW = linear.width;
  p.srcH = linear.height;
  auto m = rgbToYuvMatrix(linear.height);
  std::memcpy(p.row0, m.rows[0].data(), sizeof(p.row0));
  std::memcpy(p.row1, m.rows[1].data(), sizeof(p.row1));
  std::memcpy(p.row2, m.rows[2].data(), sizeof(p.row2));
  ID3D11ShaderResourceView* srvs[] = {linear.srv.Get()};
  dispatch(uyvyCs_.Get(), srvs, 1, out->uav.Get(), p);
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

TextureRef Converter::crop(const GpuTexture& linear, float x, float y, float w, float h) {
  auto out = pool_.acquire(linear.width, linear.height, kLinearFormat,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.outW = linear.width;
  p.outH = linear.height;
  p.srcW = linear.width;
  p.srcH = linear.height;
  p.rect[0] = x;
  p.rect[1] = y;
  p.rect[2] = w;
  p.rect[3] = h;
  ID3D11ShaderResourceView* srvs[] = {linear.srv.Get()};
  dispatch(cropCs_.Get(), srvs, 1, out->uav.Get(), p);
  return out;
}

CpuImage Converter::readback(const GpuTexture& rgba8, CpuLayout layout) {
  auto staging = pool_.acquire(rgba8.width, rgba8.height, rgba8.format, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
  CpuImage img;
  auto lock = gpu_.lock();
  auto* ctx = gpu_.context();
  ctx->CopyResource(staging->tex.Get(), rgba8.tex.Get());
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(ctx->Map(staging->tex.Get(), 0, D3D11_MAP_READ, 0, &m))) throw std::runtime_error("Map failed");
  copyRows(m, rgba8.width, rgba8.height, img);
  ctx->Unmap(staging->tex.Get(), 0);
  if (layout == CpuLayout::Uyvy) {
    img.width *= 2;  // the packed texture is width / 2 texels wide
    img.layout = CpuLayout::Uyvy;
  }
  return img;
}

void Converter::beginReadback(const TextureRef& staging, const GpuTexture& rgba8) {
  auto lock = gpu_.lock();
  gpu_.context()->CopyResource(staging->tex.Get(), rgba8.tex.Get());
}

bool Converter::tryFinishReadback(const TextureRef& staging, CpuImage& out) {
  auto lock = gpu_.lock();
  auto* ctx = gpu_.context();
  D3D11_MAPPED_SUBRESOURCE m{};
  // The DO_NOT_WAIT flag is a MapFlags argument (OR-ing it into MapType yields E_INVALIDARG).
  const HRESULT hr = ctx->Map(staging->tex.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
  if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;  // not ready: change nothing
  if (FAILED(hr)) throw std::runtime_error("Map failed");
  copyRows(m, staging->width, staging->height, out);
  ctx->Unmap(staging->tex.Get(), 0);
  return true;
}

TextureRef Converter::solid(uint32_t w, uint32_t h, std::array<float, 4> c) {
  auto out = pool_.acquire(w, h, kLinearFormat, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  auto lock = gpu_.lock();
  gpu_.context()->ClearUnorderedAccessViewFloat(out->uav.Get(), c.data());
  return out;
}

TextureRef Converter::uploadBgra8(const uint8_t* data, uint32_t w, uint32_t h, uint32_t stride) {
  auto lock = gpu_.lock();
  return uploadBgra(gpu_, pool_, data, stride, w, h);
}
}  // namespace bc
