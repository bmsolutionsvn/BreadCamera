#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "engine/matte.h"

namespace bc {
// NVIDIA Maxine AI Green Screen, batched (spec §5.1). NVIDIA RTX only.
// UNTESTED ON HARDWARE until an RTX machine runs tests/maxine_matte_test.cpp (spec §7.2).
class MaxineMatte final : public MatteBackend {
 public:
  static constexpr unsigned kWidth = 1280, kHeight = 720;
  MaxineMatte(GpuDevice& gpu, TexturePool& pool, Converter& conv, std::filesystem::path modelDir);
  ~MaxineMatte() override;
  static std::filesystem::path defaultModelDir();  // %VFXSDK_ROOT%\bin\models, else <exe dir>\models
  std::string name() const override { return "maxine"; }
  void process(std::vector<MatteJob*>& jobs) override;
  void forget(const std::string& cameraId) override;

 private:
  void build(unsigned maxStreams, MatteQuality quality);
  void release();
  GpuDevice& gpu_;
  TexturePool& pool_;
  Converter& conv_;
  std::filesystem::path modelDir_;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace bc
