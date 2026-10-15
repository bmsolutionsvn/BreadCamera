#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "io/queued_output.h"
#include "omthx/omthx.h"

namespace bc {
// Open Media Transport sender (spec A4.3), plain OMT or OMT-HX, alpha when the frame has it.
class OmthOutput final : public QueuedOutput {
 public:
  OmthOutput(std::string name, omthx::Mode mode, omthx::Codec codec = omthx::Codec::Hevc);
  ~OmthOutput() override { stop(); }
  bool needsCpu() const override { return true; }
  std::string address() const;  // "HOST (name)", for a receiver to connect to
  OutputStatus status() const override;

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "omt-failed"; }

 private:
  std::string name_;
  omthx::Mode mode_;
  omthx::Codec codec_;
  std::unique_ptr<omthx::Sender> send_;
  std::vector<float> planar_;
  omthx::VideoFormat format_;
  mutable std::mutex addrMutex_;  // guards address_, set by the worker in open()
  std::string address_;
  std::atomic<bool> program_{false};  // the tally the sender reports, published by the worker
  std::atomic<bool> preview_{false};
};
}  // namespace bc
