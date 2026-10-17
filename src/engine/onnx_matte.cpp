#include "engine/onnx_matte.h"

#include <dxgi1_4.h>
#include <wrl/client.h>

#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>

namespace bc {
namespace {
struct Dims {
  int w, h;
};
Dims dimsFor(MatteQuality q) { return q == MatteQuality::Quality ? Dims{896, 512} : Dims{448, 256}; }

// DirectML must use the same physical GPU as our D3D11 device: find its DXGI index by LUID.
int dxgiIndexOf(GpuDevice& gpu) {
  Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
  gpu.device()->QueryInterface(IID_PPV_ARGS(&dxgiDev));
  Microsoft::WRL::ComPtr<IDXGIAdapter> ours;
  dxgiDev->GetAdapter(&ours);
  DXGI_ADAPTER_DESC od{};
  ours->GetDesc(&od);
  Microsoft::WRL::ComPtr<IDXGIFactory1> f;
  CreateDXGIFactory1(IID_PPV_ARGS(&f));
  Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
  for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 d{};
    a->GetDesc1(&d);
    if (d.AdapterLuid.LowPart == od.AdapterLuid.LowPart && d.AdapterLuid.HighPart == od.AdapterLuid.HighPart) return static_cast<int>(i);
  }
  return 0;
}
}  // namespace

OnnxMatte::OnnxMatte(GpuDevice& gpu, TexturePool& pool, Converter& conv, std::filesystem::path modelPath)
    : gpu_(gpu), pool_(pool), conv_(conv) {
  if (!std::filesystem::exists(modelPath)) throw std::runtime_error("matting model not found: " + modelPath.string());
  env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "breadcamera");
  auto makeSession = [&](bool dml) {
    Ort::SessionOptions so;
    so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (dml) {
      so.DisableMemPattern();
      so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
      Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(so, dxgiIndexOf(gpu_)));
    }
    return std::make_unique<Ort::Session>(*env_, modelPath.c_str(), so);
  };
  try {
    session_ = makeSession(true);
    name_ = "onnx-directml";
  } catch (const Ort::Exception&) {
    try {
      session_ = makeSession(false);
      name_ = "onnx-cpu";
    } catch (const Ort::Exception& e) {
      throw std::runtime_error("matting model " + modelPath.string() + " could not load: " + e.what());
    }
  }
  Ort::AllocatorWithDefaultOptions alloc;
  inName_ = session_->GetInputNameAllocated(0, alloc).get();
  outName_ = session_->GetOutputNameAllocated(0, alloc).get();
  std::vector<uint8_t> ones(16 * 16, 255);
  opaque_ = pool_.acquire(16, 16, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
  {
    auto lock = gpu_.lock();
    gpu_.context()->UpdateSubresource(opaque_->tex.Get(), 0, nullptr, ones.data(), 16, 0);
  }
  thread_ = std::thread([this] { worker(); });
}

OnnxMatte::~OnnxMatte() {
  stop_ = true;
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void OnnxMatte::process(std::vector<MatteJob*>& jobs) {
  std::optional<Cam::WorkerError> failed;
  for (auto* job : jobs) {
    const auto d = dimsFor(job->quality);
    std::lock_guard l(m_);
    auto& c = cams_[job->cameraId];
    if (c.token == 0 || c.quality != job->quality) {
      c.quality = job->quality;
      c.smooth.clear();
      c.token = ++nextToken_;
    }
    // The frame readback is off the tick (spec §5.1 fallback): begin this tick's cheap staging
    // copy, and hand the worker the image of the *previous* copy if the GPU has finished it. The
    // tick never maps the frame — a missing or slow copy simply means the worker waits a tick longer.
    auto tex = conv_.toBgra8(*job->frame, static_cast<uint32_t>(d.w), static_cast<uint32_t>(d.h));
    CpuImage done;
    if (c.slot.tryFinish(conv_, done)) {
      c.pending = std::move(done);
      c.hasPending = true;
    }
    if (!c.slot.pending()) c.slot.begin(conv_, *tex);
    job->alpha = c.latest ? c.latest : opaque_;
    if (!failed && c.workerError && c.workerError->token == c.token) failed = c.workerError;
  }
  cv_.notify_one();
  // A dead session must fail visibly: report the first unresolved worker error so the engine's
  // stage guard bypasses and retries (M2 rules). The jobs above still reach the worker, so a
  // later success clears the error and the matte recovers.
  if (failed) throw std::runtime_error("the matte worker failed: " + failed->message);
}

void OnnxMatte::forget(const std::string& id) {
  std::lock_guard l(m_);
  cams_.erase(id);
}

void OnnxMatte::worker() {
  std::string lastId;
  while (!stop_) {
    std::string id;
    CpuImage img;
    MatteQuality q;
    uint64_t gen;
    {
      std::unique_lock l(m_);
      cv_.wait(l, [&] {
        return stop_ || std::any_of(cams_.begin(), cams_.end(), [](auto& kv) { return kv.second.hasPending; });
      });
      if (stop_) return;
      // Round-robin: the first pending camera after the last one served.
      auto it = cams_.upper_bound(lastId);
      for (size_t n = 0; n < cams_.size(); ++n, ++it) {
        if (it == cams_.end()) it = cams_.begin();
        if (it->second.hasPending) break;
      }
      id = it->first;
      img = std::move(it->second.pending);
      it->second.hasPending = false;
      q = it->second.quality;
      gen = it->second.token;
    }
    lastId = id;
    try {
      infer(id, img, q, gen);
    } catch (const std::exception& e) {
      // Keep the first failure for this token; process() surfaces it and a later success
      // clears it (see infer).
      std::lock_guard l(m_);
      auto it = cams_.find(id);
      if (it != cams_.end() && it->second.token == gen &&
          (!it->second.workerError || it->second.workerError->token != gen)) {
        it->second.workerError = Cam::WorkerError{gen, e.what()};
      }
    }
  }
}

void OnnxMatte::infer(const std::string& id, const CpuImage& img, MatteQuality q, uint64_t token) {
  const auto d = dimsFor(q);
  if (static_cast<int>(img.width) != d.w || static_cast<int>(img.height) != d.h) return;
  const size_t plane = static_cast<size_t>(d.w) * d.h;
  std::vector<float> input(plane * 3);
  for (size_t i = 0; i < plane; ++i) {  // BGRA bytes → RGB planes, (x/255 − 0.5)/0.5
    input[i] = img.bgra[i * 4 + 2] / 127.5f - 1.0f;
    input[plane + i] = img.bgra[i * 4 + 1] / 127.5f - 1.0f;
    input[2 * plane + i] = img.bgra[i * 4] / 127.5f - 1.0f;
  }
  const std::array<int64_t, 4> shape{1, 3, d.h, d.w};
  auto mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(), shape.data(), shape.size());
  const char* in[] = {inName_.c_str()};
  const char* out[] = {outName_.c_str()};
  const auto t0 = std::chrono::steady_clock::now();
  auto result = session_->Run(Ort::RunOptions{nullptr}, in, &tensor, 1, out, 1);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  // Guard the read below: a model with a different output shape must not be indexed as if it
  // held one alpha value per pixel.
  if (result[0].GetTensorTypeAndShapeInfo().GetElementCount() != plane) return;
  const float* alpha = result[0].GetTensorData<float>();

  std::vector<uint8_t> bytes(plane);
  {
    std::lock_guard l(m_);
    auto it = cams_.find(id);
    if (it == cams_.end() || it->second.token != token) return;  // forgotten or quality changed meanwhile
    auto& s = it->second.smooth;
    if (s.size() != plane) s.assign(alpha, alpha + plane);
    for (size_t i = 0; i < plane; ++i) {
      s[i] = 0.6f * std::clamp(alpha[i], 0.0f, 1.0f) + 0.4f * s[i];
      bytes[i] = static_cast<uint8_t>(s[i] * 255.0f + 0.5f);
    }
  }
  auto tex = pool_.acquire(static_cast<uint32_t>(d.w), static_cast<uint32_t>(d.h), DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
  {
    auto lock = gpu_.lock();
    gpu_.context()->UpdateSubresource(tex->tex.Get(), 0, nullptr, bytes.data(), static_cast<UINT>(d.w), 0);
  }
  std::lock_guard l(m_);
  auto it = cams_.find(id);
  if (it != cams_.end() && it->second.token == token) {
    it->second.latest = tex;
    it->second.workerError.reset();  // a good result supersedes any earlier failure
    lastMs_ = ms;                    // only an accepted result updates /engine/matteMs
  }
}
}  // namespace bc
