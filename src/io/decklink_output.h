#pragma once
#include <vector>

#include "core/rate.h"
#include "io/queued_output.h"

struct IDeckLink;
struct IDeckLinkOutput;
struct IDeckLinkKeyer;
struct IDeckLinkMutableVideoFrame;
struct SwsContext;

namespace bc {
// DeckLink SDI/HDMI playout; keyed outputs use the external keyer (fill + key, spec §4.3).
class DeckLinkOutput final : public QueuedOutput {
 public:
  DeckLinkOutput(int deviceIndex, Rate showRate, bool keyed) : index_(deviceIndex), rate_(showRate), keyed_(keyed) {}
  ~DeckLinkOutput() override { stop(); }
  bool needsCpu() const override { return true; }
  // Keyed outputs keep BGRA; the card runs a fixed 1080p mode, so only that size takes UYVY.
  bool wantsUyvy(uint32_t w, uint32_t h) const override { return !keyed_ && w == 1920 && h == 1080; }

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
  IDeckLinkMutableVideoFrame* yuvFrames_[3] = {};
  int yuvNext_ = 0;
  SwsContext* sws_ = nullptr;
  std::vector<int16_t> pcm_;
};
}  // namespace bc