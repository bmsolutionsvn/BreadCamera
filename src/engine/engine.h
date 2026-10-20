#pragma once
#include <atomic>
#include <condition_variable>
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
#include "engine/gpu_tools.h"
#include "engine/matte.h"
#include "gpu/compositor.h"
#include "gpu/converter.h"

namespace bc {
struct EngineStats {
  uint64_t overruns = 0;
  std::map<std::string, CameraStats> cameras;
};

// One camera's frame handed to the frame tap: its exported texture plus the shared CPU image
// produced by the tick's one readback (null when no output needed CPU and no preview wanted it).
struct FrameTap {
  std::string id;
  uint64_t tick = 0;
  TextureRef tex;
  std::shared_ptr<const CpuImage> cpu;
};

// Arrival-driven camera set. apply() is thread-safe; adapters are torn down outside the pass
// lock so a slow stop never stalls the pass.
class Engine {
 public:
  using FrameTapFn = std::function<void(uint64_t tick, const std::vector<FrameTap>& frames)>;
  Engine(GpuTools tools, StateStore& state, AdapterFactory& factory);
  ~Engine();
  void apply(const Show& show);
  void start();
  void stop();
  EngineStats stats() const;
  void setFrameTap(FrameTapFn tap) { tap_ = std::move(tap); }
  // Consulted before each camera's second half: true when a visible preview wants this camera,
  // so the tick's one readback runs even with no CPU output (D5). Call before start().
  void setPreviewWanted(std::function<bool(const std::string&)> wanted) { previewWanted_ = std::move(wanted); }
  // The matte service; call before start(). A null backend with a reason bypasses every matte.
  void setMatteBackend(std::unique_ptr<MatteBackend> backend, std::string unavailableReason);
  // Captures each camera's frame *before* any stage runs (spec: "from the frame before any
  // stage"), for the colour-match calibration service. Never call from the tick thread.
  std::map<std::string, std::vector<CpuImage>> capture(const std::vector<std::string>& cameraIds, int frames = 30,
                                                       int everyTicks = 2, uint32_t w = 256, uint32_t h = 144,
                                                       std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

 private:
  struct CaptureJob {
    std::vector<std::string> ids;
    int frames, every;
    uint32_t w, h;
    std::map<std::string, std::vector<CpuImage>> got;
    std::string error;
    bool done = false;
  };

  void run();
  std::vector<FrameTap> processReady(const std::vector<Camera*>& ready, std::chrono::steady_clock::time_point now);
  void loadSlate();
  SyncPolicy policy() const { return {std::chrono::milliseconds(250), std::chrono::milliseconds(settings_.holdMs)}; }

  GpuTools tools_;
  StateStore& state_;
  AdapterFactory& factory_;
  std::unique_ptr<MatteBackend> matte_;
  std::string matteUnavailable_;
  mutable std::mutex mutex_;
  std::shared_ptr<CaptureJob> capture_;  // guarded by mutex_
  std::condition_variable captureCv_;
  ShowSettings settings_;
  bool settingsApplied_ = false;
  TextureRef slate_;  // slate *source*; each camera fits it to its own size when it has no picture
  std::vector<std::unique_ptr<Camera>> cameras_;
  std::map<std::string, std::string> lastPhase_;
  FrameTapFn tap_;
  std::function<bool(const std::string&)> previewWanted_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> overruns_{0};
  std::atomic<bool> wake_{false};
  std::mutex wakeMutex_;
  std::condition_variable wakeCv_;
  std::thread thread_;
};
}  // namespace bc
