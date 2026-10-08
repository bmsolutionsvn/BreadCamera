#pragma once
#include <string>
#include <vector>

#include "io/queued_output.h"

struct NDIlib_send_instance_type;

namespace bc {
// NDI sender, one per camera output (spec §4.3). Needs the CPU readback.
class NdiOutput final : public QueuedOutput {
 public:
  explicit NdiOutput(std::string sourceName);
  ~NdiOutput() override { stop(); }
  bool needsCpu() const override { return true; }

 protected:
  void open() override;
  void deliver(const OutFrame& frame) override;
  void close() noexcept override;
  std::string errorCode() const override { return "ndi-failed"; }

 private:
  std::string name_;
  NDIlib_send_instance_type* send_ = nullptr;
  std::vector<float> planar_;
};
}  // namespace bc
