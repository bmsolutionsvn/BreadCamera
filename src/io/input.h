#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/rate.h"
#include "core/signal_state.h"
#include "gpu/converter.h"
#include "gpu/texture_pool.h"

namespace bc {
struct VideoFrameIn {
  TextureRef tex;  // NV12/P010 (planar SRVs) or BGRA8 (srv)
  PixelLayout layout = PixelLayout::NV12;
  bool fullRange = false;
  bool bt709 = false;  // force BT.709 for YUV at any height (OMT-HX's NV12)
  int64_t ptsUs = 0;
  std::optional<Rate> rate;  // the source's declared nominal rate; empty when unknown
};

struct AudioBlock {
  int sampleRate = 48000;
  int channels = 2;
  std::vector<float> samples;  // interleaved
};

// A picture an input already holds in CPU memory, for a recorder that encodes from it (the
// BreadReplay input tap). Valid only during the onCpuVideo call; the sink must copy what it keeps.
enum class CpuPixel { UYVY, NV12, BGRA };
struct CpuVideoFrame {
  const uint8_t* data = nullptr;  // NV12: chroma plane starts at data + stride * height
  int stride = 0;
  uint32_t width = 0, height = 0;
  CpuPixel format = CpuPixel::UYVY;
  bool fullRange = false;
  int64_t ptsUs = 0;
  std::optional<Rate> rate;
};

// A coded picture as the source sent it, for a recorder that stores it without re-encoding
// (OMT's full VMX frames). Valid only during the onCodedVideo call.
struct CodedVideoFrame {
  std::span<const uint8_t> data;
  std::string codec;
  uint32_t width = 0, height = 0;
  int64_t ptsUs = 0;
};

// Called from the input's own thread.
class InputSink {
 public:
  virtual ~InputSink() = default;
  virtual void onVideo(VideoFrameIn frame) = 0;
  virtual void onAudio(AudioBlock audio) = 0;
  virtual void onState(SignalState state, std::optional<ErrorInfo> error) = 0;
  // Defaults keep BreadCamera's Camera, which wants only GPU pictures, unchanged.
  virtual bool wantsCpuVideo() const { return false; }
  virtual bool wantsGpuVideo() const { return true; }
  virtual void onCpuVideo(const CpuVideoFrame&) {}
  virtual bool wantsCodedVmx() const { return false; }
  virtual void onCodedVideo(const CodedVideoFrame&) {}
};

// spec §4.1: every adapter runs its own thread; start()/stop() never block for long.
class Input {
 public:
  virtual ~Input() = default;
  virtual void start(InputSink& sink) = 0;
  virtual void stop() = 0;
};
}  // namespace bc
