#pragma once
#include <d3d11_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace bc {
struct GpuInfo {
  std::string adapterName;
  uint32_t vendorId = 0;
  bool warp = false;
};

// The one D3D11 device every stage shares (spec §3.1).
class GpuDevice {
 public:
  enum class Pref { Hardware, Warp, HardwareOrWarp };
  static std::unique_ptr<GpuDevice> create(Pref pref);  // throws std::runtime_error

  ID3D11Device* device() const { return device_.Get(); }
  ID3D11DeviceContext* context() const { return context_.Get(); }
  const GpuInfo& info() const { return info_; }
  bool isNvidia() const { return info_.vendorId == 0x10DE; }
  bool supportsVideoDecode() const { return videoDecode_; }
  // Hold this around any sequence of immediate-context calls (state + dispatch/draw,
  // copy + map). Single calls are already serialized by multithread protection.
  std::unique_lock<std::recursive_mutex> lock() { return std::unique_lock(mutex_); }
  std::recursive_mutex& contextMutex() { return mutex_; }

 private:
  GpuDevice() = default;
  Microsoft::WRL::ComPtr<ID3D11Device> device_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
  GpuInfo info_;
  bool videoDecode_ = false;
  std::recursive_mutex mutex_;
};
}  // namespace bc
