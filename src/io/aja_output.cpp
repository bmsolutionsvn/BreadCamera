#include "io/aja_output.h"

extern "C" {
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <stdexcept>

#include "io/uyvy.h"
#include "ntv2card.h"
#include "ntv2devicescanner.h"
#include "ntv2utils.h"

namespace bc {
namespace {
NTV2VideoFormat formatFor(Rate r) {
  if (r == Rate{24000, 1001}) return NTV2_FORMAT_1080p_2398;
  if (r == Rate{24, 1}) return NTV2_FORMAT_1080p_2400;
  if (r == Rate{25, 1}) return NTV2_FORMAT_1080p_2500;
  if (r == Rate{30000, 1001}) return NTV2_FORMAT_1080p_2997;
  if (r == Rate{30, 1}) return NTV2_FORMAT_1080p_3000;
  if (r == Rate{50, 1}) return NTV2_FORMAT_1080p_5000_A;
  if (r == Rate{60000, 1001}) return NTV2_FORMAT_1080p_5994_A;
  if (r == Rate{60, 1}) return NTV2_FORMAT_1080p_6000_A;
  throw std::invalid_argument("AJA SDI does not support " + r.str());
}
}  // namespace

AjaOutput::AjaOutput(int deviceIndex, int channel, Rate showRate) : index_(deviceIndex), channel_(channel), rate_(showRate) {}
AjaOutput::~AjaOutput() { stop(); }

void AjaOutput::open() {
  card_ = std::make_unique<CNTV2Card>();
  if (!CNTV2DeviceScanner::GetDeviceAtIndex(static_cast<ULWord>(index_), *card_)) {
    throw std::runtime_error("AJA device " + std::to_string(index_) + " not found");
  }
  const auto ch = static_cast<NTV2Channel>(channel_ - 1);
  card_->EnableChannel(ch);
  card_->SetVideoFormat(formatFor(rate_), false, false, ch);
  card_->SetFrameBufferFormat(ch, NTV2_FBF_8BIT_YCBCR);
  card_->SetMode(ch, NTV2_MODE_DISPLAY);
  card_->SetSDITransmitEnable(ch, true);
  card_->Connect(::GetSDIOutputInputXpt(ch), ::GetFrameBufferOutputXptFromChannel(ch));
  const NTV2AudioSystem as = ::NTV2ChannelToAudioSystem(ch);
  card_->SetNumberAudioChannels(8, as);
  card_->SetAudioRate(NTV2_AUDIO_48K, as);
  card_->AutoCirculateStop(ch);
  if (!card_->AutoCirculateInitForOutput(ch, 7, as) || !card_->AutoCirculateStart(ch)) {
    throw std::runtime_error("AJA AutoCirculate could not start on output " + std::to_string(channel_));
  }
}

void AjaOutput::deliver(const OutFrame& f) {
  const auto ch = static_cast<NTV2Channel>(channel_ - 1);
  AUTOCIRCULATE_STATUS st;
  card_->AutoCirculateGetStatus(ch, st);
  if (!st.CanAcceptMoreOutputFrames()) return;  // drop rather than block (spec §4.1)
  constexpr int W = 1920, H = 1080;
  const uint8_t* video = nullptr;
  if (f.uyvy && !f.uyvy->bgra.empty()) {
    // Already 4:2:2 from the GPU: hand the buffer on, no conversion and no copy. Hold the image
    // so nothing frees it until this delivery (and any deferred DMA) is done with it.
    lastUyvy_ = f.uyvy;
    video = f.uyvy->bgra.data();
  } else if (f.bgra && !f.bgra->bgra.empty()) {
    lastUyvy_.reset();
    const uint8_t* bgra = f.bgra->bgra.data();
    if (static_cast<int>(f.bgra->width) != W || static_cast<int>(f.bgra->height) != H) {
      scaled_.resize(static_cast<size_t>(W) * H * 4);
      sws_ = sws_getCachedContext(sws_, static_cast<int>(f.bgra->width), static_cast<int>(f.bgra->height), AV_PIX_FMT_BGRA, W, H,
                                  AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!sws_) throw std::runtime_error("cannot create a pixel converter for the AJA frame");
      const uint8_t* src[1] = {bgra};
      const int ss[1] = {static_cast<int>(f.bgra->width * 4)};
      uint8_t* dst[1] = {scaled_.data()};
      const int ds[1] = {W * 4};
      sws_scale(sws_, src, ss, 0, static_cast<int>(f.bgra->height), dst, ds);
      bgra = scaled_.data();
    }
    uyvy_.resize(static_cast<size_t>(W) * H * 2);
    bgraToUyvy(bgra, W, H, uyvy_.data(), W * 2, true);
    video = uyvy_.data();
  } else {
    return;
  }
  AUTOCIRCULATE_TRANSFER xfer;
  xfer.SetVideoBuffer(reinterpret_cast<ULWord*>(const_cast<uint8_t*>(video)),
                      static_cast<ULWord>(static_cast<size_t>(W) * H * 2));
  if (f.audio && !f.audio->empty()) {
    const size_t frames = f.audio->size() / 2;
    pcm_.assign(frames * 8, 0);
    for (size_t i = 0; i < frames; ++i) {
      pcm_[i * 8] = static_cast<int32_t>(std::clamp((*f.audio)[i * 2], -1.0f, 1.0f) * 2147483647.0f);
      pcm_[i * 8 + 1] = static_cast<int32_t>(std::clamp((*f.audio)[i * 2 + 1], -1.0f, 1.0f) * 2147483647.0f);
    }
    xfer.SetAudioBuffer(reinterpret_cast<ULWord*>(pcm_.data()), static_cast<ULWord>(pcm_.size() * 4));
  }
  if (!card_->AutoCirculateTransfer(ch, xfer)) throw std::runtime_error("the AJA output stopped accepting frames");
}

void AjaOutput::close() noexcept {
  sws_freeContext(sws_);
  sws_ = nullptr;
  if (card_) {
    card_->AutoCirculateStop(static_cast<NTV2Channel>(channel_ - 1));
    card_.reset();
  }
}
}  // namespace bc