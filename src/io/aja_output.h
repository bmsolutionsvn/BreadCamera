#pragma once
#include <memory>
#include <vector>

#include "core/rate.h"
#include "io/queued_output.h"

class CNTV2Card;

namespace bc {
// AJA SDI playout, fill only (spec §4.3 deviation, see follow-ups). EXPERIMENTAL.
class AjaOutput final : public QueuedOutput {
 public:
  AjaOutput(int deviceIndex, int channel, Rate showRate);
  ~AjaOutput() override;
  bool needsCpu() const override { return true; }

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "aja-failed"; }

 private:
  int index_, channel_;
  Rate rate_;
  std::unique_ptr<CNTV2Card> card_;
  std::vector<uint8_t> uyvy_, scaled_;
  std::vector<int32_t> pcm_;
};
}  // namespace bc