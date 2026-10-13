#pragma once
#include <vector>

#include "core/rate.h"
#include "io/queued_output.h"

struct IDeckLink;
struct IDeckLinkOutput;
struct IDeckLinkKeyer;
struct SwsContext;

namespace bc {
// DeckLink SDI/HDMI playout; keyed outputs use the external keyer (fill + key, spec §4.3).
class DeckLinkOutput final : public QueuedOutput {
 public:
  DeckLinkOutput(int deviceIndex, Rate showRate, bool keyed) : index_(deviceIndex), rate_(showRate), keyed_(keyed) {}
  ~DeckLinkOutput() override { stop(); }
  bool needsCpu() const override { return true; }

 protected:
  void open() override;
  void deliver(const OutFrame& f) override;
  void close() noexcept override;
  std::string errorCode() const override { return "decklink-failed"; }

 private:
  int index_;
  Rate rate_;
  bool keyed_;
  IDeckLink* device_ = nullptr;
  IDeckLinkOutput* output_ = nullptr;
  IDeckLinkKeyer* keyer_ = nullptr;
  SwsContext* sws_ = nullptr;
  std::vector<int16_t> pcm_;
};
}  // namespace bc