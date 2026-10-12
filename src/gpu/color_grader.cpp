#include "gpu/color_grader.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

#include "color_grade.h"
#include "gpu/converter.h"

namespace bc {
ColorGrade gradeFrom(const nlohmann::json& p) {
  ColorGrade g;
  g.exposure = p.value("exposure", 0.0f);
  g.temperature = p.value("temperature", 0.0f);
  g.tint = p.value("tint", 0.0f);
  g.lift = p.value("lift", 0.0f);
  g.gamma = p.value("gamma", 1.0f);
  g.gain = p.value("gain", 1.0f);
  g.saturation = p.value("saturation", 1.0f);
  return g;
}

bool ColorGrader::isIdentity(const ColorGrade& g) {
  return !g.lut && g.exposure == 0 && g.temperature == 0 && g.tint == 0 && g.lift == 0 && g.gamma == 1 &&
         g.gain == 1 && g.saturation == 1;
}

ColorGrader::ColorGrader(GpuDevice& gpu, TexturePool& pool) : gpu_(gpu), pool_(pool) {
  auto* d = gpu_.device();
  if (FAILED(d->CreateComputeShader(g_color_grade, sizeof(g_color_grade), nullptr, &cs_))) throw std::runtime_error("CreateComputeShader failed");
  D3D11_BUFFER_DESC bd{sizeof(Params), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  if (FAILED(d->CreateBuffer(&bd, nullptr, &cb_))) throw std::runtime_error("CreateBuffer failed");
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(d->CreateSamplerState(&sd, &sampler_))) throw std::runtime_error("CreateSamplerState failed");
  identity_ = upload(Lut3D::identity(2));
}

std::shared_ptr<const GpuLut> ColorGrader::upload(const Lut3D& lut) {
  std::vector<float> rgba(lut.data.size() * 4);
  for (size_t i = 0; i < lut.data.size(); ++i) {
    rgba[i * 4] = lut.data[i].r;
    rgba[i * 4 + 1] = lut.data[i].g;
    rgba[i * 4 + 2] = lut.data[i].b;
    rgba[i * 4 + 3] = 1;
  }
  const UINT n = static_cast<UINT>(lut.size);
  D3D11_TEXTURE3D_DESC d{n, n, n, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_SUBRESOURCE_DATA init{rgba.data(), n * 16, n * n * 16};
  Microsoft::WRL::ComPtr<ID3D11Texture3D> tex;
  if (FAILED(gpu_.device()->CreateTexture3D(&d, &init, &tex))) throw std::runtime_error("CreateTexture3D failed");
  auto out = std::make_shared<GpuLut>();
  out->size = lut.size;
  if (FAILED(gpu_.device()->CreateShaderResourceView(tex.Get(), nullptr, &out->srv))) throw std::runtime_error("LUT SRV failed");
  return out;
}

TextureRef ColorGrader::apply(const TextureRef& frame, const ColorGrade& g) {
  if (isIdentity(g)) return frame;
  auto out = pool_.acquire(frame->width, frame->height, Converter::kLinearFormat,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  Params p{};
  p.a[0] = std::pow(2.0f, g.exposure);
  p.a[1] = 1 + g.temperature / 400.0f;
  p.a[2] = 1 - g.tint / 400.0f;
  p.a[3] = 1 - g.temperature / 400.0f;
  p.b[0] = g.lift;
  p.b[1] = g.gamma;
  p.b[2] = g.gain;
  p.b[3] = g.saturation;
  const auto& lut = g.lut ? g.lut : identity_;
  p.c[0] = g.lut ? 1.0f : 0.0f;
  p.c[1] = static_cast<float>(lut->size);
  p.outW = p.srcW = frame->width;
  p.outH = p.srcH = frame->height;
  auto lock = gpu_.lock();
  auto* ctx = gpu_.context();
  D3D11_MAPPED_SUBRESOURCE m{};
  ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
  std::memcpy(m.pData, &p, sizeof(p));
  ctx->Unmap(cb_.Get(), 0);
  ID3D11ShaderResourceView* srvs[] = {frame->srv.Get(), lut->srv.Get()};
  ID3D11UnorderedAccessView* uav = out->uav.Get();
  ctx->CSSetShader(cs_.Get(), nullptr, 0);
  ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ctx->CSSetSamplers(0, 1, sampler_.GetAddressOf());
  ctx->CSSetShaderResources(0, 2, srvs);
  ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx->Dispatch((p.outW + 7) / 8, (p.outH + 7) / 8, 1);
  ID3D11ShaderResourceView* nulls[2] = {};
  ID3D11UnorderedAccessView* nullUav = nullptr;
  ctx->CSSetShaderResources(0, 2, nulls);
  ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
  return out;
}
}  // namespace bc
