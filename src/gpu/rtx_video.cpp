#include "gpu/rtx_video.h"

#include <stdexcept>
#include <string>

#if BC_WITH_RTXVIDEO
#include <windows.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vsr.h>
#endif

namespace bc {
namespace {
// The exact strings the Add effect row shows (spec D7 / D8): one per unmet prerequisite.
constexpr const char* kNoCard = "Needs an NVIDIA RTX card.";
constexpr const char* kNoBuild = "This build has no NVIDIA RTX Video support.";
constexpr const char* kOldDriver = "Needs an NVIDIA RTX card with driver R550 or later.";

#if BC_WITH_RTXVIDEO
// The application id the SDK's programming guide passes for an unidentified build.
constexpr unsigned long long kAppId = 0;

// The NGX model cache: %LOCALAPPDATA%\BreadCamera\ngx (spec D4). NGX creates its own subfolders.
std::wstring dataPath() {
  wchar_t buf[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  std::wstring path = (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring();
  path += L"\\BreadCamera\\ngx";
  return path;
}
#endif
}  // namespace

// NGX state lives behind the pimpl so the class header stays SDK-free (spec §7.2: the file builds
// with BC_WITH_RTXVIDEO=0 too).
struct RtxVideo::Impl {
#if BC_WITH_RTXVIDEO
  NVSDK_NGX_Parameter* params = nullptr;
  NVSDK_NGX_Handle* feature = nullptr;
#endif
};

RtxVideo::RtxVideo(GpuDevice& gpu, TexturePool& pool, Converter& conv)
    : impl_(std::make_unique<Impl>()), gpu_(gpu), pool_(pool), conv_(conv) {}

RtxVideo::~RtxVideo() {
#if BC_WITH_RTXVIDEO
  if (impl_->feature) NVSDK_NGX_D3D11_ReleaseFeature(impl_->feature);
  if (impl_->params) NVSDK_NGX_D3D11_DestroyParameters(impl_->params);
  NVSDK_NGX_D3D11_Shutdown1(gpu_.device());
#endif
}

std::unique_ptr<RtxVideo> RtxVideo::create(GpuDevice& gpu, TexturePool& pool, Converter& conv, std::string& whyNot) {
  // The card check comes first so a WARP or non-NVIDIA device reports the plain reason whatever
  // this build's SDK setting is (spec D7).
  if (!gpu.isNvidia() || gpu.info().warp) {
    whyNot = kNoCard;
    return nullptr;
  }
#if !BC_WITH_RTXVIDEO
  whyNot = kNoBuild;
  return nullptr;
#else
  if (NVSDK_NGX_D3D11_Init(kAppId, dataPath().c_str(), gpu.device()) != NVSDK_NGX_Result_Success) {
    whyNot = kOldDriver;
    return nullptr;
  }
  auto out = std::unique_ptr<RtxVideo>(new RtxVideo(gpu, pool, conv));
  if (NVSDK_NGX_D3D11_GetCapabilityParameters(&out->impl_->params) != NVSDK_NGX_Result_Success) {
    whyNot = kOldDriver;
    return nullptr;
  }
  int vsrAvailable = 0;
  if (out->impl_->params->Get(NVSDK_NGX_Parameter_VSR_Available, &vsrAvailable) != NVSDK_NGX_Result_Success ||
      vsrAvailable == 0) {
    whyNot = kOldDriver;
    return nullptr;
  }
  // One VSR feature, reused by every Clean up and Upscale call on this device.
  auto lock = gpu.lock();
  if (NVSDK_NGX_D3D11_CreateFeature(gpu.context(), NVSDK_NGX_Feature_VSR, out->impl_->params,
                                    &out->impl_->feature) != NVSDK_NGX_Result_Success) {
    whyNot = kOldDriver;
    return nullptr;
  }
  return out;
#endif
}

TextureRef RtxVideo::run(const GpuTexture& linear, uint32_t w, uint32_t h) {
#if !BC_WITH_RTXVIDEO
  (void)linear;
  (void)w;
  (void)h;
  throw std::runtime_error("no NVIDIA RTX Video support in this build");
#else
  // 8-bit BGRA both ways, as VSR expects; the pipeline's linear frames are converted around it.
  auto in = conv_.toBgra8(linear, linear.width, linear.height);
  auto out = pool_.acquire(w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
  auto lock = gpu_.lock();  // the calls below are one immediate-context sequence
  auto* ctx = gpu_.context();
  // RTX Video SDK 1.1: the helper fills the parameter block from the eval struct, so the
  // sub-rects and the quality are the only inputs; Width/Height on the params are not used.
  NVSDK_NGX_D3D11_VSR_Eval_Params eval{};
  eval.pInput = in->tex.Get();
  eval.pOutput = out->tex.Get();
  eval.InputSubrectSize.Width = linear.width;
  eval.InputSubrectSize.Height = linear.height;
  eval.OutputSubrectSize.Width = w;
  eval.OutputSubrectSize.Height = h;
  eval.QualityLevel = NVSDK_NGX_VSR_Quality_High;  // spec D4
  const NVSDK_NGX_Result rc = NGX_D3D11_EVALUATE_VSR_EXT(ctx, impl_->feature, impl_->params, &eval);
  if (rc != NVSDK_NGX_Result_Success) {
    throw std::runtime_error("NGX VSR evaluate failed (" + std::to_string(static_cast<int>(rc)) + ")");
  }
  return conv_.toLinear(*out, PixelLayout::BGRA8, true);
#endif
}
}  // namespace bc