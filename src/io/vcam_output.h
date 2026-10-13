#pragma once
#include <string>
#include <vector>

#include "core/vcam_shared.h"
#include "io/queued_output.h"

struct IMFVirtualCamera;
struct SwsContext;

namespace bc {
// Windows 11 virtual camera N (1..4): registers it with MFCreateVirtualCamera and writes frames
// into the shared section the media source creates (spec §4.3). No alpha.
class VirtualCameraOutput final : public QueuedOutput {
 public:
  VirtualCameraOutput(int index, std::string friendlyName) : index_(index), name_(std::move(friendlyName)) {}
  ~VirtualCameraOutput() override { stop(); }
  bool needsCpu() const override { return true; }
  static bool installed();  // Windows 11 + HKLM registration of the DLL

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "vcam-failed"; }

 private:
  int index_;
  std::string name_;
  IMFVirtualCamera* cam_ = nullptr;
  vcam::Writer writer_;
  uint64_t frames_ = 0;
  SwsContext* sws_ = nullptr;
  std::vector<uint8_t> scaled_;
};
}  // namespace bc