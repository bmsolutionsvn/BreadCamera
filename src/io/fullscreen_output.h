#pragma once
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include "gpu/device.h"
#include "io/queued_output.h"

namespace bc {
// Borderless topmost window covering one monitor; presents each tick frame (spec §4.3).
// The window lives on the output's own worker thread.
// ponytail: stretches to the monitor (no letterbox); add aspect handling if a non-16:9
// screen shows up.
class FullscreenOutput final : public QueuedOutput {
 public:
  FullscreenOutput(GpuDevice& gpu, int monitorIndex);
  ~FullscreenOutput() override { stop(); }
  bool needsCpu() const override { return false; }

 protected:
  void open() override;
  void deliver(const OutFrame& frame) override;
  void close() noexcept override;
  std::string errorCode() const override { return "fullscreen-failed"; }

 private:
  GpuDevice& gpu_;
  int monitor_;
  HWND hwnd_ = nullptr;
  UINT width_ = 0, height_ = 0;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
};
}  // namespace bc
