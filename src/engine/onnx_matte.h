#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/matte.h"

namespace Ort {
struct Env;
struct Session;
}  // namespace Ort

namespace bc {
// MODNet portrait matting on ONNX Runtime + DirectML, run off the tick thread (spec §5.1 fallback).
class OnnxMatte final : public MatteBackend {
 public:
  OnnxMatte(GpuDevice& gpu, TexturePool& pool, Converter& conv, std::filesystem::path modelPath);
  ~OnnxMatte() override;
  std::string name() const override { return name_; }
  void process(std::vector<MatteJob*>& jobs) override;
  void forget(const std::string& cameraId) override;
  double lastInferenceMs() const override { return lastMs_; }

 private:
  struct Cam {
    CpuImage pending;
    bool hasPending = false;
    MatteQuality quality = MatteQuality::Quality;
    std::vector<float> smooth;  // previous alpha at the current quality's size
    TextureRef latest;
    uint64_t generation = 0;    // bumps on forget/quality change; stale results are dropped
  };
  void worker();
  void infer(const std::string& id, const CpuImage& img, MatteQuality q, uint64_t generation);

  GpuDevice& gpu_;
  TexturePool& pool_;
  Converter& conv_;
  std::string name_;
  std::unique_ptr<Ort::Env> env_;
  std::unique_ptr<Ort::Session> session_;
  std::string inName_, outName_;
  TextureRef opaque_;
  std::mutex m_;
  std::condition_variable cv_;
  std::map<std::string, Cam> cams_;
  std::atomic<bool> stop_{false};
  std::atomic<double> lastMs_{0};
  std::thread thread_;
};
}  // namespace bc
