#include "gpu/upscaler.h"

#include <cstring>
#include <stdexcept>

#include "fsr_easu.h"
#include "fsr_rcas.h"
#include "gpu/converter.h"

// FSR 1 constant setup runs on the CPU (FsrEasuCon / FsrRcasCon), so include the vendored headers
// in their CPU mode here; everywhere else they are shader-only. The CPU path is old C code, so mute
// the warnings it would otherwise raise under /W4.
#define A_CPU 1
#pragma warning(push)
#pragma warning(disable : 4244 4245 4305 4310 4505 4804 4805)
#include "gpu/shaders/ffx/ffx_a.h"
#include "gpu/shaders/ffx/ffx_fsr1.h"
#pragma warning(pop)

namespace bc {

Upscaler::Upscaler(GpuDevice& gpu, TexturePool& pool) : gpu_(gpu), pool_(pool) {
  auto* dev = gpu_.device();
  if (FAILED(dev->CreateComputeShader(g_fsr_easu, sizeof(g_fsr_easu), nullptr, &easuCs_)) ||
      FAILED(dev->CreateComputeShader(g_fsr_rcas, sizeof(g_fsr_rcas), nullptr, &rcasCs_))) {
    throw std::runtime_error("CreateComputeShader failed");
  }
  D3D11_BUFFER_DESC bd{static_cast<UINT>((sizeof(EasuParams) + 15) & ~size_t{15}), D3D11_USAGE_DYNAMIC,
                       D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
  if (FAILED(dev->CreateBuffer(&bd, nullptr, &cb_))) throw std::runtime_error("CreateBuffer failed");
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(dev->CreateSamplerState(&sd, &sampler_))) throw std::runtime_error("CreateSamplerState failed");
}

void Upscaler::dispatch(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* srv, ID3D11UnorderedAccessView* uav,
                        const void* params, size_t bytes, uint32_t w, uint32_t h) {
  auto* ctx = gpu_.context();
  D3D11_MAPPED_SUBRESOURCE m{};
  ctx->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
  std::memcpy(m.pData, params, bytes);
  ctx->Unmap(cb_.Get(), 0);
  ctx->CSSetShader(cs, nullptr, 0);
  ctx->CSSetConstantBuffers(0, 1, cb_.GetAddressOf());
  ctx->CSSetSamplers(0, 1, sampler_.GetAddressOf());
  ctx->CSSetShaderResources(0, 1, &srv);
  ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
  ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
  ID3D11ShaderResourceView* nullSrv = nullptr;
  ID3D11UnorderedAccessView* nullUav = nullptr;
  ctx->CSSetShaderResources(0, 1, &nullSrv);
  ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
}

TextureRef Upscaler::apply(const GpuTexture& linear, uint32_t w, uint32_t h) {
  auto easuOut = pool_.acquire(w, h, Converter::kLinearFormat,
                               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  auto out = pool_.acquire(w, h, Converter::kLinearFormat,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);

  // EASU constants: the input is its own viewport and resource (no dynamic resolution).
  // On the CPU the FFX header's vector types are plain arrays (AU4 is GPU-only).
  EasuParams ep{};
  uint32_t con0[4], con1[4], con2[4], con3[4];
  const AF1 inW = static_cast<AF1>(linear.width), inH = static_cast<AF1>(linear.height);
  FsrEasuCon(con0, con1, con2, con3, inW, inH, inW, inH, static_cast<AF1>(w), static_cast<AF1>(h));
  std::memcpy(ep.con0, con0, sizeof(ep.con0));
  std::memcpy(ep.con1, con1, sizeof(ep.con1));
  std::memcpy(ep.con2, con2, sizeof(ep.con2));
  std::memcpy(ep.con3, con3, sizeof(ep.con3));
  ep.outW = w;
  ep.outH = h;

  // RCAS constants: FSR's default sharpness of 0.2 stops.
  RcasParams rp{};
  uint32_t rcon[4];
  FsrRcasCon(rcon, 0.2f);
  std::memcpy(rp.con, rcon, sizeof(rp.con));
  rp.outW = w;
  rp.outH = h;

  auto lock = gpu_.lock();
  dispatch(easuCs_.Get(), linear.srv.Get(), easuOut->uav.Get(), &ep, sizeof(ep), w, h);
  dispatch(rcasCs_.Get(), easuOut->srv.Get(), out->uav.Get(), &rp, sizeof(rp), w, h);
  return out;
}
}  // namespace bc