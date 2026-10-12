#include "engine/maxine_matte.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

#include "nvCVImage.h"
#include "nvVFXGreenScreen.h"
#include "nvVideoEffects.h"

namespace bc {
namespace {
void check(NvCV_Status s, const char* what) {
  if (s != NVCV_SUCCESS) throw std::runtime_error(std::string("Maxine ") + what + ": " + NvCV_GetErrorStringFromCode(s));
}
NvCVImage* nth(NvCVImage* full, NvCVImage* view, unsigned n, unsigned height) {
  NvCVImage_InitView(view, full, 0, static_cast<int>(height * n), full->width, height);
  return view;
}
// NvCVImage_Transfer with a GPU image "might occur asynchronously"; the SDK docs
// synchronize with cudaStreamSynchronize. The VFX SDK ships no sync wrapper and is
// deliberately CUDA-toolkit-link-free (that is why NvVFX_CudaStreamCreate exists),
// so resolve the driver-API twin from nvcuda.dll. CUstream handles are shared
// between the runtime and driver APIs, so this is the same stream object.
void syncStream(CUstream stream) {
  using CuStreamSynchronize = int (*)(CUstream);
  static const CuStreamSynchronize fn = [] {
    HMODULE nvcuda = GetModuleHandleW(L"nvcuda.dll");
    if (!nvcuda) nvcuda = LoadLibraryW(L"nvcuda.dll");
    return nvcuda ? reinterpret_cast<CuStreamSynchronize>(GetProcAddress(nvcuda, "cuStreamSynchronize")) : nullptr;
  }();
  if (!fn) throw std::runtime_error("Maxine: cuStreamSynchronize not found in nvcuda.dll");
  // Checked as int, not CUresult: the SDK headers are not guaranteed to expose
  // the driver-API enum (CUDA_SUCCESS == 0).
  const int r = fn(stream);
  if (r != 0) throw std::runtime_error("Maxine stream synchronize failed: " + std::to_string(r));
}
}  // namespace

struct MaxineMatte::Impl {
  NvVFX_Handle eff = nullptr;
  CUstream stream = nullptr;
  NvCVImage src{}, dst{}, tmp{}, cpuIn{}, cpuOut{};
  NvCVImage inView{}, outView{};
  unsigned maxStreams = 0;
  MatteQuality quality = MatteQuality::Quality;
  std::map<std::string, NvVFX_StateObjectHandle> states;
  std::vector<uint8_t> alphaBytes;
};

std::filesystem::path MaxineMatte::defaultModelDir() {
  char* root = nullptr;
  size_t n = 0;
  _dupenv_s(&root, &n, "VFXSDK_ROOT");
  std::filesystem::path p = root ? std::filesystem::path(root) / "bin" / "models" : std::filesystem::path(BC_MAXINE_ROOT) / "bin" / "models";
  free(root);
  if (std::filesystem::exists(p)) return p;
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return std::filesystem::path(exe).parent_path() / "models";
}

MaxineMatte::MaxineMatte(GpuDevice& gpu, TexturePool& pool, Converter& conv, std::filesystem::path modelDir)
    : gpu_(gpu), pool_(pool), conv_(conv), modelDir_(std::move(modelDir)), impl_(std::make_unique<Impl>()) {
  try {
    build(4, MatteQuality::Quality);
  } catch (...) {
    release();
    throw;
  }
}

MaxineMatte::~MaxineMatte() { release(); }

void MaxineMatte::release() {
  for (auto& [id, st] : impl_->states) NvVFX_DeallocateState(impl_->eff, st);
  impl_->states.clear();
  if (impl_->eff) NvVFX_DestroyEffect(impl_->eff);
  if (impl_->stream) NvVFX_CudaStreamDestroy(impl_->stream);
  impl_->eff = nullptr;
  impl_->stream = nullptr;
  NvCVImage_Dealloc(&impl_->src);
  NvCVImage_Dealloc(&impl_->dst);
  NvCVImage_Dealloc(&impl_->tmp);
}

void MaxineMatte::build(unsigned maxStreams, MatteQuality quality) {
  release();
  auto& m = *impl_;
  // Zeroed up front: a build that fails below (e.g. a growth-triggered one)
  // leaves maxStreams at 0, so the next process() always re-enters build()
  // instead of running the batch on the null effect handle release() left.
  m.maxStreams = 0;
  check(NvVFX_CreateEffect(NVVFX_FX_GREEN_SCREEN, &m.eff), "CreateEffect");
  check(NvCVImage_Alloc(&m.src, kWidth, kHeight * maxStreams, NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "alloc input");
  check(NvCVImage_Alloc(&m.dst, kWidth, kHeight * maxStreams, NVCV_A, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 1), "alloc output");
  const std::string models = modelDir_.string();
  check(NvVFX_SetString(m.eff, NVVFX_MODEL_DIRECTORY, models.c_str()), "model directory");
  // The effect may retain the image pointers past SetImage, so the views must
  // outlive build() — a stack local re-inited for the output would make the
  // input and output views alias and then dangle.
  check(NvVFX_SetImage(m.eff, NVVFX_INPUT_IMAGE, nth(&m.src, &m.inView, 0, kHeight)), "input image");
  check(NvVFX_SetImage(m.eff, NVVFX_OUTPUT_IMAGE, nth(&m.dst, &m.outView, 0, kHeight)), "output image");
  check(NvVFX_CudaStreamCreate(&m.stream), "stream");
  check(NvVFX_SetCudaStream(m.eff, NVVFX_CUDA_STREAM, m.stream), "set stream");
  check(NvVFX_SetU32(m.eff, NVVFX_MODE, quality == MatteQuality::Performance ? 1u : 0u), "mode");
  check(NvVFX_SetU32(m.eff, NVVFX_MAX_NUMBER_STREAMS, maxStreams), "max streams");
  check(NvVFX_SetU32(m.eff, NVVFX_MODEL_BATCH, maxStreams > 1 ? 8u : 1u), "model batch");
  check(NvVFX_Load(m.eff), "load");
  // Assigned last: a failed build leaves the old quality marker (maxStreams
  // was already zeroed above), so the next process() still sees the unmet
  // condition and retries the rebuild.
  m.maxStreams = maxStreams;
  m.quality = quality;
}

void MaxineMatte::forget(const std::string& id) {
  auto it = impl_->states.find(id);
  if (it == impl_->states.end()) return;
  NvVFX_DeallocateState(impl_->eff, it->second);
  impl_->states.erase(it);
}

void MaxineMatte::process(std::vector<MatteJob*>& jobs) {
  if (jobs.empty()) return;
  auto& m = *impl_;
  // ponytail: one quality per batch (the first job's); mixed quality settings follow the first camera.
  // ponytail: capacity is grow-only to avoid rebuild storms when the camera count dips; full
  // quality-flip hysteresis is a follow-up.
  const MatteQuality q = jobs.front()->quality;
  if (jobs.size() > m.maxStreams || q != m.quality)
    build(std::max({m.maxStreams, 4u, static_cast<unsigned>(jobs.size())}), q);

  std::vector<NvVFX_StateObjectHandle> batchStates;
  NvCVImage view{};
  for (unsigned i = 0; i < jobs.size(); ++i) {
    auto& job = *jobs[i];
    auto& st = m.states[job.cameraId];
    if (!st) check(NvVFX_AllocateState(m.eff, &st), "allocate state");
    batchStates.push_back(st);
    auto img = conv_.readback(*conv_.toBgra8(*job.frame, kWidth, kHeight));
    check(NvCVImage_Init(&m.cpuIn, kWidth, kHeight, static_cast<int>(kWidth * 4), img.bgra.data(), NVCV_BGRA, NVCV_U8,
                         NVCV_CHUNKY, NVCV_CPU),
          "wrap input");
    check(NvCVImage_Transfer(&m.cpuIn, nth(&m.src, &view, i, kHeight), 1.0f, m.stream, &m.tmp), "upload");
    // img dies at the end of this iteration, so the HtoD must have completed.
    syncStream(m.stream);
  }
  check(NvVFX_SetU32(m.eff, NVVFX_BATCH_SIZE, static_cast<unsigned>(jobs.size())), "batch size");
  check(NvVFX_SetStateObjectHandleArray(m.eff, NVVFX_STATE, batchStates.data()), "states");
  check(NvVFX_Run(m.eff, 0), "run");  // 0 = synchronous per the SDK docs

  m.alphaBytes.resize(static_cast<size_t>(kWidth) * kHeight);
  for (unsigned i = 0; i < jobs.size(); ++i) {
    check(NvCVImage_Init(&m.cpuOut, kWidth, kHeight, static_cast<int>(kWidth), m.alphaBytes.data(), NVCV_A, NVCV_U8,
                         NVCV_CHUNKY, NVCV_CPU),
          "wrap output");
    check(NvCVImage_Transfer(nth(&m.dst, &view, i, kHeight), &m.cpuOut, 1.0f, m.stream, &m.tmp), "download");
    // alphaBytes is read below (and overwritten by the next download), so the DtoH must have completed.
    syncStream(m.stream);
    auto tex = pool_.acquire(kWidth, kHeight, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    {
      auto lock = gpu_.lock();
      gpu_.context()->UpdateSubresource(tex->tex.Get(), 0, nullptr, m.alphaBytes.data(), kWidth, 0);
    }
    jobs[i]->alpha = tex;
  }
}
}  // namespace bc
