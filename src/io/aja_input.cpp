#include "io/aja_input.h"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <vector>

#include "core/backoff.h"
#include "io/uyvy.h"
#include "ntv2card.h"
#include "ntv2devicescanner.h"
#include "ntv2utils.h"

namespace bc {
namespace {
// libajantv2 18.0 keeps NTV2_AUDIOSIZE_MAX in the demo-only demos/ntv2democommon.h
// (`#define NTV2_AUDIOSIZE_MAX (401 * 1024)`, the per-frame audio space demos/ntv2capture
// reserves); the library headers do not export it. Same value, defined here.
constexpr size_t kNtv2AudioSizeMax = 401 * 1024;

std::optional<Rate> ajaRateFor(NTV2VideoFormat f) {
  switch (f) {
    case NTV2_FORMAT_1080p_2398: return Rate{24000, 1001};
    case NTV2_FORMAT_1080p_2400: return Rate{24, 1};
    case NTV2_FORMAT_1080p_2500: return Rate{25, 1};
    case NTV2_FORMAT_1080p_2997: return Rate{30000, 1001};
    case NTV2_FORMAT_1080p_3000: return Rate{30, 1};
    case NTV2_FORMAT_1080p_5000_A: return Rate{50, 1};
    case NTV2_FORMAT_1080p_5994_A: return Rate{60000, 1001};
    case NTV2_FORMAT_1080p_6000_A: return Rate{60, 1};
    default: return std::nullopt;
  }
}
}  // namespace

void AjaInput::start(InputSink& sink) {
  sink_ = &sink;
  stopping_ = false;
  thread_ = std::thread([this] { run(); });
}

void AjaInput::stop() {
  stopping_ = true;
  if (thread_.joinable()) thread_.join();
}

void AjaInput::run() {
  Backoff backoff;
  while (!stopping_) {
    sink_->onState(SignalState::Connecting, std::nullopt);
    bool clean = false;
    try {
      runOnce();
      clean = true;
    } catch (const std::exception& e) {
      if (!stopping_) sink_->onState(SignalState::Offline, ErrorInfo{"aja-failed", e.what()});
    }
    // A clean return means the capture ran (possibly until a healthy format change), so the
    // next reconnect must not inherit an escalated delay; only failures should back off.
    if (clean) backoff.reset();
    for (auto d = backoff.next(); !stopping_ && d.count() > 0; d -= std::chrono::milliseconds(100)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
  sink_->onState(SignalState::Offline, std::nullopt);
}

void AjaInput::runOnce() {
  CNTV2Card card;
  if (!CNTV2DeviceScanner::GetDeviceAtIndex(static_cast<ULWord>(index_), card)) {
    throw std::runtime_error("AJA device " + std::to_string(index_) + " not found");
  }
  const auto ch = static_cast<NTV2Channel>(channel_ - 1);
  const NTV2InputSource src = ::NTV2ChannelToInputSource(ch);
  card.EnableChannel(ch);
  card.SetSDITransmitEnable(ch, false);
  const NTV2VideoFormat vf = card.GetInputVideoFormat(src);
  if (vf == NTV2_FORMAT_UNKNOWN) throw std::runtime_error("no signal on AJA SDI input " + std::to_string(channel_));
  const auto rate = ajaRateFor(vf);
  card.SetVideoFormat(vf, false, false, ch);
  card.SetFrameBufferFormat(ch, NTV2_FBF_8BIT_YCBCR);
  card.SetMode(ch, NTV2_MODE_CAPTURE);
  card.Connect(::GetFrameBufferInputXptFromChannel(ch), ::GetSDIInputOutputXptFromChannel(ch));
  const NTV2AudioSystem as = ::NTV2ChannelToAudioSystem(ch);
  card.SetAudioSystemInputSource(as, NTV2_AUDIO_EMBEDDED, ::NTV2ChannelToEmbeddedAudioInput(ch));
  card.SetNumberAudioChannels(8, as);
  card.SetAudioRate(NTV2_AUDIO_48K, as);
  card.AutoCirculateStop(ch);
  if (!card.AutoCirculateInitForInput(ch, 7, as) || !card.AutoCirculateStart(ch)) {
    throw std::runtime_error("AJA AutoCirculate could not start on channel " + std::to_string(channel_));
  }
  const NTV2FormatDescriptor fd(vf, NTV2_FBF_8BIT_YCBCR);
  const int w = static_cast<int>(fd.GetRasterWidth()), h = static_cast<int>(fd.GetRasterHeight());
  const int rowBytes = static_cast<int>(fd.GetBytesPerRow());
  std::vector<ULWord> video(static_cast<size_t>(rowBytes) * h / 4);
  std::vector<ULWord> audio(kNtv2AudioSizeMax / 4);
  AUTOCIRCULATE_TRANSFER xfer;
  xfer.SetVideoBuffer(video.data(), static_cast<ULWord>(video.size() * 4));
  xfer.SetAudioBuffer(audio.data(), static_cast<ULWord>(audio.size() * 4));
  bool live = false;
  while (!stopping_) {
    AUTOCIRCULATE_STATUS st;
    card.AutoCirculateGetStatus(ch, st);
    if (!(st.IsRunning() && st.HasAvailableInputFrame())) {
      card.WaitForInputVerticalInterrupt(ch);
      if (card.GetInputVideoFormat(src) != vf) break;  // format change: re-init from the top
      continue;
    }
    card.AutoCirculateTransfer(ch, xfer);
    auto bgra = uyvyToBgra(reinterpret_cast<const uint8_t*>(video.data()), w, h, rowBytes, h >= 720);
    VideoFrameIn f;
    f.layout = PixelLayout::BGRA8;
    f.fullRange = true;
    f.rate = rate;
    f.tex = pool_.acquire(static_cast<uint32_t>(w), static_cast<uint32_t>(h), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    {
      auto lock = gpu_.lock();
      gpu_.context()->UpdateSubresource(f.tex->tex.Get(), 0, nullptr, bgra.data(), static_cast<UINT>(w * 4), 0);
    }
    const ULWord bytes = xfer.GetCapturedAudioByteCount();
    const int frames = static_cast<int>(bytes / (8 * 4));  // 8 channels × int32
    if (frames > 0) {
      AudioBlock blk;
      blk.samples.resize(static_cast<size_t>(frames) * 2);
      const auto* s = reinterpret_cast<const int32_t*>(audio.data());
      for (int i = 0; i < frames; ++i) {
        blk.samples[static_cast<size_t>(i) * 2] = static_cast<float>(s[i * 8]) / 2147483648.0f;
        blk.samples[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(s[i * 8 + 1]) / 2147483648.0f;
      }
      sink_->onAudio(std::move(blk));
    }
    if (!live) {
      live = true;
      sink_->onState(SignalState::Live, std::nullopt);
    }
    sink_->onVideo(std::move(f));
  }
  card.AutoCirculateStop(ch);
}
}  // namespace bc