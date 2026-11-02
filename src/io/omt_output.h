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
// An unkeyed HX output sends the frame's texture straight to the encoder (spec §4.2), so its
// camera does no readback.
class OmthOutput final : public QueuedOutput {
 public:
  OmthOutput(std::string name, omthx::Mode mode, omthx::Codec codec = omthx::Codec::Hevc, bool keyed = false);
  ~OmthOutput() override { stop(); }
  // Full (plain OMT), a keyed output (it needs the alpha) and a refused GPU path read back.
  bool needsCpu() const override { return mode_ == omthx::Mode::Full || keyed_ || gpuRefused_; }
  bool wantsGpuRgba8() const override { return !needsCpu(); }
  std::string address() const;  // "HOST (name)", for a receiver to connect to
  OutputStatus status() const override;

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "omt-failed"; }

 private:
  // The GPU path cannot take this frame or this device: from now on send through memory, once.
  void refuse(const std::string& why);

  std::string name_;
  omthx::Mode mode_;
  omthx::Codec codec_;
  bool keyed_;
  std::unique_ptr<omthx::Sender> send_;
  std::vector<float> planar_;
  omthx::VideoFormat format_;
  std::atomic<bool> gpuRefused_{false};  // read by needsCpu() on the tick thread
  mutable std::mutex warnMutex_;         // guards warn_
  std::string warn_;
  mutable std::mutex addrMutex_;  // guards address_, set by the worker in open()
  std::string address_;
  std::atomic<bool> program_{false};  // the tally the sender reports, published by the worker
  std::atomic<bool> preview_{false};
};
}  // namespace bc
