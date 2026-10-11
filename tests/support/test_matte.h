#pragma once
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine/matte.h"
#include "gpu/device.h"
#include "gpu/texture_pool.h"

namespace bc::test {
// Alpha = 1 inside a centred rectangle (the "person"), 0 elsewhere. Can fail or be slow.
class TestMatte final : public bc::MatteBackend {
 public:
  TestMatte(bc::GpuDevice& gpu, bc::TexturePool& pool) {
    std::vector<uint8_t> px(64 * 36, 0);
    for (int y = 9; y < 27; ++y)
      for (int x = 16; x < 48; ++x) px[y * 64 + x] = 255;
    rect_ = pool.acquire(64, 36, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    auto lock = gpu.lock();
    gpu.context()->UpdateSubresource(rect_->tex.Get(), 0, nullptr, px.data(), 64, 0);
  }
  std::atomic<bool> fail{false};
  std::atomic<int> delayMs{0};
  std::string name() const override { return "test"; }
  void process(std::vector<bc::MatteJob*>& jobs) override {
    if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs.load()));
    if (fail) throw std::runtime_error("test matte failed");
    std::lock_guard l(m);
    batchSizes.push_back(jobs.size());
    for (auto* j : jobs) j->alpha = rect_;
  }
  void forget(const std::string& id) override {
    std::lock_guard l(m);
    forgotten.push_back(id);
  }
  std::mutex m;
  std::vector<size_t> batchSizes;
  std::vector<std::string> forgotten;

 private:
  bc::TextureRef rect_;
};
}  // namespace bc::test
