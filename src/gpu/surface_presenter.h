#pragma once
#include <windows.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace bc {
class GpuDevice;
class Converter;
struct GpuTexture;

// One preview's DirectComposition surface and flip swap chain (spec §4.1). The presenter owns
// the surface handle; the shell duplicates it into its own process.
class SurfacePresenter {
 public:
  SurfacePresenter(GpuDevice& gpu, Converter& conv, uint32_t w, uint32_t h);  // throws std::runtime_error
  ~SurfacePresenter();
  SurfacePresenter(const SurfacePresenter&) = delete;
  SurfacePresenter& operator=(const SurfacePresenter&) = delete;

  HANDLE handle() const { return surface_; }
  uint32_t width() const { return width_; }   // the size the next present will use
  uint32_t height() const { return height_; }
  void resize(uint32_t w, uint32_t h);        // clamped 16..4096; applied by the next present
  bool present(const GpuTexture& linear);     // false = dropped or broken; never throws, never waits
  bool broken() const { return broken_; }

  static bool available(GpuDevice& gpu, Converter& conv, std::string* reason);  // 16x16 create + present
  IDXGISwapChain1* swapChainForTest() const { return swap_.Get(); }

 private:
  GpuDevice& gpu_;
  Converter& conv_;
  HANDLE surface_ = nullptr;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
  uint32_t width_ = 0, height_ = 0;       // requested (clamped) size
  uint32_t chainW_ = 0, chainH_ = 0;      // size the swap chain currently has
  bool broken_ = false;
};
}  // namespace bc