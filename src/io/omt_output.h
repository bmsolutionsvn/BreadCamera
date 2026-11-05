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
// camera does no readback. While nobody is connected it asks the camera for no picture at all:
// no readback, no texture, no pack, no encode (OMT-HX drops the frame on the wire side).
class OmthOutput final : public QueuedOutput {
 public:
  OmthOutput(std::string name, omthx::Mode mode, omthx::Codec codec = omthx::Codec::Hevc, bool keyed = false,
             omthx::Quality quality = omthx::Quality::Medium);
  ~OmthOutput() override { stop(); }
  // Full (plain OMT), a keyed output (it needs the alpha) and a refused GPU path read back — but only
  // while a receiver is connected.
  bool needsCpu() const override { return receivers_.load() && !gpuPath(); }
  bool wantsGpuRgba8() const override { return receivers_.load() && gpuPath(); }
  // An unkeyed plain OMT output sends the camera's UYVY readback: libomt takes it with no conversion,
  // and the wire stays standard VMX (spec §3.1 item 2). OMT-HX tags every picture BT.709, so SD, whose
  // UYVY is BT.601, keeps BGRA.
  bool wantsUyvy(uint32_t, uint32_t h) const override { return mode_ == omthx::Mode::Full && !keyed_ && h >= 720; }
  // Keyed or not: a coded frame keeps its alpha, so an output of the same mode passes it on.
  bool takesCodedOmt(omthx::Mode mode) const override { return mode == mode_; }
  bool takeKeyframeRequest() override { return keyframe_.exchange(false); }
  // A coded frame went over the wire as it came, with no encode (the sender's encoder is "none").
  bool sentCoded() const { return coded_.load(); }
  omthx::Quality quality() const { return quality_; }
  std::string address() const;  // "HOST (name)", for a receiver to connect to
  OutputStatus status() const override;

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "omt-failed"; }

 private:
  // The GPU path takes an unkeyed HX output's texture; everything else needs the CPU picture.
  bool gpuPath() const { return mode_ == omthx::Mode::Hx && !keyed_ && !gpuRefused_; }
  // The GPU path cannot take this frame or this device: from now on send through memory, once.
  void refuse(const std::string& why);

  std::string name_;
  omthx::Mode mode_;
  omthx::Codec codec_;
  bool keyed_;
  omthx::Quality quality_;
  std::unique_ptr<omthx::Sender> send_;
  std::vector<float> planar_;
  omthx::VideoFormat format_;
  std::atomic<bool> gpuRefused_{false};  // read by needsCpu() on the tick thread
  // send_->hasReceivers(), published by the worker at the end of deliver: the tick thread reads it to
  // skip the readback and the RGBA texture. The tick thread never touches send_.
  std::atomic<bool> receivers_{false};
  mutable std::mutex warnMutex_;         // guards warn_
  std::string warn_;
  mutable std::mutex addrMutex_;  // guards address_, set by the worker in open()
  std::string address_;
  std::atomic<bool> program_{false};  // the tally the sender reports, published by the worker
  std::atomic<bool> preview_{false};
  std::atomic<bool> keyframe_{false};  // a receiver wants a keyframe of the pass-through source
  std::atomic<bool> coded_{false};     // a coded frame was sent without encoding
};
}  // namespace bc
