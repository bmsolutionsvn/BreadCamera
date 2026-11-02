#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "gpu/surface_presenter.h"

namespace bc {
class Converter;
class GpuDevice;
struct GpuTexture;

struct SurfaceRequest {
  std::string id;
  uint32_t w = 0, h = 0;
};
struct SurfaceGrant {
  std::string id;
  uint64_t handle = 0;
  uint32_t w = 0, h = 0;
};

// Which client wants which surface, at which size (spec §4.2). Mirrors PreviewHub: the set is per
// (client, id), so a restarted shell gets new surfaces and a stale page's die with its socket.
// subscribe/dropClient run on the control-server thread, present on the tick thread; one mutex
// guards the map and is held while presenting (lock order: hub mutex, then GpuDevice::lock()).
class SurfaceHub {
 public:
  SurfaceHub(GpuDevice& gpu, Converter& conv);
  std::vector<SurfaceGrant> subscribe(const std::string& client, const std::vector<SurfaceRequest>& wanted);  // replaces the set
  void dropClient(const std::string& client);
  std::set<std::string> wanted() const;
  void present(const std::string& id, const GpuTexture& linear);  // every presenter for id; >=15 ms apart per presenter
  bool anyBroken() const;
  std::set<std::string> brokenIds() const;  // ids with at least one broken presenter
  size_t presenterCount() const;  // tests

 private:
  struct Entry {
    std::unique_ptr<SurfacePresenter> presenter;
    std::chrono::steady_clock::time_point nextDue{};
  };
  GpuDevice& gpu_;
  Converter& conv_;
  mutable std::mutex m_;
  std::map<std::string, std::map<std::string, Entry>> byClient_;
};
}  // namespace bc