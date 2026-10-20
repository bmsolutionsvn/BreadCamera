#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

#include <d3d11.h>
#include <wrl/client.h>

#include "gpu/device.h"

namespace bc {
// Per-stage GPU time from D3D11 timestamp queries (spec D14). A ring of four
// disjoint + two-timestamp sets, so up to four measurements are in flight and a
// poll never waits: the tick calls begin()/end() around a stage's GPU work and
// poll() once a tick, and a measurement comes back a tick or more late.
class GpuTimer {
 public:
  explicit GpuTimer(GpuDevice& gpu);  // throws std::runtime_error on query failure

  // Both take gpu.lock(). While all four sets are in flight, begin() and its end()
  // record nothing (the ring is full until a poll frees a finished set).
  void begin();
  void end();

  // The newest finished, non-disjoint measurement, returned once. Older finished
  // measurements are discarded; a still-running set is left pending. Never blocks:
  // nullopt when nothing is ready.
  std::optional<std::chrono::nanoseconds> poll();

 private:
  struct Set {
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
    Microsoft::WRL::ComPtr<ID3D11Query> begin;
    Microsoft::WRL::ComPtr<ID3D11Query> end;
  };
  static constexpr int kSets = 4;
  GpuDevice& gpu_;
  std::array<Set, kSets> sets_;
  std::array<int, kSets> pending_{};  // written, not-yet-read sets, oldest first
  int pendingCount_ = 0;
  int active_ = -1;  // the set between begin() and end(), -1 when none
};
}  // namespace bc