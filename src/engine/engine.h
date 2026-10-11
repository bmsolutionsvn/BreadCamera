#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/show.h"
#include "core/state_store.h"
#include "engine/adapter_factory.h"
#include "engine/camera.h"
#include "engine/matte.h"
#include "gpu/compositor.h"
#include "gpu/converter.h"

namespace bc {
struct EngineStats {
  uint64_t overruns = 0;
  std::map<std::string, CameraStats> cameras;
};

// Master tick + camera set (spec §3.3). apply() is thread-safe; adapters are torn down
// outside the tick lock so a slow stop never stalls the tick.
class Engine {
 public:
  using FrameTap = std::function<void(uint64_t tick, const std::vector<std::pair<std::string, TextureRef>>& frames)>;
  Engine(Converter& conv, Compositor& comp, StateStore& state, AdapterFactory& factory);
  ~Engine();
  void apply(const Show& show);
  void start();
  void stop();
  EngineStats stats() const;
  void setFrameTap(FrameTap tap) { tap_ = std::move(tap); }
  // The matte service; call before start(). A null backend with a reason bypasses every matte.
  void setMatteBackend(std::unique_ptr<MatteBackend> backend, std::string unavailableReason);

 private:
  void run();
  void loadSlate();
  SyncPolicy policy() const { return {std::chrono::milliseconds(250), std::chrono::milliseconds(settings_.holdMs)}; }

  Converter& conv_;
  Compositor& comp_;
  StateStore& state_;
  AdapterFactory& factory_;
  std::unique_ptr<MatteBackend> matte_;
  std::string matteUnavailable_;
  mutable std::mutex mutex_;
  ShowSettings settings_;
  bool settingsApplied_ = false;
  bool rateChanged_ = true;
  TextureRef slate_;
  std::vector<std::unique_ptr<Camera>> cameras_;
  std::map<std::string, std::string> lastPhase_;
  FrameTap tap_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> overruns_{0};
  std::thread thread_;
};
}  // namespace bc
